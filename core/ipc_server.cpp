// ipc_server.cpp - 命名管道 IPC 服务端实现
// danmaku_overlay - 高性能 mpv 弹幕叠加渲染器
//
// IPC 协议 (行分隔 JSON, lua -> exe):
//   {"type":"load_ass","filepath":"..."}
//   {"type":"sync","time_pos":12.34,"duration":300,"paused":false,"speed":1,"seeking":false,"osd_width":1920,"osd_height":1080}
//   {"type":"set_delay","delay":0.5}
//   {"type":"set_enabled","enabled":true}
//   {"type":"set_hdr","enabled":true}
//   {"type":"set_hdr_peak","hdr_peak":203}
//   {"type":"set_config","opacity":0.8,"font_scale":1.0,"max_danmaku":200}
//   {"type":"clear_danmaku"} / {"type":"shutdown"} / {"type":"ping"}
// 响应 (exe -> lua): {"type":"pong"} / {"type":"ready"} / {"type":"error","message":"..."}

#include "ipc_server.h"
#include "render/danmaku_engine.h"
#include "render/layered_presenter.h"
#include "common/log.h"

#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace danmaku_overlay {

// ============================================================
// 极简 JSON 解析器 (无第三方依赖, 仅支持本协议使用的简单结构)
// ============================================================
namespace json {

    static const char* skip_ws(const char* p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        return p;
    }

    static std::string parse_string(const char*& p) {
        std::string result;
        if (*p != '"') return result;
        ++p;
        while (*p && *p != '"') {
            if (*p == '\\') {
                ++p;
                switch (*p) {
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                case '\\': result += '\\'; break;
                case '"':  result += '"';  break;
                case '/':  result += '/';  break;
                case 'u': {
                    if (p[1] && p[2] && p[3] && p[4]) {
                        char hex[5] = {p[1], p[2], p[3], p[4], 0};
                        unsigned int cp = static_cast<unsigned int>(strtoul(hex, nullptr, 16));
                        p += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                            char hex2[5] = {p[3], p[4], p[5], p[6], 0};
                            unsigned int cp2 = static_cast<unsigned int>(strtoul(hex2, nullptr, 16));
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (cp2 - 0xDC00);
                            p += 6;
                        }
                        if (cp <= 0x7F) {
                            result += static_cast<char>(cp);
                        } else if (cp <= 0x7FF) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp <= 0xFFFF) {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xF0 | (cp >> 18));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: result += *p; break;
                }
                ++p;
            } else {
                result += *p;
                ++p;
            }
        }
        if (*p == '"') ++p;
        return result;
    }

    static double parse_number(const char*& p) {
        char* end = nullptr;
        double val = strtod(p, &end);
        p = end;
        return val;
    }

    static bool parse_bool(const char*& p) {
        if (strncmp(p, "true", 4) == 0) { p += 4; return true; }
        if (strncmp(p, "false", 5) == 0) { p += 5; return false; }
        return false;
    }

    // 正确区分字符串内外: 弹幕文本 (字符串值) 内含 "type" 等键名子串时不被误判为键
    static const char* find_key(const char* json, const char* key) {
        std::string search = "\"";
        search += key;
        search += "\"";
        const size_t key_len = search.size();
        const char* p = json;
        bool in_string = false;

        while (*p) {
            const char c = *p;
            if (in_string) {
                if (c == '\\') { ++p; if (*p) ++p; continue; }
                if (c == '"')  in_string = false;
                ++p;
                continue;
            }
            if (c == '"') {
                if (strncmp(p, search.c_str(), key_len) == 0) {
                    p += key_len;
                    p = skip_ws(p);
                    if (*p == ':') { ++p; return skip_ws(p); }
                    continue;
                }
                in_string = true;
                ++p;
                continue;
            }
            ++p;
        }
        return nullptr;
    }

    static std::string get_string(const char* json, const char* key) {
        const char* p = find_key(json, key);
        if (!p || *p != '"') return "";
        return parse_string(p);
    }
    static double get_number(const char* json, const char* key) {
        const char* p = find_key(json, key);
        if (!p) return 0.0;
        return parse_number(p);
    }
    static bool get_bool(const char* json, const char* key) {
        const char* p = find_key(json, key);
        if (!p) return false;
        return parse_bool(p);
    }
    static int get_int(const char* json, const char* key) {
        return static_cast<int>(get_number(json, key));
    }

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x",
                             static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

} // namespace json

// ============================================================
// IpcServer 实现
// ============================================================

IpcServer::IpcServer() = default;

IpcServer::~IpcServer() {
    shutdown();
}

ErrorCode IpcServer::initialize(const wchar_t* pipe_name,
                                DanmakuEngine* engine,
                                WindowManager* window) {
    if (!pipe_name || !engine || !window) {
        DANMAKU_LOG_ERROR(LOG_TAG, "IpcServer::initialize 参数无效");
        return ErrorCode::InvalidArgument;
    }
    wcscpy_s(pipe_name_, 256, pipe_name);
    engine_ = engine;
    window_ = window;
    DANMAKU_LOG_INFO(LOG_TAG, "IpcServer 初始化, 管道: %ls", pipe_name_);
    return ErrorCode::Ok;
}

void IpcServer::start_listening() {
    running_.store(true, std::memory_order_release);
    listen_thread_ = std::thread(&IpcServer::listen_thread_main, this);
}

void IpcServer::shutdown() {
    // 幂等: 显式调用与析构二次调用只应完整清理并日志一次
    // (否则 "IpcServer 已关闭" 会被打印两次)。
    if (shutdown_done_.exchange(true, std::memory_order_acq_rel)) return;

    // 即使 running_ 已被 IPC shutdown 提前置 false, 也必须完整清理
    // (关闭管道 + join 线程), 否则监听/读取线程泄漏, 进程可能卡在
    // ConnectNamedPipe 形成僵尸。joinable()/INVALID 句柄守卫保证重复调用安全。
    running_.store(false, std::memory_order_release);

    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(pipe_handle_);
        DisconnectNamedPipe(pipe_handle_);
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }

    client_connected_.store(false, std::memory_order_release);

    if (listen_thread_.joinable()) listen_thread_.join();
    if (read_thread_.joinable())   read_thread_.join();

    DANMAKU_LOG_INFO(LOG_TAG, "IpcServer 已关闭");
}

// 监听线程: 创建命名管道并等待连接
void IpcServer::listen_thread_main() {
    DANMAKU_LOG_INFO(LOG_TAG, "IPC 监听线程启动");

    while (running_.load(std::memory_order_acquire)) {
        pipe_handle_ = CreateNamedPipeW(
            pipe_name_,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, nullptr);

        if (pipe_handle_ == INVALID_HANDLE_VALUE) {
            DANMAKU_LOG_ERROR(LOG_TAG, "CreateNamedPipe 失败 (GLE=%lu)", GetLastError());
            Sleep(1000);
            continue;
        }

        DANMAKU_LOG_INFO(LOG_TAG, "等待 lua 客户端连接...");

        BOOL connected = ConnectNamedPipe(pipe_handle_, nullptr);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_PIPE_LISTENING) {
                continue;
            } else {
                DANMAKU_LOG_ERROR(LOG_TAG, "ConnectNamedPipe 失败 (GLE=%lu)", err);
                CloseHandle(pipe_handle_);
                pipe_handle_ = INVALID_HANDLE_VALUE;
                continue;
            }
        }

        if (!running_.load(std::memory_order_acquire)) {
            CloseHandle(pipe_handle_);
            pipe_handle_ = INVALID_HANDLE_VALUE;
            break;
        }

        client_connected_.store(true, std::memory_order_release);
        DANMAKU_LOG_INFO(LOG_TAG, "lua 客户端已连接");

        send_message(IpcMessageType::Ready, "{}");
        DANMAKU_LOG_INFO(LOG_TAG, "ready 消息已发送, 启动读取线程");

        if (read_thread_.joinable()) read_thread_.join();
        read_thread_ = std::thread(&IpcServer::read_thread_main, this);
        read_thread_.join();

        DisconnectNamedPipe(pipe_handle_);
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
        client_connected_.store(false, std::memory_order_release);

        DANMAKU_LOG_INFO(LOG_TAG, "客户端断开, 重新等待连接");
    }

    DANMAKU_LOG_INFO(LOG_TAG, "IPC 监听线程退出");
}

// 读取线程: 逐行读取 JSON 消息并处理
void IpcServer::read_thread_main() {
    char buffer[8192];
    recv_buffer_.clear();

    DANMAKU_LOG_INFO(LOG_TAG, "IPC 读取线程启动");

    while (running_.load(std::memory_order_acquire)
           && client_connected_.load(std::memory_order_acquire)) {
        DWORD bytes_available = 0;
        if (!PeekNamedPipe(pipe_handle_, nullptr, 0, nullptr,
                           &bytes_available, nullptr)) {
            DANMAKU_LOG_INFO(LOG_TAG, "管道断开 (PeekNamedPipe)");
            break;
        }

        if (bytes_available == 0) {
            Sleep(1);
            continue;
        }

        DWORD bytes_read = 0;
        BOOL ok = ReadFile(pipe_handle_, buffer,
                           (bytes_available < sizeof(buffer))
                               ? bytes_available : sizeof(buffer),
                           &bytes_read, nullptr);
        if (!ok || bytes_read == 0) {
            DANMAKU_LOG_INFO(LOG_TAG, "管道读取失败 (GLE=%lu)", GetLastError());
            break;
        }


        recv_buffer_.append(buffer, bytes_read);

        size_t pos = 0;
        while (true) {
            size_t nl = recv_buffer_.find('\n', pos);
            if (nl == std::string::npos) break;

            std::string line = recv_buffer_.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pos = nl + 1;

            if (line.empty()) continue;

            IpcMessage msg = parse_json_message(line);
            handle_message(msg);
        }

        if (pos > 0) recv_buffer_.erase(0, pos);
    }
}

IpcMessage IpcServer::parse_json_message(const std::string& line) {
    IpcMessage msg;
    std::string type_str = json::get_string(line.c_str(), "type");

    if (type_str == "load_ass")         msg.type = IpcMessageType::LoadAss;
    else if (type_str == "sync" || type_str == "update_state")
                                       msg.type = IpcMessageType::Sync;
    else if (type_str == "set_delay")    msg.type = IpcMessageType::SetDelay;
    else if (type_str == "set_enabled")  msg.type = IpcMessageType::SetEnabled;
    else if (type_str == "set_hdr")      msg.type = IpcMessageType::SetHdr;
    else if (type_str == "set_hdr_peak") msg.type = IpcMessageType::SetHdrPeak;
    else if (type_str == "update_window")msg.type = IpcMessageType::UpdateWindow;
    else if (type_str == "set_config")   msg.type = IpcMessageType::SetConfig;
    else if (type_str == "clear_danmaku")msg.type = IpcMessageType::ClearDanmaku;
    else if (type_str == "shutdown")     msg.type = IpcMessageType::Shutdown;
    else if (type_str == "ping")         msg.type = IpcMessageType::Ping;
    else {
        DANMAKU_LOG_WARN(LOG_TAG, "未知 IPC 消息类型: %s", type_str.c_str());
        msg.type = IpcMessageType::Ping;
    }

    msg.json_payload = line;
    return msg;
}

void IpcServer::handle_message(const IpcMessage& msg) {
    const char* json = msg.json_payload.c_str();

    switch (msg.type) {
    case IpcMessageType::Ping:
        send_message(IpcMessageType::Pong, "{}");
        break;

    case IpcMessageType::LoadAss: {
        std::string ass_path = parse_ass_filepath(json);
        DANMAKU_LOG_INFO(LOG_TAG, "LoadAss: filepath='%s', engine=%p",
                         ass_path.c_str(), (void*)engine_);
        if (engine_ && !ass_path.empty()) {
            DANMAKU_LOG_INFO(LOG_TAG, "调用 load_ass_file...");
            ErrorCode ec = ErrorCode::Ok;
            try {
                ec = engine_->load_ass_file(ass_path);
            } catch (const std::exception& e) {
                DANMAKU_LOG_ERROR(LOG_TAG, "load_ass_file 异常: %s", e.what());
                ec = ErrorCode::ParseError;
            } catch (...) {
                DANMAKU_LOG_ERROR(LOG_TAG, "load_ass_file 未知异常");
                ec = ErrorCode::ParseError;
            }
            DANMAKU_LOG_INFO(LOG_TAG, "load_ass_file 返回 ec=%d", static_cast<int>(ec));
            if (ec == ErrorCode::Ok) {
                DANMAKU_LOG_INFO(LOG_TAG, "ASS 加载成功: %s", ass_path.c_str());
                send_message(IpcMessageType::Ready, "{\"status\":\"loaded\"}");
            } else {
                DANMAKU_LOG_ERROR(LOG_TAG, "ASS 加载失败: %s (ec=%d)",
                                  ass_path.c_str(), static_cast<int>(ec));
                send_message(IpcMessageType::Error,
                    std::string("{\"error\":\"load_ass_failed\"}"));
            }
        }
        break;
    }

    case IpcMessageType::Sync: {
        // 事件驱动时间同步: bridge 不再 60Hz 持续上报, C++ 端自有 QPC 时钟,
        // 只在关键事件重设基准。seek 直接带 time-pos 同步一次; 引擎按时间
        // 是否跳变决定重建活跃集合 (跳变才重建且保留滚动中弹幕), 暂停只冻结时钟。
        IpcPlayState state = parse_play_state(json);
        if (engine_) engine_->sync(state.time_pos, state.speed, state.paused);
        break;
    }

    case IpcMessageType::SetDelay:
        parse_delay_config(json);
        break;

    case IpcMessageType::SetEnabled: {
        bool enabled = json::get_bool(json, "enabled");
        if (window_) window_->set_visible(enabled);
        if (engine_) engine_->set_enabled(enabled);
        DANMAKU_LOG_INFO(LOG_TAG, "弹幕渲染: %s", enabled ? "开启" : "关闭");
        break;
    }

    case IpcMessageType::SetHdr:
        parse_hdr_config(json);
        break;

    case IpcMessageType::SetHdrPeak:
        parse_hdr_peak_config(json);
        break;

    case IpcMessageType::UpdateWindow:
        parse_window_info(json);
        break;

    case IpcMessageType::SetConfig:
        parse_render_config(json);
        break;

    case IpcMessageType::ClearDanmaku:
        if (engine_) {
            engine_->clear();
            DANMAKU_LOG_INFO(LOG_TAG, "弹幕已清空");
        }
        break;

    case IpcMessageType::Shutdown:
        running_.store(false, std::memory_order_release);
        break;

    default:
        DANMAKU_LOG_WARN(LOG_TAG, "未处理的 IPC 消息类型: %d",
                         static_cast<int>(msg.type));
        break;
    }
}

std::string IpcServer::parse_ass_filepath(const std::string& json) {
    return json::get_string(json.c_str(), "filepath");
}

IpcPlayState IpcServer::parse_play_state(const std::string& json) {
    IpcPlayState s;
    s.time_pos   = json::get_number(json.c_str(), "time_pos");
    s.duration   = json::get_number(json.c_str(), "duration");
    s.paused      = json::get_bool(json.c_str(), "paused");
    s.speed       = json::get_number(json.c_str(), "speed");
    s.seeking     = json::get_bool(json.c_str(), "seeking");
    s.osd_width   = static_cast<uint32_t>(json::get_int(json.c_str(), "osd_width"));
    s.osd_height  = static_cast<uint32_t>(json::get_int(json.c_str(), "osd_height"));
    return s;
}

void IpcServer::parse_window_info(const std::string& json) {
    if (!window_) return;
    int x      = json::get_int(json.c_str(), "x");
    int y      = json::get_int(json.c_str(), "y");
    int width  = json::get_int(json.c_str(), "width");
    int height = json::get_int(json.c_str(), "height");
    int dpi    = json::get_int(json.c_str(), "dpi");
    window_->update_mpv_window_pos(x, y, width, height, dpi);
}

void IpcServer::parse_render_config(const std::string& json) {
    if (!engine_) return;
    double opacity     = json::get_number(json.c_str(), "opacity");
    double font_scale  = json::get_number(json.c_str(), "font_scale");
    int    max_danmaku = json::get_int(json.c_str(), "max_danmaku");
    engine_->apply_render_config(
        opacity > 0 ? static_cast<float>(opacity) : 0.0f,
        font_scale > 0 ? static_cast<float>(font_scale) : 0.0f,
        max_danmaku > 0 ? static_cast<uint32_t>(max_danmaku) : 0u);
}

void IpcServer::parse_delay_config(const std::string& json) {
    if (!engine_) return;
    double delay = json::get_number(json.c_str(), "delay");
    if (delay > 120.0) delay = 120.0;
    if (delay < -120.0) delay = -120.0;
    engine_->set_delay(delay);
    DANMAKU_LOG_INFO(LOG_TAG, "延迟调整: %.3f 秒", delay);
}

void IpcServer::parse_hdr_config(const std::string& json) {
    if (!engine_) return;
    bool enabled = json::get_bool(json.c_str(), "enabled");
    engine_->set_hdr_enabled(enabled);
    DANMAKU_LOG_INFO(LOG_TAG, "HDR 渲染: %s", enabled ? "开启" : "关闭");
}

void IpcServer::parse_hdr_peak_config(const std::string& json) {
    if (!engine_) return;
    double peak = json::get_number(json.c_str(), "hdr_peak");
    if (peak <= 0) peak = 203.0;
    engine_->set_hdr_peak(static_cast<float>(peak));
    DANMAKU_LOG_INFO(LOG_TAG, "HDR 峰值亮度: %.1f nits", peak);
}

std::string IpcServer::build_json_response(IpcMessageType type,
                                            const std::string& payload) {
    std::string type_str;
    switch (type) {
    case IpcMessageType::Pong:    type_str = "pong";     break;
    case IpcMessageType::Ready:   type_str = "ready";    break;
    case IpcMessageType::Error:   type_str = "error";    break;
    default:                      type_str = "unknown";  break;
    }

    std::ostringstream ss;
    if (type == IpcMessageType::Error && !payload.empty()) {
        ss << "{\"type\":\"" << type_str << "\",\"message\":\""
           << json::escape(payload) << "\"}";
    } else {
        ss << "{\"type\":\"" << type_str << "\""
           << (payload.empty() ? "" : ("," + payload))
           << "}";
    }
    return ss.str();
}

bool IpcServer::send_message(IpcMessageType type, const std::string& json_payload) {
    if (!client_connected_.load(std::memory_order_acquire) ||
        pipe_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::string msg = build_json_response(type, json_payload);
    msg += '\n';

    std::lock_guard<std::mutex> lock(write_mutex_);

    DWORD bytes_written = 0;
    BOOL ok = WriteFile(pipe_handle_, msg.data(),
                        static_cast<DWORD>(msg.size()),
                        &bytes_written, nullptr);
    if (!ok || bytes_written != msg.size()) {
        DANMAKU_LOG_WARN(LOG_TAG, "管道写入失败 (GLE=%lu)", GetLastError());
        return false;
    }

    // 不调用 FlushFileBuffers: lua bridge 端当前只写不读, 会阻塞监听线程
    return true;
}

} // namespace danmaku_overlay
