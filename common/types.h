#pragma once

// ============================================================
// types.h - 项目通用类型定义
// danmaku_overlay - 独立 ASS 弹幕叠加渲染器
// ============================================================
// 架构变更: lua 脚本(ssdm.lua)负责弹幕解析+轨道分配+ASS 生成,
// C++ 引擎只解析 ASS 文件并渲染, 不再做碰撞检测。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <array>

namespace danmaku_overlay {

// 字形布局缓存条目 (完整定义在 render/danmaku_engine.h 的 GlyphCache)。
// 这里只做前向声明, 让 AssEvent 能持有"渲染期缓存"指针,
// 避免每帧重新解析文本布局。
struct GlyphEntry;

// ============================================================
// 时间类型
// ============================================================
using TimePos = double;                    // 播放时间 (秒)

// ============================================================
// ASS 颜色: ABGR 格式 (ASS 使用 &HAABBGGRR)
// ============================================================
struct AssColor {
    uint8_t a = 0;   // Alpha (0=不透明, 255=透明)
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 255;

    // 从 ASS 十进制颜色值解析 (ASS 颜色为 &HAABBGGRR 的十进制)
    static AssColor from_ass_uint(uint32_t v) {
        AssColor c;
        c.a = static_cast<uint8_t>((v >> 24) & 0xFF);
        c.b = static_cast<uint8_t>((v >> 16) & 0xFF);
        c.g = static_cast<uint8_t>((v >> 8)  & 0xFF);
        c.r = static_cast<uint8_t>(v & 0xFF);
        return c;
    }

    // 转为 RGBA 浮点 (0-1, alpha 取反因为 ASS 0=不透明)
    // 调用方用 D2D1::ColorF(r, g, b, a) 构造
    struct FloatColor {
        float r, g, b, a;
    };
    FloatColor to_float(float global_opacity = 1.0f) const {
        FloatColor fc;
        fc.a = (1.0f - static_cast<float>(a) / 255.0f) * global_opacity;
        fc.r = static_cast<float>(r) / 255.0f;
        fc.g = static_cast<float>(g) / 255.0f;
        fc.b = static_cast<float>(b) / 255.0f;
        return fc;
    }
};

// ============================================================
// ASS Style (V4+ Style 行)
// ============================================================
struct AssStyle {
    std::string name;           // 样式名 (R2L, TOP, BTM 等)
    std::string fontname;       // 字体名
    float       fontsize = 0;   // 字号
    AssColor    primary;        // 主色
    AssColor    secondary;      // 次色
    AssColor    outline_c;      // 描边色
    AssColor    back_c;         // 背景色
    bool        bold = false;
    bool        italic = false;
    float       outline = 2.0f; // 描边宽度
    float       shadow = 0;     // 阴影深度
    int         alignment = 0;  // ASS 对齐 (1-9, numpad)
};
// ============================================================
// ASS 覆盖标签 (从 Dialogue 行的 {...} 中解析)
// ============================================================
struct AssOverride {
    // \move(x1,y1,x2,y2) - 滚动弹幕
    bool  has_move = false;
    float move_x1 = 0, move_y1 = 0;
    float move_x2 = 0, move_y2 = 0;

    // \pos(x,y) - 固定弹幕
    bool  has_pos = false;
    float pos_x = 0, pos_y = 0;

    // \c&HBBGGRR& 或 \1c&HBBGGRR& - 主色覆盖
    bool  has_color = false;
    AssColor color;

    // \2c&HBBGGRR& - 次色覆盖 (卡拉 OK 渐变用)
    bool  has_secondary = false;
    AssColor secondary;

    // \3c&HBBGGRR& - 描边色覆盖
    bool  has_outline_color = false;
    AssColor outline_color;

    // \4c&HBBGGRR& - 阴影色覆盖
    bool  has_shadow_color = false;
    AssColor shadow_color;

    // \alpha&HAA& 或 \1a - 透明度覆盖
    bool  has_alpha = false;
    uint8_t alpha = 0;

    // \fs<size> - 字号覆盖
    bool  has_fs = false;
    float font_size = 0;

    // \b<0/1> - 粗体覆盖
    bool  has_bold = false;
    bool  bold = false;

    // \i<0/1> - 斜体覆盖
    bool  has_italic = false;
    bool  italic = false;

    // \bord<size> - 描边宽度覆盖 (像素)
    bool  has_bord = false;
    float bord = 0;

    // \shad<depth> - 阴影深度覆盖
    bool  has_shad = false;
    float shad = 0;

    // \fn<name> - 字体覆盖
    bool  has_fn = false;
    std::string fontname;

    // \an<n> - 对齐覆盖 (1-9 numpad)
    bool  has_an = false;
    int   alignment = 0;

    // \fad(t1,t2) / \fade(t1,t2,t3,t4) - 淡入淡出 (高级, 当前未渲染但解析保留)
    bool  has_fade = false;
    float fade_in  = 0.0f;  // 起始淡入持续 (ms)
    float fade_out = 0.0f;  // 结束淡出持续 (ms)
};

// ============================================================
// ASS Dialogue 事件 (单条弹幕)
// ============================================================
struct AssEvent {
    int           layer = 0;       // Layer 字段
    TimePos       start = 0.0;     // 开始时间 (秒)
    TimePos       end = 0.0;       // 结束时间 (秒)
    std::string   style_name;      // 样式名
    std::string   text;            // 文本内容 (已去除覆盖标签的纯文本)
    AssOverride   override_tags;   // 解析出的覆盖标签

    // 运行时
    const AssStyle* style = nullptr; // 指向样式的指针 (解析后填充)
    bool  active = false;             // 是否在当前时间窗口内

    // --- 渲染期缓存 (仅渲染线程读写) ---
    // 弹幕文本与样式在存活期内不变, 文本布局只需要在"激活"时解析一次,
    // 之后每帧直接复用, 省掉每帧的: 字体名字符串构造 + 全文 FNV 哈希 +
    // 缓存查找时的加锁与 LRU 维护。
    // rt_font_size 记录该布局对应的物理字号; 窗口缩放 / 字体缩放导致字号
    // 变化时才重新解析。
    const GlyphEntry* rt_glyph     = nullptr;  // 已解析的文本布局
    float             rt_font_size = -1.0f;    // <0 表示尚未解析
};

// ============================================================
// ASS 文档 (完整解析结果)
// ============================================================
struct AssDocument {
    uint32_t play_res_x = 1920;       // PlayResX
    uint32_t play_res_y = 1080;       // PlayResY
    std::vector<AssStyle>  styles;    // 样式表
    std::vector<AssEvent>  events;    // 事件列表 (按时间排序)

    // 按名称查找样式
    const AssStyle* find_style(const std::string& name) const {
        for (auto& s : styles) {
            if (s.name == name) return &s;
        }
        return nullptr;
    }
};

// ============================================================
// 渲染配置
// ============================================================
struct RenderConfig {

    uint32_t target_fps = 60;
    bool     vsync = true;

    float    opacity = 1.0f;          // 全局透明度
    float    font_scale = 1.0f;       // 全局字体缩放
    uint32_t max_danmaku = 200;       // 同屏最大弹幕数 (0=不限)
    bool     bold = false;            // 全局加粗

    uint32_t width = 1280;
    uint32_t height = 720;
    bool     borderless = true;

    RenderConfig() = default;
};

// ============================================================
// 窗口 DPI 信息
// ============================================================
struct DpiInfo {
    uint32_t dpi_x = 96;
    uint32_t dpi_y = 96;
    float    scale_x = 1.0f;
    float    scale_y = 1.0f;

    void update() {
        scale_x = static_cast<float>(dpi_x) / 96.0f;
        scale_y = static_cast<float>(dpi_y) / 96.0f;
    }
};

// ============================================================
// 回调函数类型
// ============================================================

// ============================================================
// 错误码
// ============================================================
enum class ErrorCode : int32_t {
    Ok              = 0,
    WindowFailed    = -3,
    D3D11Failed     = -4,
    D2DFailed       = -5,
    InvalidArgument = -6,
    FileNotFound    = -7,
    ParseError      = -8,
};

inline const char* error_string(ErrorCode ec) {
    switch (ec) {
        case ErrorCode::Ok:               return "成功";
        case ErrorCode::WindowFailed:     return "窗口创建失败";
        case ErrorCode::D3D11Failed:      return "Direct3D 11 初始化失败";
        case ErrorCode::D2DFailed:        return "Direct2D 初始化失败";
        case ErrorCode::InvalidArgument:  return "参数无效";
        case ErrorCode::FileNotFound:     return "文件未找到";
        case ErrorCode::ParseError:       return "解析错误";
        default:                          return "未知错误";
    }
}

} // namespace danmaku_overlay
