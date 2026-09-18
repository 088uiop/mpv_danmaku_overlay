// ============================================================
// main.cpp - 弹幕 overlay exe 入口
// danmaku_overlay - 高性能 mpv 弹幕叠加渲染器
// ============================================================
// 启动流程:
//   1. 命令行参数解析 (mpv 窗口句柄 / 管道名)
//   2. 初始化 COM + DPI 感知
//   3. 创建透明叠加窗口 (跟踪 mpv 窗口)
//   4. 初始化 D3D11 + Direct2D/DirectWrite 弹幕引擎
//   5. 启动命名管道 IPC 服务端 (等待 lua 脚本连接)
//   6. 启动帧率锁定渲染循环 (空闲自动降频)
//   7. 等待 IPC 关闭命令或 mpv 窗口消失
// ============================================================

#include "common/platform.h"
#include "common/types.h"
#include "common/log.h"
#include "core/ipc_server.h"
#include "render/layered_presenter.h"
#include "render/danmaku_engine.h"

#include <shellapi.h>
#include <mmsystem.h>
#include <future>
#include <chrono>

namespace danmaku_overlay {

// ============================================================
// AppContext - 模块依赖集中容器
// 取代原来散落的 g_window / g_engine / g_sync / g_ipc / g_perf 等
// 全局裸指针, 按引用注入渲染线程与主流程, 初始化/析构顺序更可控。
// ============================================================
struct AppContext {
    WindowManager*  window = nullptr;
    DanmakuEngine*  engine = nullptr;
    IpcServer*      ipc    = nullptr;

    RenderConfig    render_config;

    std::atomic<bool>  running{true};        // 主消息循环运行标志
    std::atomic<bool>  render_running{false};// 渲染线程运行标志
    std::thread        render_thread;
    std::promise<bool> engine_ready;         // 渲染线程内引擎初始化完成信号

    // 退出模型: overlay 仅在以下情况退出主循环 ——
    //   1. 绑定的 mpv 窗口被销毁 (IsWindow 返回 false);
    //   2. 收到 IPC 的 shutdown 命令;
    //   3. 被同 PID 新实例顶替 (单实例互斥, 见下方)。
    // 不再额外监控 mpv 进程: 窗口句柄是 mpv 存活最直接、最可靠的信号。
    // 单实例防护: 每个 mpv (按 PID) 只允许 1 个 overlay。
    //   inst_mutex  —— 命名互斥体, 谁先拿到谁是唯一实例;
    //   quit_event  —— 命名事件, 新实例用它通知旧实例"请退出",
    //                  主循环轮询该事件, 触发后干净退出。
    HANDLE             inst_mutex  = nullptr;
    HANDLE             quit_event  = nullptr;
};

// 渲染线程入口: 引擎初始化 + 渲染循环全在该线程
// (D2D/D3D 资源必须在其创建线程使用, 主线程不再触碰任何渲染资源)
static void render_thread_main(AppContext* ctx) {
    // 本线程需要初始化 COM (D2D/DWrite 创建工厂)。
    // MTA: 与主线程的 STA 并存, 进程内 COM 对象无跨线程封送限制。
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        DANMAKU_LOG_FATAL(LOG_TAG, "渲染线程 COM 初始化失败 (0x%08X)",
                          static_cast<unsigned int>(hr));
        ctx->engine_ready.set_value(false);
        return;
    }

    auto ec = ctx->engine->initialize(ctx->window->get_hwnd(),
                                      ctx->render_config);
    if (ec != ErrorCode::Ok) {
        DANMAKU_LOG_FATAL(LOG_TAG, "弹幕引擎初始化失败: %s",
                          error_string(ec));
        ctx->engine_ready.set_value(false);
        CoUninitialize();
        return;
    }
    ctx->engine_ready.set_value(true);

    DANMAKU_LOG_INFO(LOG_TAG, "渲染线程进入帧率锁定循环");
    constexpr DWORD kIdlePollMs = 8;   // 空闲轮询间隔 (无动画时降到 ~125Hz, 省高频空转)
    while (ctx->render_running.load(std::memory_order_acquire)) {
        // 有绘制: 按目标帧率做精确节拍; 空闲(暂停/空屏): 降频轮询, 不占高频空转
        if (ctx->engine->render()) {
            ctx->engine->wait_for_next_frame();
        } else {
            Sleep(kIdlePollMs);
        }
    }

    ctx->engine->shutdown();
    CoUninitialize();
    DANMAKU_LOG_INFO(LOG_TAG, "渲染线程退出");
}

// ============================================================
// 单实例防护: 每个 mpv (按 PID) 只允许一个 overlay 实例。
// lua 反复切形态 / 重载脚本会多次 ShellExecute 拉起本 exe, 若不加管控,
// 同一 mpv 会堆积十几个孤儿进程 (卡死桌面)。这里用命名互斥体保证唯一:
//   - 首个实例拿到互斥体 + 创建 "请退出" 事件, 正常服务;
//   - 后续实例发现互斥体已被占, 就给旧实例的 "请退出" 事件发信号, 等旧实例
//     释放互斥体后再接管。无论 lua 拉起多少次, 系统里该 mpv 永远只有 1 个 overlay。
// 必须在创建窗口/线程之前调用。
// ============================================================
static bool acquire_single_instance(uint32_t mpv_pid, const wchar_t* pipe_name,
                                    danmaku_overlay::AppContext* ctx) {
    wchar_t key[32];
    if (mpv_pid != 0) {
        swprintf_s(key, 32, L"%lu", static_cast<unsigned long>(mpv_pid));
    } else {
        // 退化: 从管道名 \\.\pipe\danmaku_overlay_<pid> 提取 PID 作为 key
        const wchar_t* p = wcsrchr(pipe_name, L'_');
        if (!p || !p[1]) {
            // 无法定位唯一 key: 跳过单例 (主循环 quit_event 仍兜底退出)
            DANMAKU_LOG_WARN(LOG_TAG,
                "单实例: 无法从参数推导唯一 key, 跳过单例防护");
            return true;
        }
        wcscpy_s(key, 32, p + 1);
    }

    wchar_t mutex_name[64], quit_name[64];
    swprintf_s(mutex_name, 64, L"Local\\danmaku_overlay_inst_%ls", key);
    swprintf_s(quit_name, 64, L"Local\\danmaku_overlay_quit_%ls", key);

    const int kMaxAttempts = 12;   // 12 * 300ms ≈ 3.6s 接管窗口
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        HANDLE mtx = CreateMutexW(nullptr, FALSE, mutex_name);
        DWORD err = GetLastError();
        if (mtx && err != ERROR_ALREADY_EXISTS) {
            // 我们是唯一实例: 持有互斥体, 创建 "请退出" 事件供将来被顶替。
            ctx->inst_mutex = mtx;
            ctx->quit_event = CreateEventW(nullptr, TRUE, FALSE, quit_name);
            if (!ctx->quit_event) {
                // 事件创建失败不致命: 只是失去 "被新实例顶替" 的能力,
                // 主循环 quit_event 仍可兜底退出。
                DANMAKU_LOG_WARN(LOG_TAG,
                    "单实例: 退出事件创建失败 (GLE=%lu), 跳过", GetLastError());
            } else {
                DANMAKU_LOG_INFO(LOG_TAG,
                    "已取得 mpv (key=%ls) 单实例所有权", key);
            }
            return true;
        }
        // 互斥体已被旧实例占用 -> 通知旧实例退出, 然后重试接管。
        if (mtx) CloseHandle(mtx); // 不要持有别人的互斥体句柄
        HANDLE q = OpenEventW(EVENT_MODIFY_STATE, FALSE, quit_name);
        if (q) {
            SetEvent(q); // 让旧实例的主循环收到 "被顶替" 信号
            CloseHandle(q);
            DANMAKU_LOG_INFO(LOG_TAG,
                "检测到同 PID 旧实例, 已发退出信号 (尝试 %d)", attempt + 1);
        } else {
            // 旧实例极早期尚未创建 quit 事件, 稍后重试即可。
            DANMAKU_LOG_INFO(LOG_TAG,
                "旧实例存在但退出事件未就绪, 重试 (尝试 %d)", attempt + 1);
        }
        Sleep(300);
    }
    DANMAKU_LOG_FATAL(LOG_TAG,
        "多次尝试仍无法取得 mpv (key=%ls) 单实例, 放弃启动", key);
    return false;
}

} // namespace danmaku_overlay

// ============================================================
// 命令行参数解析
// ============================================================
struct CmdArgs {
    uint32_t mpv_pid    = 0;       // mpv 进程 ID (用于反查 HWND)
    HWND     mpv_hwnd   = nullptr;
    wchar_t  pipe_name[256] = L"";
    wchar_t  log_path[512] = L"";
    uint32_t target_fps  = 60;
};

// EnumWindows 回调: 按 PID 反找主窗口
struct EnumCtx { DWORD pid; HWND result; };

static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    // 必须是可见的顶层窗口
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE; // 跳过子窗口
    ctx->result = hwnd;
    return FALSE; // 找到了, 停止枚举
}

static HWND find_mpv_hwnd(uint32_t pid) {
    if (pid == 0) return nullptr;
    EnumCtx ctx{ static_cast<DWORD>(pid), nullptr };

    // mpv 可能尚未创建窗口, 最多等待 ~3 秒
    for (int i = 0; i < 30; ++i) {
        ctx.result = nullptr;
        EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.result) return ctx.result;
        Sleep(100);
    }
    return nullptr;
}

static CmdArgs parse_args(LPWSTR cmd_line) {
    CmdArgs args;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd_line, &argc);
    if (!argv) return args;

    for (int i = 0; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--mpv-pid" && i + 1 < argc) {
            swscanf_s(argv[++i], L"%u", &args.mpv_pid);
        } else if (arg == L"--mpv-hwnd" && i + 1 < argc) {
            // 兼容: 直接传 HWND (十六进制)
            unsigned long hwnd_val = 0;
            if (swscanf_s(argv[++i], L"%lu", &hwnd_val) == 1 ||
                swscanf_s(argv[i], L"0x%lx", &hwnd_val) == 1) {
                args.mpv_hwnd = reinterpret_cast<HWND>(hwnd_val);
            }
        } else if (arg == L"--pipe" && i + 1 < argc) {
            wcscpy_s(args.pipe_name, 256, argv[++i]);
        } else if (arg == L"--log" && i + 1 < argc) {
            wcscpy_s(args.log_path, 512, argv[++i]);
        } else if (arg == L"--fps" && i + 1 < argc) {
            swscanf_s(argv[++i], L"%u", &args.target_fps);
        }
    }

    // 如果只给了 PID, 通过 EnumWindows 反查 HWND
    if (!args.mpv_hwnd && args.mpv_pid) {
        args.mpv_hwnd = find_mpv_hwnd(args.mpv_pid);
    }

    LocalFree(argv);
    return args;
}

// ============================================================
// wWinMain - 入口
// ============================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)nCmdShow;
    // 初始化日志
    auto& logger = danmaku_overlay::Logger::instance();
    logger.set_level(danmaku_overlay::LogLevel::Info);

    // 初始化 COM (Direct2D/DirectWrite 需要)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        DANMAKU_LOG_FATAL(LOG_TAG, "COM 初始化失败 (0x%08X)",
                          static_cast<unsigned int>(hr));
        return 1;
    }

    // DPI 感知 (Per-Monitor V2)
    danmaku_overlay::platform::SetDpiAwareness();

    // 提升系统定时器分辨率到 1ms。
    // ⚠ 关键: Windows 默认定时器粒度约 15.6ms, 不加这行的话 Sleep(n) 实际会
    //   睡到 15.6ms 的整数倍, 对 60fps (16.67ms/帧) 的节奏控制是灾难 — 帧间隔
    //   会在 15.6 / 31.2ms 之间跳, 表现为弹幕"快一下慢一下"的卡顿。
    //   必须在渲染循环开始前调用, 退出时 timeEndPeriod 还原。
    timeBeginPeriod(1);
    DANMAKU_LOG_INFO(LOG_TAG, "系统定时器分辨率已提升为 1ms");

    // 解析命令行参数
    CmdArgs args = parse_args(lpCmdLine);

    // 设置日志文件 (优先用 --log 指定的路径, 否则用 TEMP)
    {
        const wchar_t* log_path = args.log_path[0] ? args.log_path : nullptr;
        char log_path_a[512] = {};
        if (!log_path) {
            // 默认: %TEMP%/danmaku_overlay.log
            wchar_t temp_dir[MAX_PATH];
            GetTempPathW(MAX_PATH, temp_dir);
            swprintf_s(args.log_path, 512, L"%lsdanmaku_overlay.log", temp_dir);
            log_path = args.log_path;
        }
        WideCharToMultiByte(CP_UTF8, 0, log_path, -1,
                            log_path_a, sizeof(log_path_a), nullptr, nullptr);
        logger.set_file(log_path_a);
    }

    if (!args.mpv_hwnd || !IsWindow(args.mpv_hwnd)) {
        DANMAKU_LOG_FATAL(LOG_TAG, "无效的 mpv 窗口句柄, 退出");
        CoUninitialize();
        return 1;
    }

    // 默认管道名 (基于 PID)
    wchar_t default_pipe[256];
    if (args.pipe_name[0] == 0) {
        swprintf_s(default_pipe, 256,
            L"\\\\.\\pipe\\danmaku_overlay_%lu", GetCurrentProcessId());
        wcscpy_s(args.pipe_name, 256, default_pipe);
    }

    DANMAKU_LOG_INFO(LOG_TAG, "overlay PID: %lu", GetCurrentProcessId());
    if (args.mpv_pid) DANMAKU_LOG_INFO(LOG_TAG, "mpv PID: %u", args.mpv_pid);
    DANMAKU_LOG_INFO(LOG_TAG, "mpv 窗口句柄: 0x%p", args.mpv_hwnd);
    DANMAKU_LOG_INFO(LOG_TAG, "管道名: %ls", args.pipe_name);
    DANMAKU_LOG_INFO(LOG_TAG, "目标帧率: %u", args.target_fps);

    // 创建模块实例 + 依赖容器 (按引用注入, 取代全局裸指针)
    danmaku_overlay::WindowManager  window;
    danmaku_overlay::DanmakuEngine  engine;
    danmaku_overlay::IpcServer      ipc;

    danmaku_overlay::AppContext ctx;
    ctx.window = &window;
    ctx.engine = &engine;
    ctx.ipc    = &ipc;

    // ---- 单实例防护: 每个 mpv 只允许 1 个 overlay 实例 ----
    // 必须在创建窗口/线程之前判定。lua 反复切形态或重载脚本会多次拉起本 exe,
    // 若不加管控就会同 PID 堆积出十几个孤儿进程 (卡死桌面)。这里用命名互斥体
    // 保证唯一, 并把旧实例顶替退出。
    {
        uint32_t mpv_pid = args.mpv_pid;
        if (mpv_pid == 0 && args.mpv_hwnd && IsWindow(args.mpv_hwnd)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(args.mpv_hwnd, &pid);
            mpv_pid = static_cast<uint32_t>(pid);
        }
        if (!danmaku_overlay::acquire_single_instance(mpv_pid, args.pipe_name, &ctx)) {
            timeEndPeriod(1);
            CoUninitialize();
            return 1;
        }
    }

    // 告诉 WindowManager 要跟踪的 mpv 窗口
    window.set_mpv_hwnd(args.mpv_hwnd);

    // 渲染配置
    danmaku_overlay::RenderConfig config;
    config.target_fps = args.target_fps;

    // 步骤 1: 创建透明叠加窗口 (跟踪 mpv 窗口)
    auto ec = window.create(hInstance, 1280, 720, true);
    if (ec != danmaku_overlay::ErrorCode::Ok) {
        DANMAKU_LOG_FATAL(LOG_TAG, "窗口创建失败: %s",
                          danmaku_overlay::error_string(ec));
        CoUninitialize();
        return 1;
    }

    // 步骤 2: 启动渲染线程 (引擎初始化在渲染线程内执行:
    // D2D/D3D 资源必须在创建它的线程上使用, 这样渲染循环、resize、
    // shutdown 全部落在同一线程, 主线程只负责窗口消息和位置跟踪)

    ctx.render_config = config;
    ctx.render_running.store(true, std::memory_order_release);
    ctx.render_thread =
        std::thread(danmaku_overlay::render_thread_main, &ctx);

    // 等待引擎初始化完成 (失败则直接退出)
    if (!ctx.engine_ready.get_future().get()) {
        if (ctx.render_thread.joinable())
            ctx.render_thread.join();
        window.destroy();
        timeEndPeriod(1);
        CoUninitialize();
        return 1;
    }

    // 引擎初始化完成 → DirectComposition target 已建立、首帧已清屏透明,
    // 此时再显示窗口, 避免 DComp 接管前闪现黑屏。
    window.show();

    // 步骤 3: 初始化 IPC 服务端
    ec = ipc.initialize(args.pipe_name, &engine, &window);
    if (ec != danmaku_overlay::ErrorCode::Ok) {
        DANMAKU_LOG_FATAL(LOG_TAG, "IPC 初始化失败: %s",
                          danmaku_overlay::error_string(ec));
        ctx.render_running.store(false, std::memory_order_release);
        if (ctx.render_thread.joinable())
            ctx.render_thread.join();
        window.destroy();
        timeEndPeriod(1);
        CoUninitialize();
        return 1;
    }

    // 渲染帧率由 DanmakuEngine 内部负责。
    // 启动 IPC 监听线程
    ipc.start_listening();

    // 进入主消息循环 (渲染已在渲染线程独立运行)
    DANMAKU_LOG_INFO(LOG_TAG, "进入主消息循环");

    // 主线程: 只处理窗口消息 + 位置/尺寸跟踪 + 退出检查。
    // 渲染循环由渲染线程负责, 主线程空闲时 Sleep 避免忙等。
    MSG msg = {};
    uint32_t last_w = 0, last_h = 0;  // 跟踪窗口尺寸变化
    while (ctx.running.load(std::memory_order_acquire)) {
        // 处理窗口消息 (非阻塞)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                ctx.running.store(false, std::memory_order_release);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!ctx.running.load(std::memory_order_acquire))
            break;

        // 单实例顶替: 同 PID 新实例已通知本实例退出 (lua 重载/切形态时)
        if (ctx.quit_event &&
            WaitForSingleObject(ctx.quit_event, 0) == WAIT_OBJECT_0) {
            DANMAKU_LOG_INFO(LOG_TAG, "被同 PID 新实例顶替, 退出主循环");
            break;
        }

        // 检查绑定的 mpv 窗口是否还存在 —— 窗口被销毁即退出主循环
        if (!IsWindow(args.mpv_hwnd)) {
            DANMAKU_LOG_INFO(LOG_TAG, "mpv 窗口已关闭, 退出");
            break;
        }

        // 收到 IPC shutdown 命令 (IpcServer 内部标志)
        if (!ipc.is_running()) {
            DANMAKU_LOG_INFO(LOG_TAG, "收到 shutdown 命令, 退出主循环");
            break;
        }

        // 同步 overlay 窗口位置到 mpv 窗口 (跟踪移动/缩放/跨屏 DPI 变化)
        window.sync_to_mpv_window();

        // 检测窗口尺寸变化 → 请求渲染线程重建 (不阻塞主线程)
        uint32_t cur_w = window.get_physical_width();
        uint32_t cur_h = window.get_physical_height();
        if (cur_w > 0 && cur_h > 0
            && (cur_w != last_w || cur_h != last_h)) {
            engine.request_resize(cur_w, cur_h);
            last_w = cur_w;
            last_h = cur_h;
        }

        // 主线程空闲: 让出 CPU, 渲染节奏完全由渲染线程的 vsync 等待控制
        Sleep(1);
    }

    // 清理
    DANMAKU_LOG_INFO(LOG_TAG, "danmaku_overlay 退出");

    ipc.shutdown();     // 先停 IPC, 避免其线程再访问引擎
    ctx.render_running.store(false, std::memory_order_release);
    if (ctx.render_thread.joinable())
        ctx.render_thread.join();   // 渲染线程退出并执行 engine.shutdown()

    // 释放单实例互斥体 / 退出事件 (进程退出时内核对象也随之回收,
    // 这里显式关闭以免句柄滞留, 确保下一实例能顺利接管)
    if (ctx.inst_mutex) {
        CloseHandle(ctx.inst_mutex);
        ctx.inst_mutex = nullptr;
    }
    if (ctx.quit_event) {
        CloseHandle(ctx.quit_event);
        ctx.quit_event = nullptr;
    }

    window.destroy();

    timeEndPeriod(1);   // 还原系统定时器分辨率 (配对 timeBeginPeriod)
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
