#pragma once

// 真实语料校验工具的「语义字段」定义与 MediaInfo 文本抽取。
//
// 为什么单独成头文件（且保持纯 std、不 include libav*）:
//   1) tools/media_corpus_validate 默认不构建（要外部几 GB 语料树），但解析逻辑
//      必须有 PR 流程跑得起的单测 —— 见 tests/unit/test_media_info_fields.cpp；
//   2) mi_* 与 truth_* 两列组共用同一套字段定义和列顺序，避免"加字段时只改了一边"；
//   3) 列顺序是有语义的（validate_corpus.py / corpus_semantic_gate.py 按列名读，
//      但表头与数据行的顺序必须逐列对齐），所以统一由 FieldNames() 一处钉死。
//
// 取值口径: 只取 General 的第一条、第一条 Video、第一条 Audio，与 truth 侧一致。
// 文本里 "-" / 缺失一律落 0 或空串，门禁侧把「没有事实」与「值为 0」同等对待
// （compare_field 返回 None 跳过），不会出现拿 0 去比真值造成的假判红。

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace corpus_fields {

// 23 个可比对字段，顺序即 TSV 列顺序。前 7 个是切片 1 已有的列（保持原位置），
// 后 16 个是本切片新增、一律追加到表尾。
inline const std::vector<std::string>& FieldNames() {
    static const std::vector<std::string> names = {
        "duration_s",     "width",      "height",       "video_codec",         "audio_codec",       "sample_rate",
        "channels",       "format",     "stream_count", "overall_bitrate_bps", "video_profile",     "video_level",
        "pix_fmt",        "frame_rate", "frame_count",  "video_bitrate_bps",   "color_space",       "color_primaries",
        "color_transfer", "rotation",   "sample_fmt",   "bit_depth",           "audio_bitrate_bps",
    };
    return names;
}

inline const std::vector<std::string>& ExtendedFieldNames() {
    static const std::vector<std::string> names(FieldNames().begin() + 7, FieldNames().end());
    return names;
}

// 切片 1 定下的历史表头前 7 个 mi_* 列顺序。它**不是** FieldNames() 的前 7 项 ——
// 当年是 duration, video_codec, width, height, audio_codec, sample_rate, channels。
// 表头前 29 列一个都不能动（历史 results.tsv 与人工诊断按列号读），所以
// ProcessOne 必须按这里给的索引顺序从 ToColumns() 里取值。
// 实测踩过：直接 for(i<7) add(columns[i]) 会把 mi_video_codec 写进 mi_width，
// 编译与运行都不报错，只有真值比对才发现整批字段串位。
inline const std::vector<std::string>& LegacyMiFieldNames() {
    static const std::vector<std::string> names = {
        "duration_s", "video_codec", "width", "height", "audio_codec", "sample_rate", "channels",
    };
    return names;
}

inline const std::vector<size_t>& LegacyMiColumnOrder() {
    static const std::vector<size_t> order = {0, 3, 1, 2, 4, 5, 6};
    return order;
}

struct SemanticFields {
    double duration_s = 0.0;
    int width = 0;
    int height = 0;
    std::string video_codec;
    std::string audio_codec;
    int sample_rate = 0;
    int channels = 0;
    std::string format;
    int stream_count = 0;
    long long overall_bitrate_bps = 0;
    std::string video_profile;
    int video_level = 0;
    std::string pix_fmt;
    double frame_rate = 0.0;
    long long frame_count = 0;
    long long video_bitrate_bps = 0;
    std::string color_space;
    std::string color_primaries;
    std::string color_transfer;
    double rotation = 0.0;
    std::string sample_fmt;
    int bit_depth = 0;
    long long audio_bitrate_bps = 0;
};

using MediaInfoFields = SemanticFields;
using TruthFields = SemanticFields;

inline std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return "";
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

inline double ParseLeadingDouble(const std::string& text) {
    return std::strtod(text.c_str(), nullptr);
}

inline long long ParseLeadingInt(const std::string& text) {
    return std::strtoll(text.c_str(), nullptr, 10);
}

// "1 min 27 s 336 ms" / "678 ms" -> 秒。MediaInfoAnalyzer::FormatDuration 的逆运算。
inline double ParseHumanDuration(const std::string& text) {
    double seconds = 0.0;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && text[pos] == ' ')
            ++pos;
        const size_t number_start = pos;
        while (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.'))
            ++pos;
        if (number_start == pos)
            break;
        const double value = std::atof(text.substr(number_start, pos - number_start).c_str());
        while (pos < text.size() && text[pos] == ' ')
            ++pos;
        const size_t unit_start = pos;
        while (pos < text.size() && std::isalpha(static_cast<unsigned char>(text[pos])))
            ++pos;
        const std::string unit = text.substr(unit_start, pos - unit_start);
        if (unit == "h")
            seconds += value * 3600.0;
        else if (unit == "min")
            seconds += value * 60.0;
        else if (unit == "s")
            seconds += value;
        else if (unit == "ms")
            seconds += value / 1000.0;
        else
            break;
    }
    return seconds;
}

// FormatBitRate 的逆运算："1.2 Mb/s" -> 1200000，"640.0 kb/s" -> 640000，
// "123 b/s" -> 123，"-" -> 0。注意 Mb/s 里也含 "b/s"，单位判定必须从长到短。
// 格式化时 Fixed(...,1) 已把精度压到 0.1 单位，因此门禁侧对码率用相对容差。
inline long long ParseBitRate(const std::string& text) {
    const double value = ParseLeadingDouble(text);
    if (text.find("Mb/s") != std::string::npos)
        return static_cast<long long>(value * 1000000.0 + 0.5);
    if (text.find("kb/s") != std::string::npos)
        return static_cast<long long>(value * 1000.0 + 0.5);
    if (text.find("b/s") != std::string::npos)
        return static_cast<long long>(value + 0.5);
    return 0;
}

// "yuv420p (8 bit)" -> "yuv420p"。产品显示带位深是刻意的，真值侧只给像素格式名，
// 抽取时剥掉后缀让两边口径一致（位深另有 bit_depth 字段可比）。
inline std::string StripBitSuffix(const std::string& text) {
    const size_t pos = text.find(" (");
    return pos == std::string::npos ? text : Trim(text.substr(0, pos));
}

// General 的 "Format : matroska,webm (Matroska / WebM)"：括号里是 iformat->long_name，
// 真值侧只给 iformat->name，抽取值时剥掉展示用的长名，避免整批假差异。
inline std::string StripParenSuffix(const std::string& text) {
    return StripBitSuffix(text);
}

inline SemanticFields ParseMediaInfoText(const std::string& text) {
    SemanticFields fields;
    std::string section;
    bool have_duration = false;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty())
            continue;
        if (trimmed.find(':') == std::string::npos) {
            if (trimmed.compare(0, 7, "General") == 0)
                section = "general";
            else if (trimmed.compare(0, 5, "Video") == 0)
                section = "video";
            else if (trimmed.compare(0, 5, "Audio") == 0)
                section = "audio";
            else if (trimmed.compare(0, 4, "Text") == 0 || trimmed.compare(0, 4, "Data") == 0 ||
                     trimmed.compare(0, 4, "Menu") == 0)
                section = "other";
            continue;
        }
        const size_t colon = trimmed.find(':');
        const std::string key = Trim(trimmed.substr(0, colon));
        const std::string value = Trim(trimmed.substr(colon + 1));

        if (key == "Duration") {
            // 流段也有 Duration，只认 General 的第一条（容器时长）。
            if (section == "general" && !have_duration) {
                fields.duration_s = ParseHumanDuration(value);
                have_duration = true;
            }
        } else if (section == "general") {
            if (key == "Format" && fields.format.empty())
                fields.format = StripParenSuffix(value);
            else if (key == "Stream count")
                fields.stream_count = static_cast<int>(ParseLeadingInt(value));
            else if (key == "Overall bit rate")
                fields.overall_bitrate_bps = ParseBitRate(value);
        } else if (section == "video") {
            if (key == "Format" && fields.video_codec.empty())
                fields.video_codec = value;
            else if (key == "Format profile" && fields.video_profile.empty())
                fields.video_profile = value;
            else if (key == "Format level" && fields.video_level == 0)
                fields.video_level = static_cast<int>(ParseLeadingInt(value));
            else if (key == "Width" && fields.width == 0)
                fields.width = static_cast<int>(ParseLeadingInt(value));
            else if (key == "Height" && fields.height == 0)
                fields.height = static_cast<int>(ParseLeadingInt(value));
            else if (key == "Pixel format" && fields.pix_fmt.empty())
                fields.pix_fmt = StripBitSuffix(value);
            else if (key == "Frame rate" && fields.frame_rate == 0.0)
                fields.frame_rate = ParseLeadingDouble(value);
            else if (key == "Frame count" && fields.frame_count == 0)
                fields.frame_count = ParseLeadingInt(value);
            else if (key == "Color space" && fields.color_space.empty())
                fields.color_space = value;
            else if (key == "Color primaries" && fields.color_primaries.empty())
                fields.color_primaries = value;
            else if (key == "Transfer characteristics" && fields.color_transfer.empty())
                fields.color_transfer = value;
            else if (key == "Rotation" && fields.rotation == 0.0)
                fields.rotation = ParseLeadingDouble(value);
            else if (key == "Bit rate" && fields.video_bitrate_bps == 0)
                fields.video_bitrate_bps = ParseBitRate(value);
        } else if (section == "audio") {
            if (key == "Format" && fields.audio_codec.empty())
                fields.audio_codec = value;
            else if (key == "Sample format" && fields.sample_fmt.empty())
                fields.sample_fmt = value;
            else if (key == "Sample rate" && fields.sample_rate == 0)
                fields.sample_rate = static_cast<int>(ParseLeadingInt(value));
            else if (key == "Channels" && fields.channels == 0)
                fields.channels = static_cast<int>(ParseLeadingInt(value));
            else if (key == "Bit depth" && fields.bit_depth == 0)
                fields.bit_depth = static_cast<int>(ParseLeadingInt(value));
            else if (key == "Bit rate" && fields.audio_bitrate_bps == 0)
                fields.audio_bitrate_bps = ParseBitRate(value);
        }
    }
    return fields;
}

// 顺序必须与 FieldNames() 严格一致；C++ 单测里用 size + 逐字段取值钉死。
inline std::vector<std::string> ToColumns(const SemanticFields& f) {
    char duration[64];
    char fps[64];
    char rotation[64];
    std::snprintf(duration, sizeof(duration), "%.3f", f.duration_s);
    std::snprintf(fps, sizeof(fps), "%.3f", f.frame_rate);
    std::snprintf(rotation, sizeof(rotation), "%.1f", f.rotation);
    return {
        duration,
        std::to_string(f.width),
        std::to_string(f.height),
        f.video_codec,
        f.audio_codec,
        std::to_string(f.sample_rate),
        std::to_string(f.channels),
        f.format,
        std::to_string(f.stream_count),
        std::to_string(f.overall_bitrate_bps),
        f.video_profile,
        std::to_string(f.video_level),
        f.pix_fmt,
        fps,
        std::to_string(f.frame_count),
        std::to_string(f.video_bitrate_bps),
        f.color_space,
        f.color_primaries,
        f.color_transfer,
        rotation,
        f.sample_fmt,
        std::to_string(f.bit_depth),
        std::to_string(f.audio_bitrate_bps),
    };
}

inline std::vector<std::string> ExtendedColumns(const SemanticFields& f) {
    const std::vector<std::string> all = ToColumns(f);
    return {all.begin() + 7, all.end()};
}

} // namespace corpus_fields
