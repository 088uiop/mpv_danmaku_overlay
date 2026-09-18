#pragma once

// ipc_server.h - 命名管道 IPC 服务端
// danmaku_overlay - 高性能 mpv 弹幕叠加渲染器
// 职责: 创建命名管道服务端等待 lua 连接; 解析行分隔 JSON 协议;
//       将弹幕数据与播放状态分发到 DanmakuEngine, 窗口跟踪信息分发到 WindowManager。
// 通信模型: \\.\pipe\danmaku_overlay_<pid> 上的行分隔 JSON (双向, lua 为主);
//           独立监听线程 + 读取线程。

#include "common/platform.h"
#include "common/types.h"
#include <thread>

namespace danmaku_overlay {

// 前向声明
class DanmakuEngine;
class WindowManager;

enum class IpcMessageType : uint8_t {
    LoadAss       = 1,    // 加载 ASS 文件 (文件路径)
    Sync          = 2,    // 事件驱动时间同步 (file-loaded/seek/speed/pause)
    UpdateWindow  = 3,    // 已废弃 (保留协议槽位)
    SetConfig     = 4,    // 渲染参数 (透明度/字体/速度)
    ClearDanmaku  = 5,    // 清空弹幕
    Shutdown      = 6,    // 关闭 overlay exe
    Ping          = 7,    // 心跳
    SetDelay      = 8,    // 弹幕延迟 {"delay":秒}
    SetEnabled    = 9,    // 弹幕渲染开关 {"enabled":bool}
    SetHdr        = 10,   // HDR 开关 {"enabled":bool}
    SetHdrPeak    = 11,   // HDR 峰值亮度 {"hdr_peak":nits}

    Pong          = 128,  // 心跳回复
    Error         = 129,  // 错误报告
    Ready         = 130,  // 初始化完成
};

struct IpcMessage {
    IpcMessageType type = IpcMessageType::Ping;
    std::string    json_payload;
    IpcMessage() = default;
    IpcMessage(IpcMessageType t, std::string payload)
        : type(t), json_payload(std::move(payload)) {}
};

struct IpcPlayState {
    double   time_pos   = 0.0;
    double   duration   = 0.0;
    bool     paused     = false;
    double   speed      = 1.0;
    bool     seeking    = false;
    uint32_t osd_width  = 0;
    uint32_t osd_height = 0;
};

class IpcServer {
public:
    IpcServer();
    ~IpcServer();

    ErrorCode initialize(const wchar_t* pipe_name,
                         DanmakuEngine* engine,
                         WindowManager* window);

    void start_listening();
    void shutdown();

    // 监听线程是否仍在运行 (shutdown 命令只置 running_, 主循环必须检查)
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    bool send_message(IpcMessageType type, const std::string& json_payload);

private:
    void listen_thread_main();
    void read_thread_main();
    void handle_message(const IpcMessage& msg);
    IpcMessage parse_json_message(const std::string& line);

    std::string parse_ass_filepath(const std::string& json);
    IpcPlayState parse_play_state(const std::string& json);
    void parse_window_info(const std::string& json);
    void parse_render_config(const std::string& json);
    void parse_delay_config(const std::string& json);
    void parse_hdr_config(const std::string& json);
    void parse_hdr_peak_config(const std::string& json);
    std::string build_json_response(IpcMessageType type,
                                    const std::string& payload = "");

    HANDLE              pipe_handle_      = INVALID_HANDLE_VALUE;
    wchar_t             pipe_name_[256]   = {};
    DanmakuEngine*      engine_           = nullptr;
    WindowManager*      window_           = nullptr;

    std::thread         listen_thread_;
    std::thread         read_thread_;
    std::atomic<bool>   running_{false};
    std::atomic<bool>   client_connected_{false};
    std::atomic<bool>   shutdown_done_{false};  // 保证 shutdown 幂等, 仅日志/清理一次

    std::mutex          write_mutex_;

    std::string         recv_buffer_;
};

} // namespace danmaku_overlay
