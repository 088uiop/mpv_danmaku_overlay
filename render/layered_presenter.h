#pragma once

// layered_presenter.h - 合成器与透明叠加窗口
// danmaku_overlay - 高性能 mpv 弹幕叠加渲染器
//
// WindowManager: 创建 DirectComposition 幽灵窗口 (无边框/透明/不接收输入),
//   作为 mpv 的 owned window 置于其上层, 并实时跟踪 mpv 窗口位置/尺寸。
// LayeredPresenter: 负责把弹幕像素画到屏幕上 (D3D11 + D2D/DWrite +
//   DirectComposition), 含 HDR 离屏 AA 合成路径。

#include "common/platform.h"
#include "common/types.h"

namespace danmaku_overlay {

// 透明叠加窗口: Win32 幽灵层, 纯显示, 不接收任何鼠标/键盘事件
class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    ErrorCode create(HINSTANCE hInstance, uint32_t width,
                     uint32_t height, bool borderless);

    DpiInfo get_dpi() const;

    HWND get_hwnd() const { return hwnd_; }

void set_mpv_hwnd(HWND hwnd) {
        mpv_hwnd_ = hwnd;
        if (hwnd_ && hwnd && IsWindow(hwnd)) {
            SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT,
                              reinterpret_cast<LONG_PTR>(hwnd));
            SetWindowPos(hwnd_, hwnd, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    HWND get_mpv_hwnd() const { return mpv_hwnd_; }

    // 来自 IPC 的废弃消息, 仅保留协议兼容 (实际几何由 sync_to_mpv_window 轮询)
    void update_mpv_window_pos(int x, int y, int width, int height, int dpi);

    // 主动轮询 mpv 窗口位置并同步 overlay (渲染 tick 中调用)
    void sync_to_mpv_window();

    void show();
    void hide();
    void set_visible(bool visible);   // 仅供 IPC 调用
    void bring_above_mpv();
    void destroy();

    uint32_t get_physical_width() const { return phys_width_; }
    uint32_t get_physical_height() const { return phys_height_; }

private:
    static LRESULT CALLBACK static_wnd_proc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp);
    LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool register_window_class();

HWND        hwnd_      = nullptr;
    HWND        mpv_hwnd_  = nullptr;
    HINSTANCE   hinstance_ = nullptr;
    DpiInfo     dpi_;

    uint32_t    phys_width_   = 1280;
    uint32_t    phys_height_  = 720;

    int         mpv_rect_x_ = 0;
    int         mpv_rect_y_ = 0;
    int         mpv_rect_w_ = 0;
    int         mpv_rect_h_ = 0;

    bool        class_registered_ = false;

    static WindowManager* s_instance;
    static const wchar_t* kClassName;
};

// 合成器: D3D11 + D2D/DWrite + DirectComposition
//
// HDR 文字渲染: AA coverage 与绝对亮度分离 —— DirectWrite 用 SDR 幅度
//   (≤1.0) 把文字画进 FP16 离屏缓冲, 再由 D3D11 pass 统一乘 (hdr_peak/80)
//   合成进 HDR 交换链, 消除 HDR 文字边缘的动态黑/灰毛刺。
// 线程契约: 所有方法必须在渲染线程调用。
class LayeredPresenter {
public:
    LayeredPresenter();
    ~LayeredPresenter();

    ErrorCode initialize(HWND hwnd, uint32_t width, uint32_t height,
                         DpiInfo dpi);
    void shutdown();

    ErrorCode resize(uint32_t width, uint32_t height);
    bool valid() const;
    ErrorCode recreate_target();

// 每帧流程: begin_frame → (引擎绘制) → end_frame → present(alpha)
HRESULT begin_frame();
    HRESULT end_frame();
    void present(float global_alpha);

    // ── 前台满载播放器抢占 GPU 时的自适应让出 ──
    //   设置目标进程 (mpv 的 pid)。原理与判定条件见 .cpp 中
    //   "GPU 调度优先级让出" 一节: 只在检测到持续呈现背压时降级, 且可逆。
    void set_gpu_yield_pid(uint32_t pid) { gpu_yield_pid_ = pid; }

    // 空闲帧通知 (引擎未绘制时由渲染循环调用): 推进统计窗口, 使弹幕空闲后
    // 及时把 mpv 的 GPU 调度优先级还回去。
    void gpu_yield_idle_frame() { gpu_yield_tick(false, 0.0); }

    // ── 合成器时钟对齐 (DWM 帧节拍) ──
    //   不再用独立的 QPC 定时器猜 vsync 相位: 渲染线程每帧阻塞到 DWM 下一次
    //   合成 tick 后再画并 Present, 弹幕位置则取"预计上屏时刻"的媒体时间。
    //   好处: (1) 与显示刷新无拍频, 不会周期性重复/跳帧; (2) 渲染线程被调度
    //   延迟、Present 偶发阻塞时, 位置仍与上屏时刻匹配; (3) 顺带观测 DWM 实际
    //   合成间隔, 作为"合成被饿死"的判据 (见 gpu_yield_tick)。
    // 返回 true = 确实等到了一次合成 tick; false = 超时/不支持, 调用方应退回定时器。
    bool wait_compositor_tick(DWORD timeout_ms);
    // 预计下一次合成 (即本帧被 DWM 取用) 的 QPC 时刻; 失败返回 false。
    bool predict_next_frame_qpc(LONGLONG& out_qpc);
    // DWM 当前合成速率 (Hz), 未知时为 60。
    double compositor_rate_hz() const { return comp_rate_hz_; }

    // ── 呈现诊断 (环境变量 DANMAKU_DIAG=1 时开启, 默认不产生任何开销) ──
    // 用于区分「我们自己提交被节流」与「提交正常但 DWM 不按目标帧率取帧」:
    //   分相位耗时 (begin_frame / EndDraw 绘制 / Present) + Present 返回码
    //   + DXGI GetFrameStatistics 的 DWM 侧取帧计数。
    bool diag_active() const {
        return diag_on_ && diag_freq_.QuadPart != 0;
    }
    void diag_tick_render();                    // 每次 render() 调用一次
    void diag_add_phase(int phase, const LARGE_INTEGER& t0);   // 0=begin 1=draw 2=present
    void diag_flush_if_due(uint32_t active_danmaku, double target_fps);

    // HDR 模式 (开关/峰值经 IPC 设置, 由渲染线程 process_pending_hdr 消费)
    void request_hdr_enabled(bool enabled);
    void request_hdr_peak(float peak);
    void process_pending_hdr();
    bool is_hdr() const { return hdr_active_; }

    // 颜色映射: map_color 为 SDR 恒等; map_color_hdr_layer 为 HDR 离屏路径
    //   (sRGB→线性, 不乘 hdr_peak, 峰值缩放留给合成 pass)
    D2D1_COLOR_F map_color(uint8_t r, uint8_t g, uint8_t b, float alpha) const;
    D2D1_COLOR_F map_color_hdr_layer(uint8_t r, uint8_t g, uint8_t b,
                                     float alpha) const;

    // 绘制资源访问 (引擎在 begin_frame/end_frame 之间使用)
    ID2D1DeviceContext* context() const { return d2d_context_.Get(); }
    ID2D1SolidColorBrush* text_brush() const { return color_brush_.Get(); }
    ID2D1SolidColorBrush* outline_brush() const { return color_brush_.Get(); }
    ID2D1SolidColorBrush* shadow_brush() const { return color_brush_.Get(); }
    ID2D1StrokeStyle* stroke_style() const { return stroke_style_.Get(); }

    // HDR 离屏 AA 绘制资源 (HDR 模式下引擎改用这组)
    ID2D1DeviceContext*  hdr_scratch_context()  const { return hdr_scratch_ctx_.Get(); }
    ID2D1SolidColorBrush* hdr_text_brush()    const { return hdr_brush_.Get(); }
    ID2D1SolidColorBrush* hdr_outline_brush() const { return hdr_brush_.Get(); }
    ID2D1SolidColorBrush* hdr_shadow_brush()  const { return hdr_brush_.Get(); }

    // factory 供 GlyphCache 初始化
    ID2D1Factory1* d2d_factory() const { return d2d_factory_.Get(); }
    IDWriteFactory* dwrite_factory() const { return dwrite_factory_.Get(); }

    uint32_t width() const { return phys_width_; }
    uint32_t height() const { return phys_height_; }
    DpiInfo dpi() const { return dpi_; }

private:
    ErrorCode init_d3d11();
    ErrorCode init_d2d();
    ErrorCode create_composition_resources();
    ErrorCode create_swap_chain(DXGI_FORMAT format);
    ErrorCode rebuild_for_hdr(bool hdr);

    ErrorCode init_hdr_composite();
    ErrorCode ensure_hdr_scratch();
    ErrorCode create_hdr_scratch();
    void destroy_hdr_scratch();
    void release_hdr_scratch_target();
    void composite_hdr_scratch();
    ID3D11RenderTargetView* acquire_backbuffer_rtv(ID3D11Texture2D* tex);
    void release_backbuffer_rtvs();

    HWND hwnd_ = nullptr;

    // D3D11
    ComPtr<ID3D11Device>          d3d_device_;
    ComPtr<ID3D11DeviceContext>   d3d_context_;

// DXGI composition swap chain
    ComPtr<IDXGIFactory2>         dxgi_factory2_;
    ComPtr<IDXGISwapChain1>       swap_chain_;
    ComPtr<ID3D11Texture2D>       back_buffer_;

    // DirectComposition 视觉树
    ComPtr<IDCompositionDevice>   dcomp_device_;
    ComPtr<IDCompositionTarget>   dcomp_target_;
    ComPtr<IDCompositionVisual>   dcomp_visual_;

    // D2D
    ComPtr<ID2D1Factory1>         d2d_factory_;
    ComPtr<ID2D1Device>           d2d_device_;
    ComPtr<ID2D1DeviceContext>    d2d_context_;
    ComPtr<ID2D1Bitmap1>          d2d_target_;
    ComPtr<ID2D1SolidColorBrush>  color_brush_;
    ComPtr<ID2D1StrokeStyle>      stroke_style_;

    // DWrite
    ComPtr<IDWriteFactory>        dwrite_factory_;

    // 尺寸 / DPI
    uint32_t phys_width_  = 1280;
    uint32_t phys_height_ = 720;
    DpiInfo  dpi_;

    // 整层透明度 (ass_alpha * config.opacity), 由 present() 写入
    float global_alpha_ = 1.0f;

    // HDR 渲染模式
    bool  hdr_active_ = false;
    float hdr_peak_   = 203.0f;
    float sdr_lin_lut_[256] = {};   // sRGB 字节 → 线性 (0..1)

    // HDR 离屏 AA 合成 (scratch)
    ComPtr<ID2D1DeviceContext>       hdr_scratch_ctx_;
    ComPtr<ID2D1SolidColorBrush>     hdr_brush_;
    ComPtr<ID3D11Texture2D>          hdr_scratch_tex_;
    ComPtr<ID3D11ShaderResourceView> hdr_scratch_srv_;
    ComPtr<ID2D1Bitmap1>             hdr_scratch_bmp_;
    uint32_t hdr_scratch_w_    = 0;
    uint32_t hdr_scratch_h_    = 0;

    // 合成 pass 的 D3D11 资源
    ComPtr<ID3D11VertexShader>       comp_vs_;
    ComPtr<ID3D11PixelShader>        comp_ps_;
    ComPtr<ID3D11Buffer>             comp_cb_;
    ComPtr<ID3D11BlendState>         comp_blend_;
    ComPtr<ID3D11SamplerState>       comp_sampler_;
    ComPtr<ID3D11RasterizerState>    comp_rs_;
    bool  hdr_composite_ready_ = false;

    // back buffer RTV 缓存 (避免每帧新建视图)
    static constexpr int kBackBufferRtvSlots = 3;
    ID3D11Texture2D*              bb_rtv_tex_[kBackBufferRtvSlots] = {};
    ComPtr<ID3D11RenderTargetView> bb_rtv_[kBackBufferRtvSlots];
    int                           bb_rtv_next_ = 0;

// 呈现诊断状态 (仅渲染线程访问; 见 diag_* 方法)
    bool          diag_on_     = false;
    LARGE_INTEGER diag_freq_{};
    LARGE_INTEGER diag_win_start_{};
    uint32_t      diag_renders_          = 0;
    uint32_t      diag_presents_         = 0;
    uint32_t      diag_present_fail_     = 0;
    uint32_t      diag_present_occluded_ = 0;
    
    unsigned      diag_present_hr_last_  = 0;
    double        diag_ms_[6]            = {};
    double        diag_ms_max_[6]        = {};
    uint32_t      diag_slow20_[6]        = {};
    uint32_t      diag_slow50_[6]        = {};
    ComPtr<ID3D11Query> diag_fence_;   // 诊断: 本帧绘制在 GPU 上真正完成的耗时

    // GPU 调度优先级让出 (见 .cpp 中的实现注释)
    void gpu_yield_tick(bool presented, double present_ms);
    void gpu_yield_apply(bool demote, bool log_msg);
    uint32_t      gpu_yield_pid_         = 0;      // 目标进程 (0 = 不启用)
    bool          gpu_yield_demoted_     = false;  // 当前是否已降级
    LARGE_INTEGER gpu_yield_freq_{};
    LARGE_INTEGER gpu_yield_next_{};               // 下一个统计窗口的起点
    LARGE_INTEGER gpu_yield_cooldown_{};           // 试探性恢复的冷却截止
    uint32_t      gpu_yield_win_frames_  = 0;      // 本窗口 Present 次数
    uint32_t      gpu_yield_win_slow_    = 0;      // 本窗口被阻塞的 Present 次数
    uint32_t      gpu_yield_ok_streak_   = 0;      // 连续"有帧且无背压"窗口数
    uint32_t      gpu_yield_idle_streak_ = 0;      // 连续"无帧"窗口数
    // DWM 合成间隔信号: 本窗口观察到的 DWM 帧数 / 其中间隔明显偏长的帧数。
    // 补充"Present 被阻塞"这一信号 —— flip-discard 下 Present 常常不阻塞,
    // 而是已提交的帧在被 DWM 取用前被后一帧直接顶掉 (表现为丢帧但 Present 很快)。
    uint32_t      gpu_yield_win_dwm_      = 0;
    uint32_t      gpu_yield_win_dwm_late_ = 0;
    bool          gpu_yield_use_dwm_      = true;  // DANMAKU_YIELD_DWM=0 关闭
    void          gpu_yield_note_dwm_interval(double ms);
    void          observe_dwm_frame();

    // 合成器时钟状态
    double        comp_rate_hz_      = 60.0;
    double        comp_nominal_ms_   = 1000.0 / 60.0;
    LONGLONG      comp_last_frame_   = 0;        // 上次观察到的 DWM lastFrameTime (QPC)
    uint32_t      diag_dwm_          = 0;
    uint32_t      diag_dwm_late_     = 0;
    double        diag_dwm_max_ms_   = 0.0;

    unsigned long diag_fs_pc_ = 0, diag_fs_pr_ = 0, diag_fs_sr_ = 0;
    bool          diag_fs_valid_ = false;

    // IPC 线程写 → 渲染线程读
    std::atomic<bool>  hdr_enabled_pending_{false};
    std::atomic<bool>  hdr_enabled_requested_{false};
    std::atomic<bool>  hdr_peak_pending_{false};
    std::atomic<float> hdr_peak_requested_{203.0f};
};

} // namespace danmaku_overlay
