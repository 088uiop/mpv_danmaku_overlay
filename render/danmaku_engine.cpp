// danmaku_engine.cpp - ASS 弹幕渲染引擎 + 文本布局缓存实现
// danmaku_overlay - 独立弹幕叠加渲染器
// (glyph_cache.cpp 已并入本文件)

#include "danmaku_engine.h"
#include "core/ass_parser.h"
#include "common/log.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

// Win10 1803+ 高分辨率可等待定时器标志 (旧 SDK 无此宏时兜底;
// 更低版本系统上创建会失败, 代码自动退回 Sleep 路径)。
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace danmaku_overlay {

// ============================================================
// 自定义 IDWriteTextRenderer - 把文本抽成几何轮廓
// (仅 DrawGlyphRun: 每个 glyph run 生成 ID2D1PathGeometry, 平移后合并)
// MinGW 下 IDWriteGeometrySink 即 ID2D1SimplifiedGeometrySink 的 typedef。
// ============================================================
namespace {

class PathTextRenderer : public IDWriteTextRenderer {
public:
    explicit PathTextRenderer(ID2D1Factory* factory) : factory_(factory) {
        if (factory_) factory_->AddRef();
    }
    virtual ~PathTextRenderer() {
        for (auto* g : runs_) if (g) g->Release();
        if (factory_) factory_->Release();
    }

    const std::vector<ID2D1Geometry*>& runs() const { return runs_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown
            || riid == __uuidof(IDWritePixelSnapping)
            || riid == __uuidof(IDWriteTextRenderer)) {
            *ppv = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        if (disabled) *disabled = TRUE;   // 要矢量轮廓, 不要像素对齐
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        if (m) *m = DWRITE_MATRIX{ 1.f, 0.f, 0.f, 1.f, 0.f, 0.f };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* v) override {
        if (v) *v = 1.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* glyphRun,
        const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        if (!glyphRun || !glyphRun->fontFace || !factory_) return S_OK;
        if (glyphRun->glyphCount == 0) return S_OK;

        ComPtr<ID2D1PathGeometry> pg;
        if (FAILED(factory_->CreatePathGeometry(&pg))) return S_OK;

        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(pg->Open(&sink))) return S_OK;

        HRESULT hr = glyphRun->fontFace->GetGlyphRunOutline(
            glyphRun->fontEmSize, glyphRun->glyphIndices,
            glyphRun->glyphAdvances, glyphRun->glyphOffsets,
            glyphRun->glyphCount, glyphRun->isSideways,
            (glyphRun->bidiLevel & 1) != 0, sink.Get());
        if (FAILED(hr) || FAILED(sink->Close())) return S_OK;

        ComPtr<ID2D1TransformedGeometry> tg;
        D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Translation(
            baselineOriginX, baselineOriginY);
        if (SUCCEEDED(factory_->CreateTransformedGeometry(pg.Get(), m, &tg))) {
            ID2D1Geometry* g = nullptr;
            tg->QueryInterface(__uuidof(ID2D1Geometry),
                              reinterpret_cast<void**>(&g));
            if (g) runs_.push_back(g);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT,
                                         const DWRITE_UNDERLINE*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT,
                                         const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT,
                                        IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }

private:
    ID2D1Factory*               factory_ = nullptr;
    LONG                        ref_     = 1;
    std::vector<ID2D1Geometry*> runs_;
};

} // anonymous namespace

// ============================================================
// 文本布局缓存: 静态/局部工具
// ============================================================

uint64_t GlyphCache::hash_text(const std::string& text,
                              float font_size, bool bold, bool italic,
                              const std::string& fontname) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    uint32_t fs_bits = static_cast<uint32_t>(font_size * 2.0f);
    h ^= static_cast<uint64_t>(fs_bits);
    h *= 1099511628211ULL;
    h ^= bold ? 1ULL : 0ULL;
    h *= 1099511628211ULL;
    h ^= italic ? 1ULL : 0ULL;
    h *= 1099511628211ULL;
    for (unsigned char c : fontname) {   // 同文本不同字体不能共用缓存条目
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

namespace {

// 剥掉 字重/斜体 词缀 (DirectWrite 只认纯 family 名), 并查系统确认存在
void strip_font_suffixes(std::string& fam) {
    static const char* kSuffixes[] = {
        " Bold", " Italic", " Oblique", " Regular", " Light",
        " SemiBold", " DemiBold", " ExtraBold", " Black", " Thin",
        " Medium", " Heavy", " Book", " Ultra",
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* suf : kSuffixes) {
            size_t n = strlen(suf);
            if (fam.size() > n
                && _stricmp(fam.c_str() + fam.size() - n, suf) == 0) {
                fam.erase(fam.size() - n);
                while (!fam.empty() && fam.back() == ' ') fam.pop_back();
                changed = true;
                break;
            }
        }
    }
}

std::wstring utf8_to_wide_local(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    size_t i = 0, n = s.size();
    const auto* u = reinterpret_cast<const unsigned char*>(s.data());
    while (i < n) {
        uint32_t cp = 0;
        unsigned char c = u[i];
        if (c <= 0x7F) { cp = c; i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (u[i+1] & 0x3F); i += 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((u[i+1] & 0x3F) << 6) | (u[i+2] & 0x3F); i += 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((u[i+1] & 0x3F) << 12) | ((u[i+2] & 0x3F) << 6) | (u[i+3] & 0x3F); i += 4; }
        else { i += 1; continue; }
        if (cp <= 0xFFFF) w += static_cast<wchar_t>(cp);
        else { cp -= 0x10000; w += static_cast<wchar_t>(0xD800 + (cp >> 10)); w += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)); }
    }
    return w;
}

const wchar_t* kDefaultFamily = L"Microsoft YaHei";

} // anonymous namespace

// 返回真正要用的 family (UTF-16); fontname 空/未知 -> 默认
static std::wstring resolve_ass_font(IDWriteFactory* factory,
                                     const std::string& fontname) {
    std::string fam = fontname.empty() ? "Microsoft YaHei" : fontname;
    strip_font_suffixes(fam);
    if (fam.empty()) return kDefaultFamily;

    std::wstring fam16 = utf8_to_wide_local(fam);

    ComPtr<IDWriteFontCollection> collection;
    if (SUCCEEDED(factory->GetSystemFontCollection(&collection))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        collection->FindFamilyName(fam16.c_str(), &index, &exists);
        if (exists) return fam16;
        DANMAKU_LOG_WARN(LOG_TAG, "ASS 字体 \"%s\" 系统未安装, 回退默认", fam.c_str());
        return kDefaultFamily;
    }
    return fam16.empty() ? kDefaultFamily : fam16;
}

std::wstring GlyphCache::utf8_to_utf16(const std::string& text) {
    // 与匿名命名空间里的 utf8_to_wide_local 同实现, 直接复用,
    // 避免两份 UTF-8 解码逻辑各自漂移 (代理对/非法序列处理需一致)。
    return utf8_to_wide_local(text);
}

// ============================================================
// GlyphCache 构造 / 析构 / 初始化
// ============================================================

GlyphCache::GlyphCache() {}
GlyphCache::~GlyphCache() { clear(); }

ErrorCode GlyphCache::initialize(IDWriteFactory* dwrite_factory,
                                ID2D1Factory* d2d_factory) {
    if (!dwrite_factory) {
        DANMAKU_LOG_ERROR(LOG_TAG, "GlyphCache: dwrite_factory 为空");
        return ErrorCode::InvalidArgument;
    }
    dwrite_factory_ = dwrite_factory;
    if (d2d_factory) {
        d2d_factory_ = d2d_factory;
        DANMAKU_LOG_INFO(LOG_TAG, "GlyphCache 初始化完成 (容量=%zu, 轮廓几何=开)", max_entries_);
    } else {
        DANMAKU_LOG_WARN(LOG_TAG, "GlyphCache: 无 D2D factory, 不生成轮廓几何 (描边退化为多遍叠加)");
    }
    return ErrorCode::Ok;
}

bool GlyphCache::build_path_geometry(IDWriteTextLayout* layout,
                                    ComPtr<ID2D1Geometry>& out) {
    if (!layout || !d2d_factory_) return false;

    PathTextRenderer renderer(d2d_factory_);
    if (FAILED(layout->Draw(nullptr, &renderer, 0.0f, 0.0f))) return false;

    const auto& runs = renderer.runs();
    if (runs.empty()) return false;

    if (runs.size() == 1) {
        out = runs[0];
        return true;
    }

    // 多个 glyph run (混排/回退字体) -> 合并成一个几何组
    ComPtr<ID2D1GeometryGroup> group;
    HRESULT hr = d2d_factory_->CreateGeometryGroup(
        D2D1_FILL_MODE_WINDING,   // MinGW 头无 NONZERO 别名
        const_cast<ID2D1Geometry**>(runs.data()),
        static_cast<UINT32>(runs.size()), &group);
    if (FAILED(hr)) return false;

    out = group;
    return true;
}

const GlyphEntry* GlyphCache::get_or_create(const std::string& text,
                                          float font_size, bool bold,
                                          bool italic,
                                          const std::string& fontname) {
    if (!dwrite_factory_ || text.empty()) return nullptr;

    uint64_t key = hash_text(text, font_size, bold, italic, fontname);

    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_.find(key);
    if (it != cache_.end() && it->second.valid) {
        it->second.last_used = frame_counter_;
        // splice 把节点移到链表尾 (只改指针, 不分配/释放), 避免命中时反复 malloc/free
        lru_list_.splice(lru_list_.end(), lru_list_, it->second.lru_it);
        return &it->second;
    }

    while (cache_.size() >= max_entries_) evict_lru();

    GlyphEntry entry;
    entry.hash = key;
    entry.last_used = frame_counter_;

    if (!create_text_layout(entry, text, font_size, bold, italic, fontname)) {
        DANMAKU_LOG_WARN(LOG_TAG, "GlyphCache: 创建文本布局失败");
        return nullptr;
    }

    entry.valid = true;
    lru_list_.push_back(key);
    entry.lru_it = std::prev(lru_list_.end());
    auto result = cache_.emplace(key, std::move(entry));
    return &result.first->second;
}

void GlyphCache::touch(uint64_t key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end() || !it->second.valid) return;
    it->second.last_used = frame_counter_;
    lru_list_.splice(lru_list_.end(), lru_list_, it->second.lru_it);
}

bool GlyphCache::create_text_layout(GlyphEntry& entry,
                                    const std::string& text,
                                    float font_size, bool bold,
                                    bool italic,
                                    const std::string& fontname) {
    if (!dwrite_factory_) return false;

    IDWriteTextFormat* fmt = get_text_format(font_size, bold, italic, fontname);
    if (!fmt) {
        DANMAKU_LOG_ERROR(LOG_TAG, "GlyphCache: 无法创建 TextFormat");
        return false;
    }

    std::wstring text16 = utf8_to_utf16(text);
    if (text16.empty()) return false;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dwrite_factory_->CreateTextLayout(
        text16.c_str(), static_cast<UINT32>(text16.size()),
        fmt, 8192.0f, 256.0f, &layout);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "CreateTextLayout 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return false;
    }

    DWRITE_TEXT_METRICS metrics = {};
    hr = layout->GetMetrics(&metrics);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "GetMetrics 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return false;
    }

    entry.layout = layout;
    entry.width  = metrics.width;
    entry.height = metrics.height;

    build_path_geometry(layout.Get(), entry.path);
    return true;
}

IDWriteTextFormat* GlyphCache::get_text_format(float font_size, bool bold,
                                             bool italic,
                                             const std::string& fontname) {
    if (!dwrite_factory_) return nullptr;

    const std::wstring family = resolve_ass_font(dwrite_factory_, fontname);

    // key: family|size|bold|italic (family 以 wide 原样拼字节, 免编码转换)
    char buf[24];
    snprintf(buf, sizeof(buf), "|%d|%d|%d",
             static_cast<int>(font_size * 2.0f + 0.5f),
             bold ? 1 : 0, italic ? 1 : 0);
    std::string key;
    key.reserve(family.size() * 2 + 24);
    for (wchar_t wc : family) {
        key += static_cast<char>((wc >> 8) & 0xFF);
        key += static_cast<char>(wc & 0xFF);
    }
    key += buf;

    auto it = format_cache_.find(key);
    if (it != format_cache_.end()) return it->second.Get();

    ComPtr<IDWriteTextFormat> format;
    HRESULT hr = dwrite_factory_->CreateTextFormat(
        family.c_str(), nullptr,
        bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, font_size, L"zh-CN", &format);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "CreateTextFormat 失败 (HR=0x%08X, size=%.1f)",
                          static_cast<unsigned int>(hr), font_size);
        return nullptr;
    }

    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    auto result = format_cache_.emplace(key, std::move(format));
    return result.first->second.Get();
}

// LRU 淘汰: 跳过"本帧仍在使用"的条目 (渲染线程把 GlyphEntry* 缓存在弹幕上,
// 淘汰会让正在滚动的弹幕指向已释放内存)
void GlyphCache::evict_lru() {
    if (lru_list_.empty()) return;

    size_t attempts = lru_list_.size();
    while (attempts-- > 0) {
        const uint64_t key = lru_list_.front();
        auto it = cache_.find(key);
        if (it != cache_.end() && it->second.last_used == frame_counter_) {
            lru_list_.splice(lru_list_.end(), lru_list_, lru_list_.begin());
            continue;
        }
        cache_.erase(key);
        lru_list_.pop_front();
        return;
    }
}

void GlyphCache::clear() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.clear();
    lru_list_.clear();
    format_cache_.clear();
}

// ============================================================
// DanmakuEngine 构造 / 析构
// ============================================================

DanmakuEngine::DanmakuEngine() {}
DanmakuEngine::~DanmakuEngine() { shutdown(); }

// ============================================================
// initialize
// ============================================================

ErrorCode DanmakuEngine::initialize(HWND overlay_hwnd,
                                 const RenderConfig& config) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (!overlay_hwnd) {
        DANMAKU_LOG_ERROR(LOG_TAG, "DanmakuEngine: overlay_hwnd 为空");
        return ErrorCode::InvalidArgument;
    }

    config_ = config;

    DpiInfo dpi = platform::GetWindowDpi(overlay_hwnd);
    RECT rc;
    GetClientRect(overlay_hwnd, &rc);
    uint32_t width  = static_cast<uint32_t>(rc.right - rc.left);
    uint32_t height = static_cast<uint32_t>(rc.bottom - rc.top);
    if (width == 0)  width  = 1280;
    if (height == 0) height = 720;

    DANMAKU_LOG_INFO(LOG_TAG, "DanmakuEngine 初始化: %ux%u, DPI=%u",
                     width, height, dpi.dpi_x);

    ErrorCode ec = presenter_.initialize(overlay_hwnd, width, height, dpi);
    if (ec != ErrorCode::Ok) return ec;

    ec = glyph_cache_.initialize(presenter_.dwrite_factory(),
                               presenter_.d2d_factory());
    if (ec != ErrorCode::Ok) return ec;

    QueryPerformanceFrequency(&qpc_freq_);
    QueryPerformanceCounter(&clock_base_qpc_);
    frame_interval_ = 1.0 / static_cast<double>(config_.target_fps ? config_.target_fps : 60);
    frame_qpc_ = clock_base_qpc_;

    // 高分辨率帧节拍定时器: 让 wait_for_next_frame 精确睡到帧截止, 把忙等自旋
    // 从 ~1ms 压到 ~0.3ms, 高刷下尤其省 CPU。创建失败 (旧系统) 则用 Sleep 兜底。
    if (!frame_timer_) {
        frame_timer_ = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }

    initialized_ = true;
    DANMAKU_LOG_INFO(LOG_TAG, "DanmakuEngine 初始化完成 (layered ghost window)");
    return ErrorCode::Ok;
}

void DanmakuEngine::request_resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    resize_w_.store(width, std::memory_order_relaxed);
    resize_h_.store(height, std::memory_order_relaxed);
    resize_pending_.store(true, std::memory_order_release);
}

// ============================================================
// load_ass_file - 加载 ASS 文件
// ============================================================

ErrorCode DanmakuEngine::load_ass_file(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(engine_mutex_);

    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs.is_open()) {
        DANMAKU_LOG_ERROR(LOG_TAG, "无法打开 ASS 文件: %s", filepath.c_str());
        return ErrorCode::FileNotFound;
    }

    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();

    if (content.size() >= 3
        && static_cast<unsigned char>(content[0]) == 0xEF
        && static_cast<unsigned char>(content[1]) == 0xBB
        && static_cast<unsigned char>(content[2]) == 0xBF) {
        content = content.substr(3);   // 跳过 UTF-8 BOM
    }

    ErrorCode ec;
    ass_doc_ = AssParser::parse(content, &ec);
    if (ec != ErrorCode::Ok) return ec;

    for (auto& ev : ass_doc_.events) {
        ev.style = ass_doc_.find_style(ev.style_name);
    }

    // 整体透明度: 从 ASS 自身取 (ssdm 把 opt.opacity 写在 Style 颜色的 alpha 字节)。
    // 还原为"整层不透明度", 在合成时一次性施加到所有像素 (真正整体透明)。
    ass_alpha_ = 1.0f;
    for (const auto& st : ass_doc_.styles) {
        if (st.primary.a != 0) {
            ass_alpha_ = 1.0f - static_cast<float>(st.primary.a) / 255.0f;
            break;
        }
    }
    DANMAKU_LOG_INFO(LOG_TAG, "ASS 整体不透明度: %.3f (样式数=%zu)",
                     ass_alpha_, ass_doc_.styles.size());

    seek_cursor_ = 0;
    active_events_.clear();
    force_redraw_ = true;

    DANMAKU_LOG_INFO(LOG_TAG, "ASS 加载完成: %zu 条事件, %zu 个样式",
                     ass_doc_.events.size(), ass_doc_.styles.size());
    return ErrorCode::Ok;
}

D2D1_COLOR_F DanmakuEngine::map_ass_color(const AssColor& c, float alpha,
                                         bool hdr) const {
    return hdr
        ? presenter_.map_color_hdr_layer(c.r, c.g, c.b, alpha)
        : presenter_.map_color(c.r, c.g, c.b, alpha);
}

// ============================================================
// 内置 QPC 时钟 (时间同步核心)
// 媒体时间 = clock_base_time + (now_qpc - clock_base_qpc) * speed - delay
// delay 必须**减**: 正延迟 = 弹幕晚出现, 写 "+delay" 会让时钟超前 (方向反了)。
// 只在关键事件 sync*; 其余时间由 render() 用 QPC 自行推进, 不依赖 Lua 上报。
// ============================================================

double DanmakuEngine::clock_now() const {
    // 未校准 / 暂停 / 无 QPC 频率: 返回基准时间 (已减延迟), 时钟冻结。
    if (!clock_valid_ || clock_paused_ || qpc_freq_.QuadPart == 0)
        return clock_base_time_ - delay_;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = static_cast<double>(now.QuadPart - clock_base_qpc_.QuadPart) /
                    static_cast<double>(qpc_freq_.QuadPart);
    return clock_base_time_ + elapsed * clock_speed_ - delay_;
}

void DanmakuEngine::rebuild_active(TimePos t) {
    for (AssEvent* ev : active_events_) ev->active = false;
    active_events_.clear();

    // 前向游标: 第一条 start >= t 的事件
    auto it = std::lower_bound(
        ass_doc_.events.begin(), ass_doc_.events.end(), t,
        [](const AssEvent& e, TimePos tv) { return e.start < tv; });
    uint32_t cursor = static_cast<uint32_t>(it - ass_doc_.events.begin());

    // 回填"已开始但未结束"的弹幕 (按 (t-start)/(end-start) 插值, 滚到该在的位置)
    const TimePos kMaxLookBack = 30.0;
    for (uint32_t i = cursor; i > 0;) {
        --i;
        AssEvent& ev = ass_doc_.events[i];
        if (t - ev.start > kMaxLookBack) break;
        if (ev.end >= t) {
            ev.active = true;
            active_events_.push_back(&ev);
            if (config_.max_danmaku > 0
                && active_events_.size() >= config_.max_danmaku) break;
        }
    }

    seek_cursor_ = cursor;
}

void DanmakuEngine::sync(TimePos time_pos, double speed, bool paused) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (!initialized_) return;

    const bool   first = !clock_valid_;
    const double prev = first ? time_pos - delay_ : clock_now();

    clock_base_time_ = time_pos;
    clock_speed_     = speed > 0 ? speed : 1.0;
    clock_paused_    = paused;
    QueryPerformanceCounter(&clock_base_qpc_);
    clock_valid_ = true;
    last_time_pos_ = time_pos;
    force_redraw_ = true;

    // 只有时间轴真的跳变了 (seek/首次同步/漂移过大) 才重建活跃集合;
    // 暂停/倍速/常规校准 delta 很小, 保留正在滚动的弹幕。
    const double kReseekThreshold = 0.35;
    if (first || std::abs(time_pos - prev) > kReseekThreshold) {
        rebuild_active(time_pos - delay_);
    }
}

void DanmakuEngine::set_speed(double speed) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (!initialized_) return;
    if (speed <= 0) speed = 1.0;
    TimePos cur = clock_now();
    clock_base_time_ = cur;
    clock_speed_     = speed;
    QueryPerformanceCounter(&clock_base_qpc_);
}

void DanmakuEngine::set_paused(bool paused) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (!initialized_) return;
    if (paused == clock_paused_) return;
    if (paused) {
        clock_base_time_ = clock_now();
        clock_paused_    = true;
    } else {
        clock_base_time_ = clock_now();
        clock_paused_    = false;
    }
    QueryPerformanceCounter(&clock_base_qpc_);
    force_redraw_ = true;   // 暂停/恢复要补画一帧
}

void DanmakuEngine::set_delay(double seconds) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (!initialized_) return;
    delay_ = seconds;
    needs_reseek_  = true;   // 平移活跃集合 (保留滚动中弹幕, 视觉上是平移非闪断)
    force_redraw_ = true;
    DANMAKU_LOG_INFO(LOG_TAG, "弹幕延迟调整为 %.3f 秒", seconds);
}

// ============================================================
// render - 渲染弹幕
// ============================================================

bool DanmakuEngine::render() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (!initialized_ || !presenter_.context()) return false;

    // 处理主线程发来的尺寸变更请求 (实际重建只在渲染线程执行)
    if (resize_pending_.exchange(false, std::memory_order_acq_rel)) {
        if (presenter_.resize(resize_w_.load(std::memory_order_relaxed),
                              resize_h_.load(std::memory_order_relaxed))
            == ErrorCode::Ok) {
            force_redraw_ = true;
        }
    }

    presenter_.process_pending_hdr();

    if (needs_reseek_) {
        rebuild_active(clock_now());
        needs_reseek_ = false;
    }
    if (!render_enabled_) {
        // 禁用瞬间清掉窗口残留画面 (只清一次, 不每帧 Present):
        // 否则 enable=false 期间窗口一直保留最后画面, 恢复 enable 后
        // 会先闪现一帧旧状态。force_redraw_ 在 set_enabled(false) 时置位。
        if (force_redraw_ && presenter_.valid()) {
            force_redraw_ = false;
            HRESULT hr = presenter_.begin_frame();
            if (SUCCEEDED(hr)) {
                hr = presenter_.end_frame();
                if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
                    DANMAKU_LOG_WARN(LOG_TAG, "清屏 D2D 目标失效, 重建...");
                    presenter_.recreate_target();
                } else if (FAILED(hr)) {
                    DANMAKU_LOG_ERROR(LOG_TAG, "清屏 EndDraw 失败 (HR=0x%08X)",
                                      static_cast<unsigned int>(hr));
                }
                presenter_.present(ass_alpha_ * config_.opacity);
            }
        }
        return false;
    }

    // --- 用内置 QPC 时钟推进弹幕时间 (无 IPC 依赖) ---
    TimePos time_pos = clock_now();
    current_time_    = time_pos;

    // --- 1. 激活新弹幕 ---
    while (seek_cursor_ < ass_doc_.events.size()) {
        AssEvent& ev = ass_doc_.events[seek_cursor_];
        if (ev.start > time_pos) break;
        if (ev.end >= time_pos) {
            ev.active = true;
            active_events_.push_back(&ev);
            if (active_events_.size() >= config_.max_danmaku
                && config_.max_danmaku > 0) break;
        }
        ++seek_cursor_;
    }

    // --- 2. 清理已过期弹幕 ---
    active_events_.erase(
        std::remove_if(active_events_.begin(), active_events_.end(),
            [&](AssEvent* ev) {
                if (ev->end < time_pos) { ev->active = false; return true; }
                return false;
            }),
        active_events_.end());

    glyph_cache_.tick();

    // --- 3. 这一帧是否真的需要重画? ---
    // 弹幕位置只由媒体时间决定; 暂停时时间冻结 -> 整帧可跳过 (不读回+ULW)。
    // force_redraw_ 覆盖"配置/尺寸/暂停/seek/延迟"等会让画面变化的时刻。
    const bool time_advancing = clock_valid_ && !clock_paused_;
    if (!force_redraw_ && !time_advancing) return false;
    if (!force_redraw_ && active_events_.empty() && last_frame_empty_) return false;
    force_redraw_ = false;
    last_frame_empty_ = active_events_.empty();

    if (!presenter_.valid()) {
        DANMAKU_LOG_INFO(LOG_TAG, "D2D 目标无效, 尝试重建...");
        presenter_.recreate_target();
        if (!presenter_.valid()) {
            DANMAKU_LOG_WARN(LOG_TAG, "D2D 目标重建失败, 跳过渲染");
            return false;
        }
    }
    presenter_.begin_frame();

    // SDR: 直画 swap chain; HDR: 画进离屏 FP16 coverage 缓冲 (合成 pass 统一乘峰值)。
    // 两条路径共用同一套绘制代码, 只差"画到哪个上下文/用什么颜色"。
    const bool hdr = presenter_.is_hdr();
    ID2D1DeviceContext* ctx = hdr ? presenter_.hdr_scratch_context()
                                  : presenter_.context();
    ID2D1SolidColorBrush* text_brush = hdr ? presenter_.hdr_text_brush()
                                           : presenter_.text_brush();
    ID2D1SolidColorBrush* outline_brush = hdr ? presenter_.hdr_outline_brush()
                                              : presenter_.outline_brush();
    ID2D1SolidColorBrush* shadow_brush = hdr ? presenter_.hdr_shadow_brush()
                                             : presenter_.shadow_brush();
    if (!ctx || !text_brush) {
        presenter_.end_frame();
        return false;
    }

    // 预计算缩放系数 (本会话内恒定, 避免每事件重复除法/分支)
    const float sx = (ass_doc_.play_res_x == 0)
        ? 1.0f
        : static_cast<float>(presenter_.width()) / static_cast<float>(ass_doc_.play_res_x);
    const float sy = (ass_doc_.play_res_y == 0)
        ? 1.0f
        : static_cast<float>(presenter_.height()) / static_cast<float>(ass_doc_.play_res_y);

    for (auto* ev : active_events_) {
        if (!ev->active) continue;

        const AssStyle* style = ev->style;
        if (!style) continue;

        float font_size = style->fontsize;
        bool  bold      = style->bold;
        bool  italic    = style->italic;
        const AssColor* color         = &style->primary;
        const AssColor* outline_color = &style->outline_c;
        const AssColor* shadow_color  = &style->back_c;
        float outline_w = style->outline;
        float shadow_d  = style->shadow;

        if (ev->override_tags.has_fs)         font_size = ev->override_tags.font_size;
        if (ev->override_tags.has_bold)       bold      = ev->override_tags.bold;
        if (ev->override_tags.has_italic)     italic    = ev->override_tags.italic;
        if (ev->override_tags.has_color)      color     = &ev->override_tags.color;
        if (ev->override_tags.has_outline_color) outline_color = &ev->override_tags.outline_color;
        if (ev->override_tags.has_shadow_color)  shadow_color  = &ev->override_tags.shadow_color;
        if (ev->override_tags.has_bord)       outline_w = ev->override_tags.bord;
        if (ev->override_tags.has_shad)       shadow_d  = ev->override_tags.shad;

        font_size *= sy * config_.font_scale * 0.75f;
        outline_w *= sy * config_.font_scale;
        shadow_d  *= sy * config_.font_scale;

        // 只在"尚未解析"或"物理字号变了"时解析一次, 之后每帧直接复用。
        if (!ev->rt_glyph || ev->rt_font_size != font_size) {
            std::string fontname;
            if (ev->override_tags.has_fn && !ev->override_tags.fontname.empty()) {
                fontname = ev->override_tags.fontname;
            } else if (!style->fontname.empty()) {
                fontname = style->fontname;
            }
            ev->rt_glyph = glyph_cache_.get_or_create(
                ev->text, font_size, bold, italic, fontname);
            ev->rt_font_size = font_size;
        } else {
            glyph_cache_.touch(ev->rt_glyph->hash);   // 续 LRU 命, 防悬空指针
        }
        const GlyphEntry* entry = ev->rt_glyph;
        if (!entry || !entry->layout) continue;

        float draw_x = 0.0f, draw_y = 0.0f;

        if (ev->override_tags.has_move) {
            TimePos duration = ev->end - ev->start;
            if (duration <= 0) continue;
            double progress = (current_time_ - ev->start) / duration;
            if (progress < 0) progress = 0;
            if (progress > 1) progress = 1;

            float ass_x = static_cast<float>(
                ev->override_tags.move_x1
                + (ev->override_tags.move_x2 - ev->override_tags.move_x1) * progress);
            float ass_y = static_cast<float>(
                ev->override_tags.move_y1
                + (ev->override_tags.move_y2 - ev->override_tags.move_y1) * progress);
            draw_x = ass_x * sx;
            draw_y = ass_y * sy;
            // \move: x 减半宽 (整条弹幕完整进出); y 是顶部锚点, 不减半 height
            draw_x -= entry->width * 0.5f;
        } else if (ev->override_tags.has_pos) {
            draw_x = ev->override_tags.pos_x * sx;
            draw_y = ev->override_tags.pos_y * sy;
            int align = style->alignment;
            if (ev->override_tags.has_an) align = ev->override_tags.alignment;
            // ASS alignment 1-9: 7=左上,8=上中,9=右上,4=左中,5=中,6=右中,1=左下,2=下中,3=右下
            switch (align) {
                case 7: break;
                case 8: draw_x -= entry->width * 0.5f; break;
                case 9: draw_x -= entry->width; break;
                case 4: draw_y -= entry->height * 0.5f; break;
                case 5: draw_x -= entry->width * 0.5f; draw_y -= entry->height * 0.5f; break;
                case 6: draw_x -= entry->width; draw_y -= entry->height * 0.5f; break;
                case 1: draw_y -= entry->height; break;
                case 2: draw_x -= entry->width * 0.5f; draw_y -= entry->height; break;
                case 3: draw_x -= entry->width; draw_y -= entry->height; break;
            }
        } else {
            draw_x = 0; draw_y = 0;
        }

        // 只用 RGB, 不用颜色自带 alpha (整体透明度已在合成时统一施加)
        float alpha = 1.0f;
        if (ev->override_tags.has_alpha) {
            alpha = 1.0f - static_cast<float>(ev->override_tags.alpha) / 255.0f;
        }
        const D2D1_COLOR_F fc = map_ass_color(*color, alpha, hdr);
        const D2D1_COLOR_F oc = map_ass_color(*outline_color, alpha, hdr);

        // --- 描边 ---
        if (outline_w > 0.0f) {
            outline_brush->SetColor(oc);

            if (outline_w >= 0.75f && entry->path) {
                ctx->SetTransform(D2D1::Matrix3x2F::Translation(draw_x, draw_y));
                ctx->DrawGeometry(entry->path.Get(), outline_brush,
                                 outline_w * 2.0f, presenter_.stroke_style());
                ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            } else if (outline_w < 0.75f) {
                float offset = 1.0f;
                ctx->DrawTextLayout(D2D1::Point2F(draw_x, draw_y - offset), entry->layout.Get(), outline_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                ctx->DrawTextLayout(D2D1::Point2F(draw_x, draw_y + offset), entry->layout.Get(), outline_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                ctx->DrawTextLayout(D2D1::Point2F(draw_x - offset, draw_y), entry->layout.Get(), outline_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                ctx->DrawTextLayout(D2D1::Point2F(draw_x + offset, draw_y), entry->layout.Get(), outline_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
            } else {
                const float k = outline_w;
                const D2D1_POINT_2F offsets[8] = {
                    { -k, 0.0f }, { k, 0.0f }, { 0.0f, -k }, { 0.0f, k },
                    { -k, -k },   { k, -k },   { -k, k },    { k, k }
                };
                for (int i = 0; i < 8; ++i) {
                    ctx->DrawTextLayout(D2D1::Point2F(draw_x + offsets[i].x, draw_y + offsets[i].y),
                                        entry->layout.Get(), outline_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                }
            }
        }

        // --- 阴影 (depth > 0 时画, 近似 libass drop shadow) ---
        if (shadow_d > 0.25f) {
            const D2D1_COLOR_F sc = map_ass_color(*shadow_color, alpha, hdr);
            shadow_brush->SetColor(sc);
            float sx_off = shadow_d;
            float sy_off = shadow_d;
            ctx->DrawTextLayout(D2D1::Point2F(draw_x + sx_off, draw_y + sy_off), entry->layout.Get(), shadow_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
            ctx->DrawTextLayout(D2D1::Point2F(draw_x - sx_off * 0.4f, draw_y + sy_off), entry->layout.Get(), shadow_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
            ctx->DrawTextLayout(D2D1::Point2F(draw_x + sx_off, draw_y - sy_off * 0.4f), entry->layout.Get(), shadow_brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
        }

        // --- 正文 ---
        text_brush->SetColor(fc);
        ctx->DrawTextLayout(D2D1::Point2F(draw_x, draw_y), entry->layout.Get(),
                            text_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    HRESULT hr = presenter_.end_frame();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        DANMAKU_LOG_WARN(LOG_TAG, "D2D 需要重建目标");
        presenter_.recreate_target();
        return false;
    } else if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "EndDraw 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
    }
    presenter_.present(ass_alpha_ * config_.opacity);
    return true;
}

void DanmakuEngine::clear() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    active_events_.clear();
    ass_doc_.events.clear();
    ass_doc_.styles.clear();
    seek_cursor_ = 0;
    glyph_cache_.clear();
    last_frame_empty_ = false;
    force_redraw_     = true;
}

void DanmakuEngine::shutdown() {
    std::lock_guard<std::mutex> lock(engine_mutex_);

    if (frame_timer_) {     // 无论初始化是否走完, 都回收定时器句柄
        CloseHandle(frame_timer_);
        frame_timer_ = nullptr;
    }
    if (!initialized_) return;

    active_events_.clear();
    ass_doc_.events.clear();
    ass_doc_.styles.clear();
    seek_cursor_ = 0;
    glyph_cache_.clear();
    last_frame_empty_ = false;

    presenter_.shutdown();

    clock_valid_ = false;
    initialized_ = false;
}

// ============================================================
// 渲染开关 / 配置 (IPC 线程可调, 加锁)
// ============================================================

void DanmakuEngine::wait_for_next_frame() {
    if (qpc_freq_.QuadPart == 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG interval =
        static_cast<LONGLONG>(frame_interval_ * qpc_freq_.QuadPart);

    if (frame_qpc_.QuadPart == 0) frame_qpc_ = now;

    frame_qpc_.QuadPart += interval;

    // 卡顿超过约 3 帧时重新对时, 不连续追帧
    if (now.QuadPart - frame_qpc_.QuadPart > interval * 3) {
        frame_qpc_ = now;
        return;
    }

    const LONGLONG delta = frame_qpc_.QuadPart - now.QuadPart;
    if (delta <= 0) return;

    const double ms = static_cast<double>(delta) * 1000.0 / qpc_freq_.QuadPart;

    // 高分辨率定时器精确睡到帧截止前 ~0.3ms, 只自旋最后一小段。
    // 相比"Sleep(ms-1) + 近 1ms 忙等", 高刷下忙等 CPU 显著下降 (240Hz 约 28%->7%)。
    if (frame_timer_) {
        const double sleep_ms = ms - 0.3;
        if (sleep_ms > 0.0) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(sleep_ms * 10000.0);  // 100ns 单位
            if (SetWaitableTimer(frame_timer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(frame_timer_, 50);
            }
        }
    } else if (ms > 1.5) {
        Sleep(static_cast<DWORD>(ms - 1.0));
    }

    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < frame_qpc_.QuadPart);
}

void DanmakuEngine::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    render_enabled_ = enabled;
    needs_reseek_ = true;   // 重新启用时把时间轴对齐到当前媒体时间
    force_redraw_ = true;   // 禁用→渲染线程清屏一帧; 启用→强制重画一帧
}

bool DanmakuEngine::is_enabled() const {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    return render_enabled_;
}

void DanmakuEngine::apply_render_config(float opacity, float font_scale,
                                        uint32_t max_danmaku) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (opacity > 0 && std::fabs(config_.opacity - opacity) > 1e-4f) {
        config_.opacity = opacity;
        force_redraw_ = true;
    }
    if (font_scale > 0) {
        if (std::fabs(config_.font_scale - font_scale) > 1e-4f) force_redraw_ = true;
        config_.font_scale = font_scale;
    }
    if (max_danmaku > 0) config_.max_danmaku = max_danmaku;
}

void DanmakuEngine::set_hdr_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    presenter_.request_hdr_enabled(enabled);
    force_redraw_ = true;
    DANMAKU_LOG_INFO(LOG_TAG, "HDR 开关请求: %s", enabled ? "开启" : "关闭");
}

void DanmakuEngine::set_hdr_peak(float peak) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    presenter_.request_hdr_peak(peak);
    force_redraw_ = true;
    DANMAKU_LOG_INFO(LOG_TAG, "HDR 峰值亮度请求: %.1f nits", peak);
}

uint32_t DanmakuEngine::get_active_count() const {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    return static_cast<uint32_t>(active_events_.size());
}

RenderConfig DanmakuEngine::get_config() const {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    return config_;
}

} // namespace danmaku_overlay
