#pragma once

// danmaku_engine.h - ASS 弹幕渲染引擎 + 文本布局缓存 (glyph_cache 已并入)
// danmaku_overlay - 独立弹幕叠加渲染器
//
// 职责 (纯弹幕语义层, 底层像素合成拆到 LayeredPresenter):
//   - ASS 文件解析 (Script Info / Styles / Events)
//   - ASS 覆盖标签解析 (\move, \pos, \c, \alpha, \fs, \fn, \b, \an, ...)
//   - 内置 QPC 时钟驱动弹幕推进 (不依赖 Lua 高频上报, 只在关键事件同步)
//   - 每帧把弹幕画到 LayeredPresenter 的透明画布上, 由其合成上屏
//   - 透明度/字体缩放/全局延迟/渲染开关 控制

#include "common/platform.h"
#include "common/types.h"
#include "render/layered_presenter.h"

#include <mutex>
#include <atomic>
#include <unordered_map>
#include <list>

namespace danmaku_overlay {

// ============================================================
// 文本布局缓存条目 (GlyphCache 用)
// ============================================================
struct GlyphEntry {
    uint64_t hash = 0;
    ComPtr<IDWriteTextLayout> layout;
    // 字形轮廓几何 (layout 坐标系, 原点 = layout 左上)。描边只能用 DrawGeometry
    // 描外圈, 否则整字多层重画会让半透明 alpha 累积到接近不透明。
    ComPtr<ID2D1Geometry>   path;
    float width  = 0.0f;
    float height = 0.0f;
    uint64_t last_used = 0;
    bool valid = false;
    std::list<uint64_t>::iterator lru_it;

    GlyphEntry() = default;
};

// ============================================================
// GlyphCache - 文本布局缓存 (IDWriteTextLayout + 字形轮廓几何, LRU 淘汰)
// ============================================================
class GlyphCache {
public:
    GlyphCache();
    ~GlyphCache();

    // d2d_factory 可选: 传入后额外构建字形轮廓几何 (供正确描边)
    ErrorCode initialize(IDWriteFactory* dwrite_factory,
                         ID2D1Factory* d2d_factory = nullptr);

    const GlyphEntry* get_or_create(const std::string& text,
                                    float font_size, bool bold,
                                    bool italic = false,
                                    const std::string& fontname = "");

    void evict_lru();           // 淘汰最久未用 (跳过本帧正在使用的条目)
    void clear();

    void tick() { ++frame_counter_; }

    // 声明"该条目本帧仍在使用" (更新 LRU 位置, 免被淘汰), 仅供 64 位 key 查表
    void touch(uint64_t key);

private:
    bool create_text_layout(GlyphEntry& entry,
                            const std::string& text,
                            float font_size, bool bold, bool italic,
                            const std::string& family);
    bool build_path_geometry(IDWriteTextLayout* layout,
                             ComPtr<ID2D1Geometry>& out);
    IDWriteTextFormat* get_text_format(float font_size, bool bold, bool italic,
                                       const std::string& family);

    static uint64_t hash_text(const std::string& text,
                              float font_size, bool bold, bool italic,
                              const std::string& fontname);
    static std::wstring utf8_to_utf16(const std::string& text);

    IDWriteFactory*                 dwrite_factory_ = nullptr;
    ID2D1Factory*                   d2d_factory_    = nullptr;

    std::unordered_map<uint64_t, GlyphEntry> cache_;
    std::list<uint64_t>                     lru_list_;   // front=最久未用
    size_t                          max_entries_    = 4096;
    uint64_t                        frame_counter_  = 0;
    std::mutex                      cache_mutex_;

    std::unordered_map<std::string, ComPtr<IDWriteTextFormat>> format_cache_;
};

// ============================================================
// DanmakuEngine - 弹幕渲染引擎
// ============================================================
class DanmakuEngine {
public:
    DanmakuEngine();
    ~DanmakuEngine();

    // 初始化: 创建底层合成器 + 字形缓存 + 内置时钟
    ErrorCode initialize(HWND overlay_hwnd, const RenderConfig& config);

    // 窗口尺寸变更请求 (任意线程可调, 实际重建在渲染线程下一帧执行)
    void request_resize(uint32_t width, uint32_t height);

    void shutdown();

    // 加载 ASS 文件 (解析并构建 AssDocument)
    ErrorCode load_ass_file(const std::string& filepath);

    // 清空弹幕。返回 true 表示确实清掉了内容 (供 IPC 日志去重)
    bool clear();

    // 时间同步 (只在关键事件调用, 不持续上报):
    //   sync/set_speed/set_paused 重设时钟基准; set_delay 运行时热调
    void sync(TimePos time_pos, double speed, bool paused);
    void set_speed(double speed);
    void set_paused(bool paused);
    // 返回 true 表示延迟确实变化 (未变化时不做平移/重画, 供日志去重)
    bool set_delay(double seconds);

    // 渲染一帧: 用内置 QPC 时钟推进弹幕时间 -> 画到合成器画布 -> 合成上屏
    bool render();   // 返回 true 表示本帧真的提交了绘制 (空闲可据此降频)
    void wait_for_next_frame();   // 阻塞到下一帧的呈现时刻
    void wait_idle(uint32_t ms);  // 空闲等待 (高分辨率定时器, 不抬高系统定时器精度)

    // 渲染开关 (关时跳过全部绘制/合成, 进程存活可即时恢复)
    void set_enabled(bool enabled);
    bool is_enabled() const;

    // IPC 线程调用, 内部加锁
    void apply_render_config(float opacity, float font_scale, uint32_t max_danmaku);
    void set_hdr_enabled(bool enabled);
    void set_hdr_peak(float peak);

uint32_t get_active_count() const;

    // 前台满载播放器抢 GPU 时的自适应让出: 注册目标进程 (mpv 的 pid)。
    //   详见 LayeredPresenter::set_gpu_yield_pid 与 layered_presenter.cpp。
    void set_mpv_gpu_pid(uint32_t pid) { presenter_.set_gpu_yield_pid(pid); }

    // 空闲帧通知 (未绘制时调用): 推进让出/恢复的统计窗口
    void notify_idle_frame() { presenter_.gpu_yield_idle_frame(); }

private:
    double clock_now() const;                                // 当前媒体时间
    void rebuild_active(TimePos t);                          // 重建活跃弹幕集合
    // ASS 颜色 -> 画刷颜色 (hdr=true 走 HDR 离屏路径; 否则 SDR 恒等)
    D2D1_COLOR_F map_ass_color(const AssColor& c, float alpha, bool hdr) const;

    LayeredPresenter    presenter_;     // 分层窗口合成器
    GlyphCache          glyph_cache_;   // 文本布局缓存
    RenderConfig        config_;
    AssDocument         ass_doc_;
    uint32_t            seek_cursor_   = 0;

    std::vector<AssEvent*>  active_events_;

    // 内置 QPC 时钟: 媒体时间 = base_time + (now_qpc - base_qpc) * speed - delay
    LARGE_INTEGER       qpc_freq_           {};
    LARGE_INTEGER       clock_base_qpc_     {};
    TimePos             clock_base_time_    = 0.0;
    double              clock_speed_        = 1.0;
    bool                clock_paused_       = false;
    bool                clock_valid_        = false;
    double              delay_              = 0.0;

    TimePos             current_time_     = 0.0;
    TimePos             last_time_pos_    = -1.0;

    bool                initialized_      = false;
    bool                render_enabled_   = true;
    bool                needs_reseek_     = false;
    bool                force_redraw_     = true;   // 配置/尺寸/暂停/seek 等需重画
    float               ass_alpha_        = 1.0f;   // 整层不透明度 (取自 ASS)
    bool                last_frame_empty_ = false;

    LARGE_INTEGER       frame_qpc_{};             // 下一次允许上屏的 QPC 截止时刻
    double              frame_interval_ = 1.0 / 60.0;
    HANDLE              frame_timer_ = nullptr;   // 高分辨率帧节拍定时器 (可空)
    bool                timer_period_raised_ = false; // 定时器兜底: 是否已 timeBeginPeriod
    bool                auto_pace_ = false;       // target_fps<=0: 每个合成 tick 提交一次
    bool                pace_compositor_ = true;  // 对齐 DWM 合成 tick (DANMAKU_PACING=timer 关闭)
    uint32_t            pace_fail_streak_ = 0;    // 连续等 tick 失败次数, 超限退回定时器

    // 线程安全: IPC 线程写 sync/set_*/load/clear/apply/get; 渲染线程持锁整帧渲染
    mutable std::mutex      engine_mutex_;
    std::atomic<bool>       resize_pending_{false};
    std::atomic<uint32_t>   resize_w_{0};
    std::atomic<uint32_t>   resize_h_{0};
};

} // namespace danmaku_overlay
