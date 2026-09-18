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

    void set_mpv_hwnd(HWND hwnd) { mpv_hwnd_ = hwnd; }
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

    // IPC 线程写 → 渲染线程读
    std::atomic<bool>  hdr_enabled_pending_{false};
    std::atomic<bool>  hdr_enabled_requested_{false};
    std::atomic<bool>  hdr_peak_pending_{false};
    std::atomic<float> hdr_peak_requested_{203.0f};
};

} // namespace danmaku_overlay
