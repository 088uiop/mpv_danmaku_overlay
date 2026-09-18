#pragma once

// ass_parser.h - ASS 字幕文件解析器 (纯逻辑, 不依赖 D3D/D2D/时钟/线程)
// danmaku_overlay - 独立 ASS 弹幕叠加渲染器

#include "common/types.h"

namespace danmaku_overlay {

// AssParser - ASS 解析器 (无状态, 全静态方法)
// 解析三段 (Script Info / V4+ Styles / Events) 与覆盖标签
// (\move, \pos, \c, \alpha, \fs, \fn, \b, \i, \an, ...)。
class AssParser {
public:
    // 解析 ASS 文本 (UTF-8 内容), 返回完整文档; 失败时经 ec 返回错误码。
    static AssDocument parse(const std::string& content,
                             ErrorCode* ec = nullptr);

private:
    static void parse_script_info(AssDocument& doc, const std::string& line);
    static void parse_styles(AssDocument& doc, const std::string& line);
    static void parse_events(AssDocument& doc, const std::string& line);
    static void parse_style_line(AssDocument& doc, const std::string& line);
    static void parse_dialogue_line(AssDocument& doc, const std::string& line);
    static AssOverride parse_override_tags(std::string& text);
    static TimePos parse_ass_time(const std::string& time_str);
    static AssColor parse_ass_color(const std::string& color_str);
    static std::vector<std::string> split_csv(const std::string& line,
                                             int max_fields = -1);
};

} // namespace danmaku_overlay
