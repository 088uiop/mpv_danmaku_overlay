// ass_parser.cpp - ASS 字幕文件解析器实现
// danmaku_overlay - 独立 ASS 弹幕叠加渲染器

#include "ass_parser.h"
#include "common/log.h"

#include <algorithm>
#include <sstream>

namespace danmaku_overlay {

// parse - 按行扫描, 跟踪当前段, 分发到对应段解析器
AssDocument AssParser::parse(const std::string& content, ErrorCode* ec) {
    AssDocument doc;
    if (ec) *ec = ErrorCode::Ok;

    std::string current_section;
    std::istringstream iss(content);
    std::string line;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            continue;
        }

        if (current_section == "Script Info") {
            parse_script_info(doc, line);
        } else if (current_section == "V4+ Styles"
                || current_section == "V4 Styles") {
            parse_styles(doc, line);
        } else if (current_section == "Events") {
            parse_events(doc, line);
        }
    }

    std::sort(doc.events.begin(), doc.events.end(),
              [](const AssEvent& a, const AssEvent& b) {
                  return a.start < b.start;
              });

    if (doc.events.empty()) {
        DANMAKU_LOG_WARN(LOG_TAG, "ASS 文件中未找到任何事件");
    }

    return doc;
}

void AssParser::parse_script_info(AssDocument& doc, const std::string& line) {
    // 只关心 PlayResX / PlayResY
    if (line.compare(0, 9, "PlayResX:") == 0) {
        doc.play_res_x = static_cast<uint32_t>(std::stoul(line.substr(9)));
    } else if (line.compare(0, 9, "PlayResY:") == 0) {
        doc.play_res_y = static_cast<uint32_t>(std::stoul(line.substr(9)));
    }
}

void AssParser::parse_styles(AssDocument& doc, const std::string& line) {
    if (line.compare(0, 7, "Format:") == 0) return;
    if (line.compare(0, 6, "Style:") == 0) {
        parse_style_line(doc, line);
    }
}

// ssdm.lua 输出格式:
// Style: R2L,Microsoft YaHei,50,&H00FFFFFF&,...,0,0,0,1,2,0,7,0,0,0,1
void AssParser::parse_style_line(AssDocument& doc, const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return;
    std::string rest = line.substr(colon + 1);
    size_t start = rest.find_first_not_of(" \t");
    if (start != std::string::npos) rest = rest.substr(start);

    auto fields = split_csv(rest, 24);
    if (fields.size() < 6) return;

    AssStyle style;
    style.name      = fields[0];
    style.fontname  = fields[1];
    style.fontsize  = std::stof(fields[2]);
    style.primary   = parse_ass_color(fields[3]);
    style.secondary = parse_ass_color(fields[4]);
    style.outline_c = parse_ass_color(fields[5]);
    if (fields.size() > 6)
        style.back_c = parse_ass_color(fields[6]);
    if (fields.size() > 7)
        style.bold = (std::stoi(fields[7]) != 0);
    if (fields.size() > 8)
        style.italic = (std::stoi(fields[8]) != 0);
    // V4+ Styles 字段 (0-based): 16=Outline 17=Shadow 18=Alignment
    if (fields.size() > 16) style.outline = std::stof(fields[16]);
    if (fields.size() > 17) style.shadow  = std::stof(fields[17]);
    if (fields.size() > 18) style.alignment = std::stoi(fields[18]);

    doc.styles.push_back(std::move(style));
}

void AssParser::parse_events(AssDocument& doc, const std::string& line) {
    if (line.compare(0, 7, "Format:") == 0) return;
    if (line.compare(0, 8, "Dialogue") == 0 && line[8] == ':') {
        parse_dialogue_line(doc, line);
    }
}

// ssdm.lua 输出格式:
// Dialogue: 0,0:00:01.50,0:00:16.50,R2L,,0,0,0,,{\move(1920,0,0,0)}text
void AssParser::parse_dialogue_line(AssDocument& doc, const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return;
    std::string rest = line.substr(colon + 1);
    size_t start = rest.find_first_not_of(" \t");
    if (start != std::string::npos) rest = rest.substr(start);

    // 10 个字段: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
    // Text 字段可能含逗号, 故最多分割到 10 段
    auto fields = split_csv(rest, 10);
    if (fields.size() < 10) return;

    AssEvent ev;
    ev.layer      = std::stoi(fields[0]);
    ev.start      = parse_ass_time(fields[1]);
    ev.end        = parse_ass_time(fields[2]);
    ev.style_name = fields[3];

    // split_csv 已把第 10 个逗号之后 (含逗号) 整段归入 fields[9], 无需再拼回。
    std::string text = fields[9];

    ev.override_tags = parse_override_tags(text);
    ev.text = text;

    doc.events.push_back(std::move(ev));
}

// parse_override_tags - 提取 {...} 块内的覆盖标签, 并从文本中移除
AssOverride AssParser::parse_override_tags(std::string& text) {
    AssOverride ov;

    size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] != '{') break;

        size_t end = text.find('}', pos);
        if (end == std::string::npos) break;

        std::string block = text.substr(pos + 1, end - pos - 1);

        // \move(x1,y1,x2,y2[,t1,t2])
        size_t mv = block.find("\\move(");
        if (mv != std::string::npos) {
            size_t close = block.find(')', mv);
            if (close != std::string::npos) {
                std::string args = block.substr(mv + 6, close - mv - 6);
                std::vector<std::string> parts;
                std::istringstream iss(args);
                std::string part;
                while (std::getline(iss, part, ',')) parts.push_back(part);
                if (parts.size() >= 4) {
                    ov.has_move = true;
                    ov.move_x1 = std::stof(parts[0]);
                    ov.move_y1 = std::stof(parts[1]);
                    ov.move_x2 = std::stof(parts[2]);
                    ov.move_y2 = std::stof(parts[3]);
                }
            }
        }

        // \pos(x,y)
        size_t p = block.find("\\pos(");
        if (p != std::string::npos) {
            size_t close = block.find(')', p);
            if (close != std::string::npos) {
                std::string args = block.substr(p + 5, close - p - 5);
                size_t comma = args.find(',');
                if (comma != std::string::npos) {
                    ov.has_pos = true;
                    ov.pos_x = std::stof(args.substr(0, comma));
                    ov.pos_y = std::stof(args.substr(comma + 1));
                }
            }
        }

        // 通用 \Nc&H......& 颜色标签 (\c/\1c 主色 \2c 次色 \3c 描边 \4c 阴影)
        // 用 "标签+&" 作搜索模式, 天然排除 \clip 等同前缀标签
        auto try_color_tag = [&](const std::string& pat, int slot) -> bool {
            size_t p = block.find(pat);
            if (p == std::string::npos) return false;
            size_t amp = p + pat.size() - 1;
            if (amp >= block.size() || block[amp] != '&') return false;
            size_t end = block.find('&', amp + 1);
            if (end == std::string::npos) return false;
            AssColor c = parse_ass_color(block.substr(amp, end - amp + 1));
            switch (slot) {
                case 1: ov.color = c;         ov.has_color = true; break;
                case 2: ov.secondary = c;     ov.has_secondary = true; break;
                case 3: ov.outline_color = c; ov.has_outline_color = true; break;
                case 4: ov.shadow_color = c;  ov.has_shadow_color = true; break;
                default: return false;
            }
            return true;
        };

        try_color_tag("\\c&",  1);
        try_color_tag("\\1c&", 1);
        try_color_tag("\\2c&", 2);
        try_color_tag("\\3c&", 3);
        try_color_tag("\\4c&", 4);

        // \alpha&HAA&  - 全局透明度 (AA 在 &H 之后、结尾 & 之前)
        size_t a = block.find("\\alpha&H");
        if (a != std::string::npos) {
            size_t val_start = a + 7;   // 跳过 "\alpha&H"
            size_t amp = block.find('&', val_start);   // 结尾的 &
            if (amp != std::string::npos) {
                std::string alpha_str = block.substr(val_start, amp - val_start);
                if (!alpha_str.empty()) {
                    ov.has_alpha = true;
                    ov.alpha = static_cast<uint8_t>(
                        std::stoul(alpha_str, nullptr, 16));
                }
            }
        }

        // \fs<size>
        size_t fs = block.find("\\fs");
        if (fs != std::string::npos) {
            size_t num_start = fs + 3;
            size_t num_end = num_start;
            while (num_end < block.size()
                   && (isdigit(static_cast<unsigned char>(block[num_end]))
                       || block[num_end] == '.')) {
                ++num_end;
            }
            if (num_end > num_start) {
                ov.has_fs = true;
                ov.font_size = std::stof(block.substr(num_start, num_end - num_start));
            }
        }

        // \b<0/1>  - 粗体
        size_t b = block.find("\\b");
        if (b != std::string::npos) {
            size_t num_start = b + 2;
            if (num_start < block.size()
                && (block[num_start] == '0' || block[num_start] == '1')) {
                ov.has_bold = true;
                ov.bold = (block[num_start] == '1');
            }
        }

        // \i<0/1>  - 斜体 (单独 \i 等同 \i1)
        size_t it = block.find("\\i");
        if (it != std::string::npos) {
            size_t num_start = it + 2;
            if (num_start < block.size()
                && (block[num_start] == '0' || block[num_start] == '1')) {
                ov.has_italic = true;
                ov.italic = (block[num_start] == '1');
            } else {
                ov.has_italic = true;
                ov.italic = true;
            }
        }

        // \an<n>  - 对齐覆盖 (1-9, numpad)
        {
            size_t p = block.find("\\an");
            if (p != std::string::npos) {
                size_t num_start = p + 3;
                size_t num_end = num_start;
                while (num_end < block.size()
                       && isdigit(static_cast<unsigned char>(block[num_end]))) {
                    ++num_end;
                }
                if (num_end > num_start) {
                    int n = std::stoi(block.substr(num_start, num_end - num_start));
                    if (n >= 1 && n <= 9) {
                        ov.has_an = true;
                        ov.alignment = n;
                    }
                }
            }
        }

        // \bord<size>  - 描边宽度覆盖
        {
            size_t p = block.find("\\bord");
            if (p != std::string::npos) {
                size_t num_start = p + 5;
                size_t num_end = num_start;
                while (num_end < block.size()
                       && (isdigit(static_cast<unsigned char>(block[num_end]))
                           || block[num_end] == '.')) {
                    ++num_end;
                }
                if (num_end > num_start) {
                    ov.has_bord = true;
                    ov.bord = std::stof(block.substr(num_start, num_end - num_start));
                }
            }
        }

        // \shad<depth>  - 阴影深度覆盖
        {
            size_t p = block.find("\\shad");
            if (p != std::string::npos) {
                size_t num_start = p + 5;
                size_t num_end = num_start;
                while (num_end < block.size()
                       && (isdigit(static_cast<unsigned char>(block[num_end]))
                           || block[num_end] == '.' || block[num_end] == '-')) {
                    ++num_end;
                }
                if (num_end > num_start) {
                    ov.has_shad = true;
                    ov.shad = std::stof(block.substr(num_start, num_end - num_start));
                }
            }
        }

        // \fad(t1,t2)  - 淡入淡出 (仅解析, 渲染时按需使用)
        {
            size_t p = block.find("\\fad(");
            if (p != std::string::npos) {
                size_t close = block.find(')', p);
                if (close != std::string::npos) {
                    std::string args = block.substr(p + 5, close - p - 5);
                    size_t comma = args.find(',');
                    if (comma != std::string::npos) {
                        try {
                            ov.fade_in  = std::stof(args.substr(0, comma)) / 1000.0f;
                            ov.fade_out = std::stof(args.substr(comma + 1)) / 1000.0f;
                            ov.has_fade = true;
                        } catch (...) { }
                    }
                }
            }
        }

        // \fn<name>
        size_t fn = block.find("\\fn");
        if (fn != std::string::npos) {
            size_t name_start = fn + 3;
            size_t name_end = name_start;
            while (name_end < block.size() && block[name_end] != '\\'
                   && block[name_end] != '}') {
                ++name_end;
            }
            if (name_end > name_start) {
                ov.has_fn = true;
                ov.fontname = block.substr(name_start, name_end - name_start);
            }
        }

        text.erase(pos, end - pos + 1);
    }

    while (!text.empty() && text[0] == ' ') text.erase(0, 1);

    return ov;
}

// H:MM:SS.cc -> 秒
TimePos AssParser::parse_ass_time(const std::string& time_str) {
    size_t colon1 = time_str.find(':');
    if (colon1 == std::string::npos) return 0.0;

    int h = std::stoi(time_str.substr(0, colon1));

    size_t colon2 = time_str.find(':', colon1 + 1);
    if (colon2 == std::string::npos) return static_cast<double>(h);

    int m = std::stoi(time_str.substr(colon1 + 1, colon2 - colon1 - 1));
    double s = std::stod(time_str.substr(colon2 + 1));

    return h * 3600.0 + m * 60.0 + s;
}

// &HBBGGRR 或 &HAABBGGRR -> AssColor
AssColor AssParser::parse_ass_color(const std::string& color_str) {
    AssColor c;
    std::string hex;
    size_t start = 0;
    if (color_str.compare(0, 2, "&H") == 0
        || color_str.compare(0, 2, "&h") == 0) {
        start = 2;
    }
    size_t end = color_str.size();
    if (end > start && color_str[end - 1] == '&') --end;

    hex = color_str.substr(start, end - start);

    if (hex.size() >= 6) {
        if (hex.size() >= 8) {
            c.a = static_cast<uint8_t>(std::stoul(hex.substr(0, 2), nullptr, 16));
            c.b = static_cast<uint8_t>(std::stoul(hex.substr(2, 2), nullptr, 16));
            c.g = static_cast<uint8_t>(std::stoul(hex.substr(4, 2), nullptr, 16));
            c.r = static_cast<uint8_t>(std::stoul(hex.substr(6, 2), nullptr, 16));
        } else {
            c.b = static_cast<uint8_t>(std::stoul(hex.substr(0, 2), nullptr, 16));
            c.g = static_cast<uint8_t>(std::stoul(hex.substr(2, 2), nullptr, 16));
            c.r = static_cast<uint8_t>(std::stoul(hex.substr(4, 2), nullptr, 16));
        }
    }
    return c;
}

// 分割逗号分隔字段 (考虑大括号内不分割)
std::vector<std::string> AssParser::split_csv(const std::string& line,
                                                int max_fields) {
    std::vector<std::string> fields;
    std::string current;
    int brace_depth = 0;
    int count = 0;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (c == '{') {
            ++brace_depth;
            current += c;
        } else if (c == '}') {
            --brace_depth;
            current += c;
        } else if (c == ',' && brace_depth == 0
                   && (max_fields < 0 || count < max_fields - 1)) {
            fields.push_back(current);
            current.clear();
            ++count;
        } else {
            current += c;
        }
    }
    fields.push_back(current);
    return fields;
}

} // namespace danmaku_overlay
