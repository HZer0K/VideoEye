#include "core/analyzer/SubtitleAnalyzer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/dict.h>
#include <libavutil/rational.h>
}

namespace videoeye {
namespace analyzer {
namespace {

constexpr uint32_t kTagCea608 = MKTAG('c', '6', '0', '8');
constexpr uint32_t kTagCea708 = MKTAG('c', '7', '0', '8');

std::string Trim(const std::string& in) {
    size_t begin = 0;
    size_t end = in.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(in[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) --end;
    return in.substr(begin, end - begin);
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

bool IsAllDigits(const std::string& text) {
    if (text.empty()) return false;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool StartsWithIgnoreCase(const std::string& text, const char* prefix) {
    if (prefix == nullptr) return false;
    const size_t n = std::strlen(prefix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(text[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

// 展示用单行文本：换行/制表符压成空格，连续空格合并
std::string FlattenText(const std::string& in) {
    std::string out;
    bool pending_space = false;
    for (char c : in) {
        if (c == '\n' || c == '\r' || c == '\t') {
            pending_space = !out.empty();
            continue;
        }
        if (c == ' ') {
            if (!out.empty()) pending_space = true;
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return Trim(out);
}

std::string StripAngleTags(const std::string& in) {
    std::string out;
    bool in_tag = false;
    for (char c : in) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag) out.push_back(c);
    }
    return out;
}

// 时间串 -> 秒：支持 "SS.mmm" / "MM:SS.mmm" / "HH:MM:SS,mmm" / "H:MM:SS.cc"
bool TimestampToSeconds(const std::string& raw, double& seconds) {
    std::string text = Trim(raw);
    const size_t space = text.find_first_of(" \t");
    if (space != std::string::npos) text = text.substr(0, space);   // WebVTT 的 cue settings
    if (text.empty()) return false;

    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ':') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    if (parts.size() < 1 || parts.size() > 4) return false;

    std::string last = parts.back();
    double fraction = 0.0;
    const size_t sep = last.find_first_of(".,");
    if (sep != std::string::npos) {
        const std::string frac_text = last.substr(sep + 1);
        for (char c : frac_text) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        double divisor = 1.0;
        for (size_t i = 0; i < frac_text.size(); ++i) divisor *= 10.0;
        fraction = static_cast<double>(std::atoll(frac_text.c_str())) / divisor;
        last = last.substr(0, sep);
    }
    for (char c : last) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }

    const size_t n = parts.size();
    const double seconds_part = static_cast<double>(std::atoll(last.c_str())) + fraction;
    double minutes = 0.0;
    double hours = 0.0;
    if (n >= 2) {
        const std::string& m = parts[n - 2];
        for (char c : m) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        minutes = static_cast<double>(std::atoll(m.c_str()));
    }
    if (n >= 3) {
        const std::string& h = parts[n - 3];
        for (char c : h) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        hours = static_cast<double>(std::atoll(h.c_str()));
    }
    seconds = hours * 3600.0 + minutes * 60.0 + seconds_part;
    return true;
}

// SRT / WebVTT 共用的分块解析
bool ParseCueBlocks(const std::string& text, bool webvtt, std::vector<ParsedSubtitleCue>& out) {
    const std::vector<std::string> lines = SplitLines(text);
    std::vector<std::string> block;
    bool saw_any = false;

    auto parse_block = [&](const std::vector<std::string>& blk) {
        if (blk.empty()) return;
        if (webvtt && (StartsWithIgnoreCase(blk.front(), "NOTE") ||
                       StartsWithIgnoreCase(blk.front(), "STYLE") ||
                       StartsWithIgnoreCase(blk.front(), "REGION"))) {
            return;
        }
        size_t time_line = std::string::npos;
        for (size_t i = 0; i < blk.size(); ++i) {
            if (blk[i].find("-->") != std::string::npos) {
                time_line = i;
                break;
            }
        }
        if (time_line == std::string::npos) return;

        const std::string& timing = blk[time_line];
        const size_t arrow = timing.find("-->");
        std::string start_text = Trim(timing.substr(0, arrow));
        std::string end_text = Trim(timing.substr(arrow + 3));

        ParsedSubtitleCue cue;
        if (!TimestampToSeconds(start_text, cue.start_seconds)) return;
        if (!TimestampToSeconds(end_text, cue.end_seconds)) return;

        // 序号列（SRT 必有，WebVTT 可选且可以是任意标识）
        if (time_line > 0 && IsAllDigits(Trim(blk[time_line - 1]))) {
            cue.cue_number = static_cast<int>(std::atoll(Trim(blk[time_line - 1]).c_str()));
        }

        std::string raw;
        for (size_t i = time_line + 1; i < blk.size(); ++i) {
            if (i > time_line + 1) raw.push_back('\n');
            raw += blk[i];
        }
        cue.raw_text = Trim(raw);
        cue.text = FlattenText(StripAngleTags(cue.raw_text));
        cue.valid = true;
        out.push_back(std::move(cue));
        saw_any = true;
    };

    for (const std::string& line : lines) {
        if (Trim(line).empty()) {
            parse_block(block);
            block.clear();
            continue;
        }
        block.push_back(line);
    }
    parse_block(block);
    return saw_any;
}

std::string Cea608Char(uint8_t byte) {
    const uint8_t value = static_cast<uint8_t>(byte & 0x7F);
    switch (value) {
        case 0x2A: return "\xC3\xA1";   // á
        case 0x5C: return "\xC3\xA9";   // é
        case 0x5E: return "\xC3\xAD";   // í
        case 0x5F: return "\xC3\xB3";   // ó
        case 0x60: return "\xC3\xBA";   // ú
        case 0x7B: return "\xC3\xA7";   // ç
        case 0x7C: return "\xC3\xB7";   // ÷
        case 0x7D: return "\xC3\x91";   // Ñ
        case 0x7E: return "\xC3\xB1";   // ñ
        case 0x7F: return "\xE2\x96\x88";  // █
        default: break;
    }
    if (value >= 0x20 && value <= 0x7F) return std::string(1, static_cast<char>(value));
    return std::string();
}

}  // namespace

model::SubtitleFormat SubtitleAnalyzer::FormatFromCodecId(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_SUBRIP:
        case AV_CODEC_ID_SRT:
            return model::SubtitleFormat::Srt;
        case AV_CODEC_ID_ASS:
            return model::SubtitleFormat::Ass;
        case AV_CODEC_ID_SSA:
            return model::SubtitleFormat::Ssa;
        case AV_CODEC_ID_WEBVTT:
            return model::SubtitleFormat::WebVtt;
        case AV_CODEC_ID_MOV_TEXT:
            return model::SubtitleFormat::MovText;
        case AV_CODEC_ID_EIA_608:
            return model::SubtitleFormat::Cea608;
        case AV_CODEC_ID_DVB_SUBTITLE:
            return model::SubtitleFormat::DvbSubtitle;
        case AV_CODEC_ID_DVD_SUBTITLE:
            return model::SubtitleFormat::DvdSubtitle;
        case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
            return model::SubtitleFormat::HdmvPgs;
        case AV_CODEC_ID_XSUB:
            return model::SubtitleFormat::Xsub;
        case AV_CODEC_ID_DVB_TELETEXT:
            return model::SubtitleFormat::Teletext;
        case AV_CODEC_ID_TEXT:
        case AV_CODEC_ID_TTML:
        case AV_CODEC_ID_HDMV_TEXT_SUBTITLE:
            return model::SubtitleFormat::OtherText;
        default:
            return model::SubtitleFormat::Unknown;
    }
}

model::SubtitleKind SubtitleAnalyzer::KindFromFormat(model::SubtitleFormat format) {
    switch (format) {
        case model::SubtitleFormat::Srt:
        case model::SubtitleFormat::Ssa:
        case model::SubtitleFormat::Ass:
        case model::SubtitleFormat::WebVtt:
        case model::SubtitleFormat::MovText:
        case model::SubtitleFormat::Teletext:
        case model::SubtitleFormat::OtherText:
            return model::SubtitleKind::Text;
        case model::SubtitleFormat::Cea608:
        case model::SubtitleFormat::Cea708:
            return model::SubtitleKind::ClosedCaption;
        case model::SubtitleFormat::DvbSubtitle:
        case model::SubtitleFormat::DvdSubtitle:
        case model::SubtitleFormat::HdmvPgs:
        case model::SubtitleFormat::Xsub:
        case model::SubtitleFormat::OtherBitmap:
            return model::SubtitleKind::Bitmap;
        case model::SubtitleFormat::Unknown:
        default:
            return model::SubtitleKind::Unknown;
    }
}

bool SubtitleAnalyzer::ParseTimestamp(const std::string& text, double& seconds) {
    return TimestampToSeconds(text, seconds);
}

std::string SubtitleAnalyzer::StripAssOverrides(const std::string& in) {
    std::string out;
    bool in_brace = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '{') {
            in_brace = true;
            continue;
        }
        if (c == '}') {
            in_brace = false;
            continue;
        }
        if (in_brace) continue;
        // \N / \n 换行指令 -> 空格
        if (c == '\\' && i + 1 < in.size() && (in[i + 1] == 'N' || in[i + 1] == 'n')) {
            ++i;
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string SubtitleAnalyzer::StripVttTags(const std::string& in) {
    return StripAngleTags(in);
}

bool SubtitleAnalyzer::ParseSrtText(const std::string& text, std::vector<ParsedSubtitleCue>& out) {
    return ParseCueBlocks(text, false, out);
}

bool SubtitleAnalyzer::ParseWebVttText(const std::string& text, std::vector<ParsedSubtitleCue>& out) {
    const std::vector<std::string> lines = SplitLines(text);
    // 跳过 WEBVTT 头部（到第一个空行为止）
    size_t begin = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (StartsWithIgnoreCase(Trim(lines[i]), "WEBVTT")) {
            begin = i + 1;
            break;
        }
    }
    std::string body;
    for (size_t i = begin; i < lines.size(); ++i) {
        body += lines[i];
        body.push_back('\n');
    }
    return ParseCueBlocks(body, true, out);
}

bool SubtitleAnalyzer::ParseAssDialogueLine(const std::string& line, ParsedSubtitleCue& out) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string rest = line.substr(colon + 1);

    // 前 9 个逗号是分隔符，第 10 段是文本（文本里可以带逗号）
    std::vector<std::string> fields;
    size_t start = 0;
    for (int i = 0; i < 9; ++i) {
        const size_t comma = rest.find(',', start);
        if (comma == std::string::npos) break;
        fields.push_back(rest.substr(start, comma - start));
        start = comma + 1;
    }
    fields.push_back(rest.substr(start));
    if (fields.size() < 10) return false;

    out = ParsedSubtitleCue{};
    if (!TimestampToSeconds(fields[1], out.start_seconds)) return false;
    if (!TimestampToSeconds(fields[2], out.end_seconds)) return false;
    out.raw_text = fields[9];
    out.text = FlattenText(StripAssOverrides(out.raw_text));
    out.valid = true;
    return true;
}

bool SubtitleAnalyzer::ParseAssText(const std::string& text, std::vector<ParsedSubtitleCue>& out) {
    const std::vector<std::string> lines = SplitLines(text);
    bool saw_any = false;
    for (const std::string& line : lines) {
        if (!StartsWithIgnoreCase(Trim(line), "dialogue")) continue;
        ParsedSubtitleCue cue;
        if (ParseAssDialogueLine(Trim(line), cue)) {
            out.push_back(std::move(cue));
            saw_any = true;
        }
    }
    return saw_any;
}

std::string SubtitleAnalyzer::DecodeCea608Field(const uint8_t* data, size_t size) {
    std::string out;
    if (data == nullptr) return out;
    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint8_t first = static_cast<uint8_t>(data[i] & 0x7F);
        const uint8_t second = static_cast<uint8_t>(data[i + 1] & 0x7F);
        // 0x10-0x1F 开头的是控制码对（PAC / mid-row / tab offset 等），不是可见字符
        if (first >= 0x10 && first <= 0x1F) continue;
        out += Cea608Char(first);
        if (second >= 0x10 && second <= 0x1F) continue;
        out += Cea608Char(second);
    }
    return Trim(out);
}

size_t SubtitleAnalyzer::CountVisibleChars(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) {
        if (c >= 0x80 && c < 0xC0) continue;         // UTF-8 续字节
        if (std::isspace(c)) continue;
        ++count;
    }
    return count;
}

bool SubtitleAnalyzer::ParsePacketPayload(int codec_id, const uint8_t* data, size_t size,
                                          double pts_seconds, double duration_seconds,
                                          ParsedSubtitleCue& out) {
    out = ParsedSubtitleCue{};
    if (data == nullptr || size == 0) return false;

    const model::SubtitleFormat format = FormatFromCodecId(codec_id);

    std::string payload;
    if (format == model::SubtitleFormat::MovText && size >= 2) {
        // tx3g 样本: uint16 文本长度 + UTF-8 文本（后面可能跟样式 box）
        const size_t declared = (static_cast<size_t>(data[0]) << 8) | static_cast<size_t>(data[1]);
        const size_t available = size - 2;
        const size_t length = std::min(declared, available);
        payload.assign(reinterpret_cast<const char*>(data) + 2, length);
    } else {
        payload.assign(reinterpret_cast<const char*>(data), size);
    }
    // UTF-8 BOM
    if (payload.size() >= 3 && static_cast<unsigned char>(payload[0]) == 0xEF &&
        static_cast<unsigned char>(payload[1]) == 0xBB &&
        static_cast<unsigned char>(payload[2]) == 0xBF) {
        payload.erase(0, 3);
    }

    bool parsed = false;
    if (format == model::SubtitleFormat::Ass || format == model::SubtitleFormat::Ssa) {
        if (StartsWithIgnoreCase(payload, "dialogue")) {
            parsed = ParseAssDialogueLine(payload, out);
        }
    } else if (format == model::SubtitleFormat::Cea608) {
        const std::string text = DecodeCea608Field(data, size);
        if (!text.empty()) {
            out.text = text;
            out.raw_text = text;
            out.valid = true;
            parsed = true;
        }
    } else if (format == model::SubtitleFormat::Srt || format == model::SubtitleFormat::WebVtt ||
               format == model::SubtitleFormat::MovText ||
               format == model::SubtitleFormat::OtherText) {
        std::vector<ParsedSubtitleCue> cues;
        const bool ok = (format == model::SubtitleFormat::WebVtt)
                            ? ParseWebVttText(payload, cues)
                            : ParseSrtText(payload, cues);
        if (ok && !cues.empty()) {
            out = cues.front();
            parsed = true;
        }
    }

    if (!parsed) {
        // 退化路径：整包就是文本（Matroska 里常见），时间靠 PTS
        out.raw_text = Trim(payload);
        out.text = FlattenText(StripAngleTags(StripAssOverrides(out.raw_text)));
        out.valid = true;
    }

    if (out.start_seconds < 0.0) out.start_seconds = (pts_seconds >= 0.0) ? pts_seconds : 0.0;
    if (out.end_seconds < out.start_seconds && duration_seconds > 0.0) {
        out.end_seconds = out.start_seconds + duration_seconds;
    }
    if (out.text.empty() && !out.raw_text.empty()) {
        out.text = FlattenText(StripAngleTags(StripAssOverrides(out.raw_text)));
    }
    return true;
}

void SubtitleAnalyzer::ValidateCues(std::vector<model::SubtitleCue>& cues,
                                    std::vector<model::SubtitleIssue>& issues,
                                    const SubtitleOptions& options,
                                    double media_duration_seconds) {
    auto add_issue = [&issues](model::SubtitleIssueType type, model::IssueSeverity severity,
                               int stream_index, int cue_index, double start, double end,
                               const std::string& detail) {
        model::SubtitleIssue issue;
        issue.type = type;
        issue.severity = severity;
        issue.stream_index = stream_index;
        issue.cue_index = cue_index;
        issue.start_seconds = start;
        issue.end_seconds = end;
        issue.detail = detail;
        issues.push_back(std::move(issue));
    };

    double prev_start = -1.0;
    double prev_end = -1.0;
    int stream_index = -1;

    for (model::SubtitleCue& cue : cues) {
        stream_index = cue.stream_index;
        char buf[256] = {0};
        const double duration = cue.end_seconds - cue.start_seconds;
        bool flagged = false;

        if (options.check_empty && cue.empty) {
            std::snprintf(buf, sizeof(buf), "第 %d 条字幕没有可见文本", cue.index + 1);
            add_issue(model::SubtitleIssueType::EmptyText, model::IssueSeverity::Warning,
                      stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
            flagged = true;
        }

        if (options.check_duration && cue.end_seconds > 0.0) {
            if (duration <= 0.0) {
                std::snprintf(buf, sizeof(buf), "第 %d 条结束时间 %.3fs 不晚于开始时间 %.3fs",
                              cue.index + 1, cue.end_seconds, cue.start_seconds);
                add_issue(model::SubtitleIssueType::InvalidDuration, model::IssueSeverity::Error,
                          stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            } else if (duration < options.min_cue_duration_seconds) {
                std::snprintf(buf, sizeof(buf), "第 %d 条停留 %.3fs，短于下限 %.3fs",
                              cue.index + 1, duration, options.min_cue_duration_seconds);
                add_issue(model::SubtitleIssueType::TooShort, model::IssueSeverity::Warning,
                          stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            } else if (duration > options.max_cue_duration_seconds) {
                std::snprintf(buf, sizeof(buf), "第 %d 条停留 %.3fs，长于上限 %.3fs",
                              cue.index + 1, duration, options.max_cue_duration_seconds);
                add_issue(model::SubtitleIssueType::TooLong, model::IssueSeverity::Warning,
                          stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            }
        }

        if (prev_start >= 0.0) {
            if (options.check_order && cue.start_seconds < prev_start - 1e-6) {
                std::snprintf(buf, sizeof(buf), "第 %d 条起点 %.3fs 早于第 %d 条起点 %.3fs",
                              cue.index + 1, cue.start_seconds, cue.index, prev_start);
                add_issue(model::SubtitleIssueType::NonMonotonic, model::IssueSeverity::Error,
                          stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            } else if (options.check_overlap &&
                       cue.start_seconds < prev_end - options.overlap_tolerance_seconds) {
                std::snprintf(buf, sizeof(buf),
                              "第 %d 条起点 %.3fs 早于上一条终点 %.3fs（重叠 %.3fs）",
                              cue.index + 1, cue.start_seconds, prev_end,
                              prev_end - cue.start_seconds);
                add_issue(model::SubtitleIssueType::Overlap, model::IssueSeverity::Warning,
                          stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            }
        }

        if (options.check_reading_speed && duration > 0.2 && !cue.empty) {
            const double cps = static_cast<double>(cue.char_count) / duration;
            if (cps > options.max_chars_per_second) {
                std::snprintf(buf, sizeof(buf), "第 %d 条阅读速度 %.1f 字符/秒，超过上限 %.1f",
                              cue.index + 1, cps, options.max_chars_per_second);
                add_issue(model::SubtitleIssueType::TooFast, model::IssueSeverity::Info,
                          stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            }
        }

        if (media_duration_seconds > 0.0 &&
            cue.start_seconds > media_duration_seconds + 0.5) {
            std::snprintf(buf, sizeof(buf), "第 %d 条起点 %.3fs 超出媒体时长 %.3fs",
                          cue.index + 1, cue.start_seconds, media_duration_seconds);
            add_issue(model::SubtitleIssueType::OutOfRange, model::IssueSeverity::Warning,
                      stream_index, cue.index, cue.start_seconds, cue.end_seconds, buf);
            flagged = true;
        }

        if (flagged) cue.has_issue = true;
        prev_start = cue.start_seconds;
        prev_end = (cue.end_seconds > cue.start_seconds) ? cue.end_seconds : cue.start_seconds;
    }
}

void SubtitleAnalyzer::Reset(const SubtitleOptions& options) {
    options_ = options;
    result_ = model::SubtitleAnalysisResult{};
}

void SubtitleAnalyzer::RegisterStreams(const AVFormatContext* fmt) {
    if (fmt == nullptr) return;
    result_.streams.clear();

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st == nullptr || st->codecpar == nullptr) continue;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

        model::SubtitleStreamInfo info;
        info.stream_index = static_cast<int>(i);
        info.format = FormatFromCodecId(static_cast<int>(st->codecpar->codec_id));
        // CEA-708 没有独立 codec id，靠 MP4 的 'c708' tag 认
        if (st->codecpar->codec_tag == kTagCea708) {
            info.format = model::SubtitleFormat::Cea708;
        } else if (st->codecpar->codec_tag == kTagCea608) {
            info.format = model::SubtitleFormat::Cea608;
        }
        info.kind = KindFromFormat(info.format);

        const char* codec_name = avcodec_get_name(st->codecpar->codec_id);
        info.codec_name = codec_name ? codec_name : "";
        char tag_buf[8] = {0};
        const uint32_t tag = st->codecpar->codec_tag;
        if (tag != 0) {
            tag_buf[0] = static_cast<char>((tag >> 24) & 0xFF);
            tag_buf[1] = static_cast<char>((tag >> 16) & 0xFF);
            tag_buf[2] = static_cast<char>((tag >> 8) & 0xFF);
            tag_buf[3] = static_cast<char>(tag & 0xFF);
            bool printable = true;
            for (int k = 0; k < 4; ++k) {
                const unsigned char c = static_cast<unsigned char>(tag_buf[k]);
                if (c < 0x20 || c > 0x7E) printable = false;
            }
            if (printable) info.codec_tag = std::string(tag_buf, 4);
        }

        const AVDictionaryEntry* language = av_dict_get(st->metadata, "language", nullptr, 0);
        if (language != nullptr && language->value != nullptr) info.language = language->value;
        const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
        if (title != nullptr && title->value != nullptr) info.title = title->value;
        const AVDictionaryEntry* handler = av_dict_get(st->metadata, "handler_name", nullptr, 0);
        if (handler != nullptr && handler->value != nullptr) info.handler_name = handler->value;

        info.default_disposition = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
        info.forced = (st->disposition & AV_DISPOSITION_FORCED) != 0;
        info.hearing_impaired = (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED) != 0;

        const double tb = (st->time_base.den > 0) ? av_q2d(st->time_base) : 0.0;
        if (st->start_time != AV_NOPTS_VALUE && tb > 0.0) {
            info.start_seconds = static_cast<double>(st->start_time) * tb;
        }
        if (st->duration != AV_NOPTS_VALUE && tb > 0.0) {
            info.duration_seconds = static_cast<double>(st->duration) * tb;
        }

        if (info.kind == model::SubtitleKind::Bitmap) {
            info.note = "图形字幕：第一阶段只输出流 metadata 与包时间线（预览待第二阶段）";
            ++result_.bitmap_stream_count;
        } else if (info.kind == model::SubtitleKind::ClosedCaption) {
            info.note = (info.format == model::SubtitleFormat::Cea708)
                            ? "CEA-708：只输出流 metadata 与包时间线（DTVCC 解码待第二阶段）"
                            : "CEA-608：抽取可见文本；完整服务层状态机待第二阶段";
            ++result_.text_stream_count;
        } else {
            ++result_.text_stream_count;
        }

        result_.streams.push_back(std::move(info));
    }
}

void SubtitleAnalyzer::OnPacket(const AVPacket* pkt, const AVStream* stream) {
    if (pkt == nullptr || stream == nullptr || stream->codecpar == nullptr) return;

    model::SubtitleStreamInfo* info = nullptr;
    for (model::SubtitleStreamInfo& s : result_.streams) {
        if (s.stream_index == pkt->stream_index) {
            info = &s;
            break;
        }
    }
    if (info == nullptr) return;

    ++info->packet_count;
    info->byte_count += static_cast<int64_t>(pkt->size);

    const double tb = (stream->time_base.den > 0) ? av_q2d(stream->time_base) : 0.0;
    const double ts = (pkt->pts != AV_NOPTS_VALUE && tb > 0.0)
                          ? static_cast<double>(pkt->pts) * tb
                          : -1.0;
    const double duration = (pkt->duration > 0 && tb > 0.0)
                                ? static_cast<double>(pkt->duration) * tb
                                : 0.0;
    if (ts >= 0.0) {
        if (info->packet_count == 1) info->start_seconds = ts;
        const double end = ts + ((duration > 0.0) ? duration : 0.0);
        if (end > info->duration_seconds) info->duration_seconds = end;
    }

    const bool is_text = (info->kind == model::SubtitleKind::Text) ||
                         (info->kind == model::SubtitleKind::ClosedCaption &&
                          info->format == model::SubtitleFormat::Cea608);
    if (!options_.parse_text_cues || !is_text) return;
    if (info->cue_count >= static_cast<int>(options_.max_cues_per_stream)) return;

    ParsedSubtitleCue parsed;
    if (!ParsePacketPayload(static_cast<int>(stream->codecpar->codec_id), pkt->data,
                            static_cast<size_t>(pkt->size), ts, duration, parsed)) {
        return;
    }

    model::SubtitleCue cue;
    cue.index = info->cue_count;
    cue.stream_index = info->stream_index;
    cue.cue_number = parsed.cue_number;
    cue.start_seconds = parsed.start_seconds;
    cue.end_seconds = parsed.end_seconds;
    cue.duration_seconds = (parsed.end_seconds > parsed.start_seconds)
                               ? (parsed.end_seconds - parsed.start_seconds)
                               : 0.0;
    cue.text = parsed.text;
    cue.raw_text = parsed.raw_text;
    cue.language = info->language;
    cue.format = info->format;
    cue.char_count = CountVisibleChars(cue.text);
    cue.empty = (cue.char_count == 0);

    ++info->cue_count;
    info->parsed = true;
    result_.cues.push_back(std::move(cue));
}

void SubtitleAnalyzer::Finish(double media_duration_seconds) {
    // 按流分组校验（包是交错来的，同一流的 cue 在数组里并不连续）
    for (const model::SubtitleStreamInfo& stream : result_.streams) {
        std::vector<model::SubtitleCue> group;
        std::vector<size_t> positions;
        for (size_t i = 0; i < result_.cues.size(); ++i) {
            if (result_.cues[i].stream_index == stream.stream_index) {
                group.push_back(result_.cues[i]);
                positions.push_back(i);
            }
        }
        if (group.empty()) continue;

        ValidateCues(group, result_.issues, options_, media_duration_seconds);
        for (size_t i = 0; i < group.size(); ++i) {
            result_.cues[positions[i]] = group[i];
        }
    }

    if (options_.check_language) {
        for (const model::SubtitleStreamInfo& stream : result_.streams) {
            if (stream.language.empty()) {
                model::SubtitleIssue issue;
                issue.type = model::SubtitleIssueType::MissingLanguage;
                issue.severity = model::IssueSeverity::Info;
                issue.stream_index = stream.stream_index;
                issue.cue_index = -1;
                issue.detail = "字幕流 " + std::to_string(stream.stream_index) + " 没有 language tag";
                result_.issues.push_back(std::move(issue));
            }
            if (!stream.codec_tag.empty() && stream.handler_name.empty()) {
                model::SubtitleIssue issue;
                issue.type = model::SubtitleIssueType::MissingHandler;
                issue.severity = model::IssueSeverity::Info;
                issue.stream_index = stream.stream_index;
                issue.cue_index = -1;
                issue.detail = "字幕流 " + std::to_string(stream.stream_index) + " 没有 handler name";
                result_.issues.push_back(std::move(issue));
            }
        }
    }

    result_.total_cue_count = static_cast<int>(result_.cues.size());
    result_.analyzed = true;
}

}  // namespace analyzer
}  // namespace videoeye
