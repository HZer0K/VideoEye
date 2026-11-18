#include "core/analysis/diagnostics/SubtitleAnalyzer.h"

#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
namespace {

constexpr uint32_t kTagCea608 = MKTAG('c', '6', '0', '8');
constexpr uint32_t kTagCea708 = MKTAG('c', '7', '0', '8');

std::string Trim(const std::string& in) {
    size_t begin = 0;
    size_t end = in.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(in[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1])))
        --end;
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
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

bool IsAllDigits(const std::string& text) {
    if (text.empty())
        return false;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

// 有原生整包解析器、不依赖 FFmpeg 解码器的格式
bool IsNativelyParsedFormat(model::SubtitleFormat format) {
    switch (format) {
    case model::SubtitleFormat::Srt:
    case model::SubtitleFormat::Ssa:
    case model::SubtitleFormat::Ass:
    case model::SubtitleFormat::WebVtt:
    case model::SubtitleFormat::MovText:
        return true;
    default:
        return false;
    }
}

// ISO 639-2 里这几个码都表示"没有确定的单一语言"：
//   und 未指定（demuxer 在文件没写语言时填的就是它，MP4 mdhd 的 0x55C4）
//   mul 多语言 / zxx 无语言内容 / xxx 保留未定义
// 它们都不是空串，但语义上等于"没标语言"：播放器照样没法按界面语言自动选轨，
// 所以 missing_language 要把它们和空串一样算缺失。unk / unknown 是非标准写法，一并覆盖。
bool IsUnspecifiedLanguage(const std::string& language) {
    if (language.empty())
        return true;
    static const char* const kPlaceholders[] = {"und", "undetermined", "unk", "unknown", "mul", "zxx", "xxx"};
    std::string lower;
    lower.reserve(language.size());
    for (char c : language) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const char* placeholder : kPlaceholders) {
        if (lower == placeholder)
            return true;
    }
    return false;
}

bool StartsWithIgnoreCase(const std::string& text, const char* prefix) {
    if (prefix == nullptr)
        return false;
    const size_t n = std::strlen(prefix);
    if (text.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(text[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
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
            if (!out.empty())
                pending_space = true;
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

// '<' 什么时候是标签开始？只看两种真实存在的写法：
//   HTML 标签 <i> / </i> / <font ...>，以及 WebVTT 时间戳标签 <00:01:02.500>。
// 字幕正文里的比较符号（如 "2<3,5>1,4<6"）不能当标签删掉 —— 否则可见文本与
// 字符数都会失真，阅读速度跟着误报。
bool IsAngleTagStart(const std::string& text, size_t pos) {
    const size_t next = pos + 1;
    if (next >= text.size())
        return false;
    const unsigned char c = static_cast<unsigned char>(text[next]);
    if (std::isalpha(c) || c == '/')
        return true;
    if (!std::isdigit(c))
        return false;
    // 数字开头只有 "mm:ss" / "hh:mm:ss" 这种时间戳才算标签
    for (size_t i = next; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == ':')
            return true;
        if (!std::isdigit(ch))
            return false;
    }
    return false;
}

std::string StripAngleTags(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '<' && IsAngleTagStart(in, i)) {
            const size_t close = in.find('>', i + 1);
            if (close == std::string::npos) {
                out.append(in, i, std::string::npos);
                break;
            }
            i = close + 1;
            continue;
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

// 时间串 -> 秒：支持 "SS.mmm" / "MM:SS.mmm" / "HH:MM:SS,mmm" / "H:MM:SS.cc"
bool TimestampToSeconds(const std::string& raw, double& seconds) {
    std::string text = Trim(raw);
    const size_t space = text.find_first_of(" \t");
    if (space != std::string::npos)
        text = text.substr(0, space); // WebVTT 的 cue settings
    if (text.empty())
        return false;

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
    if (parts.size() < 1 || parts.size() > 4)
        return false;

    std::string last = parts.back();
    double fraction = 0.0;
    const size_t sep = last.find_first_of(".,");
    if (sep != std::string::npos) {
        const std::string frac_text = last.substr(sep + 1);
        for (char c : frac_text) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
        }
        double divisor = 1.0;
        for (size_t i = 0; i < frac_text.size(); ++i)
            divisor *= 10.0;
        fraction = static_cast<double>(std::atoll(frac_text.c_str())) / divisor;
        last = last.substr(0, sep);
    }
    for (char c : last) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }

    const size_t n = parts.size();
    const double seconds_part = static_cast<double>(std::atoll(last.c_str())) + fraction;
    double minutes = 0.0;
    double hours = 0.0;
    if (n >= 2) {
        const std::string& m = parts[n - 2];
        for (char c : m) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
        }
        minutes = static_cast<double>(std::atoll(m.c_str()));
    }
    if (n >= 3) {
        const std::string& h = parts[n - 3];
        for (char c : h) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
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
        if (blk.empty())
            return;
        if (webvtt && (StartsWithIgnoreCase(blk.front(), "NOTE") || StartsWithIgnoreCase(blk.front(), "STYLE") ||
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
        if (time_line == std::string::npos)
            return;

        const std::string& timing = blk[time_line];
        const size_t arrow = timing.find("-->");
        std::string start_text = Trim(timing.substr(0, arrow));
        std::string end_text = Trim(timing.substr(arrow + 3));

        ParsedSubtitleCue cue;
        if (!TimestampToSeconds(start_text, cue.start_seconds))
            return;
        if (!TimestampToSeconds(end_text, cue.end_seconds))
            return;

        // 序号列（SRT 必有，WebVTT 可选且可以是任意标识）
        if (time_line > 0 && IsAllDigits(Trim(blk[time_line - 1]))) {
            cue.cue_number = static_cast<int>(std::atoll(Trim(blk[time_line - 1]).c_str()));
        }

        std::string raw;
        for (size_t i = time_line + 1; i < blk.size(); ++i) {
            if (i > time_line + 1)
                raw.push_back('\n');
            raw += blk[i];
        }
        cue.raw_text = Trim(raw);
        // SRT 里也常出现 {\an8} 之类的 ASS 覆盖指令，必须一并剥掉，
        // 否则这些控制符会计进字符数并出现在导出文本里。
        cue.text = FlattenText(StripAngleTags(SubtitleAnalyzer::StripAssOverrides(cue.raw_text)));
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
    case 0x2A:
        return "\xC3\xA1"; // á
    case 0x5C:
        return "\xC3\xA9"; // é
    case 0x5E:
        return "\xC3\xAD"; // í
    case 0x5F:
        return "\xC3\xB3"; // ó
    case 0x60:
        return "\xC3\xBA"; // ú
    case 0x7B:
        return "\xC3\xA7"; // ç
    case 0x7C:
        return "\xC3\xB7"; // ÷
    case 0x7D:
        return "\xC3\x91"; // Ñ
    case 0x7E:
        return "\xC3\xB1"; // ñ
    case 0x7F:
        return "\xE2\x96\x88"; // █
    default:
        break;
    }
    if (value >= 0x20 && value <= 0x7F)
        return std::string(1, static_cast<char>(value));
    return std::string();
}

// ---- 视频内嵌 CC（A53）解析 ----
// 三种载体最终都收敛成 ATSC A/53 的 3 字节三元组（marker/valid/type + 两个 608/708 字节），
// 再交给既有的 CeaCaptionDecoder —— 它已经有 608 通道/708 服务的完整状态机，
// 这里只负责"把隐藏字幕从视频码流里挖出来"。

uint8_t ReverseBits8(uint8_t value) {
    value = static_cast<uint8_t>(((value & 0xF0) >> 4) | ((value & 0x0F) << 4));
    value = static_cast<uint8_t>(((value & 0xCC) >> 2) | ((value & 0x33) << 2));
    return static_cast<uint8_t>(((value & 0xAA) >> 1) | ((value & 0x55) << 1));
}

// MSB-first 位读取，只服务 SCTE-20 的定长字段
class MsbBitReader {
public:
    MsbBitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {
    }

    int BitsLeft() const {
        return static_cast<int>(size_ * 8 - bit_pos_);
    }

    uint32_t Read(int bits) {
        uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            const size_t byte_index = bit_pos_ / 8;
            if (byte_index >= size_) {
                return value << (bits - i - 1);
            }
            const int bit_index = 7 - static_cast<int>(bit_pos_ % 8);
            value = (value << 1) | ((data_[byte_index] >> bit_index) & 1u);
            ++bit_pos_;
        }
        return value;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t bit_pos_ = 0;
};

// A/53 cc_data 里三元组前面的两个字节：
//   [0] user_data_type_code (0x03)
//   [1] reserved(1) process_cc_data_flag(1) zero(1) cc_count(5)
//   [2] reserved(8)
// 三元组从 [3] 开始；末尾还有一个 0xFF marker，按 cc_count 截取即可。
bool TripletsFromCcData(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (data == nullptr || size < 3 || data[0] != 0x03)
        return false;
    const uint8_t header = data[1];
    if ((header & 0x40) == 0)
        return false;
    const size_t cc_count = header & 0x1F;
    if (cc_count == 0 || size < 3 + cc_count * 3)
        return false;
    out.assign(data + 3, data + 3 + cc_count * 3);
    return true;
}

// SCTE-20（MPEG-2 早期 CC）：user_data 以 0x03 0x01 开头，字段是位打包的，
// 且 608 字节对按位反转存放。top_field_first 决定 1/2 场映射到 cc_type。
bool TripletsFromScte20(const uint8_t* data, size_t size, bool top_field_first, std::vector<uint8_t>& out) {
    if (data == nullptr || size < 3 || data[0] != 0x03 || (data[1] & 0x7F) != 0x01)
        return false;
    MsbBitReader reader(data + 2, size - 2);
    const int cc_count = static_cast<int>(reader.Read(5));
    if (cc_count <= 0)
        return false;
    out.reserve(static_cast<size_t>(cc_count) * 3);
    for (int i = 0; i < cc_count; ++i) {
        if (reader.BitsLeft() < 26)
            break;
        reader.Read(2); // cc_priority
        const int field = static_cast<int>(reader.Read(2));
        reader.Read(5); // line_offset
        const uint8_t cc1 = static_cast<uint8_t>(reader.Read(8));
        const uint8_t cc2 = static_cast<uint8_t>(reader.Read(8));
        reader.Read(1); // marker
        if (field == 0) {
            // 禁止的场：写入 valid=0 的三元组，保持场序不被打乱
            out.push_back(0x00);
            out.push_back(0x00);
            out.push_back(0x00);
            continue;
        }
        int cc_type = (field == 2) ? 1 : 0;
        if (!top_field_first)
            cc_type = 1 - cc_type;
        out.push_back(static_cast<uint8_t>(0x04 | cc_type));
        out.push_back(ReverseBits8(cc1));
        out.push_back(ReverseBits8(cc2));
    }
    return !out.empty();
}

// SEI 里的 T.35（ATSC A/53）：country 0xB5 + provider 0x0031 + 'GA94' + type 0x03
void CollectSeiTriplets(const uint8_t* payload, size_t size, std::vector<uint8_t>& out) {
    if (payload == nullptr || size < 8)
        return;
    size_t i = 0;
    if (payload[i++] != 0xB5)
        return;
    if (i + 2 > size)
        return;
    const uint16_t provider = static_cast<uint16_t>((payload[i] << 8) | payload[i + 1]);
    i += 2;
    if (provider != 0x0031)
        return;
    if (i + 4 > size || std::memcmp(payload + i, "GA94", 4) != 0)
        return;
    i += 4;
    TripletsFromCcData(payload + i, size - i, out);
}

// 遍历一个 H.264/HEVC 包的 SEI NAL，把其中所有 A53 三元组追加到 out
void CollectSeiFromPacket(const uint8_t* data, size_t size, int codec_id, int nal_prefix_bytes,
                          std::vector<uint8_t>& out) {
    NalSyntax syntax;
    int sei_type = -1;
    int sei_type2 = -1;
    if (codec_id == AV_CODEC_ID_H264) {
        syntax = NalSyntax::H264;
        sei_type = 6;
    } else if (codec_id == AV_CODEC_ID_HEVC) {
        syntax = NalSyntax::Hevc;
        sei_type = 39;  // prefix SEI
        sei_type2 = 40; // suffix SEI
    } else {
        return;
    }

    auto handle_nal = [&](const NalView& nal) {
        if (nal.type != sei_type && nal.type != sei_type2)
            return;
        const std::vector<uint8_t> rbsp = UnescapeRbsp(nal.payload, nal.size);
        size_t pos = 0;
        while (pos + 1 < rbsp.size()) {
            int payload_type = 0;
            while (pos < rbsp.size() && rbsp[pos] == 0xFF) {
                payload_type += 255;
                ++pos;
            }
            if (pos >= rbsp.size())
                break;
            payload_type += rbsp[pos++];
            int payload_size = 0;
            while (pos < rbsp.size() && rbsp[pos] == 0xFF) {
                payload_size += 255;
                ++pos;
            }
            if (pos >= rbsp.size())
                break;
            payload_size += rbsp[pos++];
            if (payload_size < 0 || pos + static_cast<size_t>(payload_size) > rbsp.size())
                break;
            if (payload_type == 4)
                CollectSeiTriplets(rbsp.data() + pos, static_cast<size_t>(payload_size), out);
            pos += static_cast<size_t>(payload_size);
        }
    };

    const bool annex_b = size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1));
    if (annex_b || nal_prefix_bytes == 0) {
        ExtradataParser::ForEachAnnexBNalUnit(data, size, syntax, handle_nal);
        return;
    }
    bool truncated = false;
    ExtradataParser::ForEachLengthPrefixedNalUnit(data, size, syntax, nal_prefix_bytes, &truncated, handle_nal);
}

// MPEG-2：user_data（0x000001B2）里可能是 A/53 'GA94'，也可能是 SCTE-20；
// 顺带跟踪 picture coding extension（0x000001B5）的 top_field_first。
void CollectMpeg2UserData(const uint8_t* data, size_t size, bool& top_field_first, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i + 4 <= size) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
            ++i;
            continue;
        }
        const uint8_t code = data[i + 3];
        const uint8_t* payload = data + i + 4;
        size_t end = i + 4;
        while (end + 3 <= size) {
            if (data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1)
                break;
            ++end;
        }
        if (end + 3 > size)
            end = size;
        const size_t payload_size = end - (i + 4);

        if (code == 0xB2 && payload_size >= 5 && std::memcmp(payload, "GA94", 4) == 0 && payload[4] == 0x03) {
            TripletsFromCcData(payload + 4, payload_size - 4, out);
        } else if (code == 0xB2) {
            TripletsFromScte20(payload, payload_size, top_field_first, out);
        } else if (code == 0xB5 && payload_size >= 6) {
            // picture_coding_extension: 4 bit id + 4×4 bit f_code + 2 bit precision
            // + 2 bit structure，第 40 位就是 top_field_first（payload[5] 的最高位）
            top_field_first = (payload[5] & 0x80) != 0;
        }
        i = (end > i) ? end : (i + 4);
    }
}

// UTF-8 码点迭代；非法序列按 U+FFFD 处理并继续前进，避免整段文本被判成乱码
bool NextUtf8CodePoint(const std::string& text, size_t& pos, uint32_t& code_point) {
    if (pos >= text.size())
        return false;
    const unsigned char lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) {
        code_point = lead;
        ++pos;
        return true;
    }
    int extra = 0;
    uint32_t value = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
    } else {
        code_point = 0xFFFD;
        ++pos;
        return true;
    }
    if (pos + static_cast<size_t>(extra) >= text.size()) {
        code_point = 0xFFFD;
        ++pos;
        return true;
    }
    for (int i = 1; i <= extra; ++i) {
        const unsigned char continuation = static_cast<unsigned char>(text[pos + i]);
        if ((continuation & 0xC0) != 0x80) {
            code_point = 0xFFFD;
            ++pos;
            return true;
        }
        value = (value << 6) | (continuation & 0x3F);
    }
    pos += static_cast<size_t>(extra) + 1;
    code_point = value;
    return true;
}

// 乱码判定只认三类有明确证据的码点：
//   U+FFFD —— 解码失败被替换；私用区 —— 字体映射错位；C0/C1 控制符 —— 二进制被当文本。
// 不去猜"全半角混排"这类风格问题，那种规则会在正常中英混排上大面积误报。
bool FindGarbledReason(const std::string& text, std::string& reason) {
    size_t pos = 0;
    uint32_t code_point = 0;
    while (NextUtf8CodePoint(text, pos, code_point)) {
        if (code_point == 0xFFFD) {
            reason = "包含 U+FFFD 替换字符";
            return true;
        }
        if ((code_point >= 0xE000 && code_point <= 0xF8FF) || (code_point >= 0xF0000 && code_point <= 0xFFFFD) ||
            (code_point >= 0x100000 && code_point <= 0x10FFFD)) {
            char buf[64] = {0};
            std::snprintf(buf, sizeof(buf), "包含私用区码点 U+%04X", code_point);
            reason = buf;
            return true;
        }
        const bool c0_control = code_point < 0x20 && code_point != '\n' && code_point != '\r' && code_point != '\t';
        const bool c1_control = code_point >= 0x80 && code_point <= 0x9F;
        if (c0_control || c1_control) {
            char buf[64] = {0};
            std::snprintf(buf, sizeof(buf), "包含控制字符 U+%04X", code_point);
            reason = buf;
            return true;
        }
    }
    return false;
}

// MP4/MKV 的 AVC/HEVC 是长度前缀存储，前缀宽度写在 avcC / hvcC 的 lengthSizeMinusOne
int NalLengthPrefixBytes(const AVCodecParameters* par) {
    if (par == nullptr || par->extradata == nullptr || par->extradata_size < 23)
        return 0;
    const uint8_t* extra = par->extradata;
    if (par->codec_id == AV_CODEC_ID_H264 && extra[0] == 1)
        return (extra[4] & 0x03) + 1;
    if (par->codec_id == AV_CODEC_ID_HEVC && extra[0] == 1)
        return (extra[21] & 0x03) + 1;
    return 0;
}

} // namespace

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
    case AV_CODEC_ID_MICRODVD:
        return model::SubtitleFormat::MicroDvd;
    case AV_CODEC_ID_SUBVIEWER:
    case AV_CODEC_ID_SUBVIEWER1:
        return model::SubtitleFormat::SubViewer;
    case AV_CODEC_ID_SAMI:
        return model::SubtitleFormat::Sami;
    case AV_CODEC_ID_REALTEXT:
        return model::SubtitleFormat::RealText;
    case AV_CODEC_ID_STL:
        return model::SubtitleFormat::Stl;
    case AV_CODEC_ID_JACOSUB:
        return model::SubtitleFormat::JacoSub;
    case AV_CODEC_ID_MPL2:
        return model::SubtitleFormat::Mpl2;
    case AV_CODEC_ID_PJS:
        return model::SubtitleFormat::Pjs;
    case AV_CODEC_ID_VPLAYER:
        return model::SubtitleFormat::VPlayer;
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
    case model::SubtitleFormat::MicroDvd:
    case model::SubtitleFormat::SubViewer:
    case model::SubtitleFormat::Sami:
    case model::SubtitleFormat::RealText:
    case model::SubtitleFormat::Stl:
    case model::SubtitleFormat::JacoSub:
    case model::SubtitleFormat::Mpl2:
    case model::SubtitleFormat::Pjs:
    case model::SubtitleFormat::VPlayer:
    case model::SubtitleFormat::AqTitle:
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
        if (in_brace)
            continue;
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
    if (colon == std::string::npos)
        return false;
    std::string rest = line.substr(colon + 1);

    // 前 9 个逗号是分隔符，第 10 段是文本（文本里可以带逗号）
    std::vector<std::string> fields;
    size_t start = 0;
    for (int i = 0; i < 9; ++i) {
        const size_t comma = rest.find(',', start);
        if (comma == std::string::npos)
            break;
        fields.push_back(rest.substr(start, comma - start));
        start = comma + 1;
    }
    fields.push_back(rest.substr(start));
    if (fields.size() < 10)
        return false;

    out = ParsedSubtitleCue{};
    if (!TimestampToSeconds(fields[1], out.start_seconds))
        return false;
    if (!TimestampToSeconds(fields[2], out.end_seconds))
        return false;
    out.raw_text = fields[9];
    out.text = FlattenText(StripAssOverrides(out.raw_text));
    out.valid = true;
    return true;
}

bool SubtitleAnalyzer::ParseAssText(const std::string& text, std::vector<ParsedSubtitleCue>& out) {
    const std::vector<std::string> lines = SplitLines(text);
    bool saw_any = false;
    for (const std::string& line : lines) {
        if (!StartsWithIgnoreCase(Trim(line), "dialogue"))
            continue;
        ParsedSubtitleCue cue;
        if (ParseAssDialogueLine(Trim(line), cue)) {
            out.push_back(std::move(cue));
            saw_any = true;
        }
    }
    return saw_any;
}

bool SubtitleAnalyzer::ParseMicroDvdText(const std::string& text, double frame_rate,
                                         std::vector<ParsedSubtitleCue>& out) {
    if (frame_rate <= 1.0)
        frame_rate = 25.0;
    const std::vector<std::string> lines = SplitLines(text);
    bool saw_any = false;
    for (const std::string& line : lines) {
        const std::string trimmed = Trim(line);
        if (trimmed.size() < 5 || trimmed[0] != '{')
            continue;
        const size_t first_close = trimmed.find('}');
        if (first_close == std::string::npos)
            continue;
        const size_t second_open = trimmed.find('{', first_close + 1);
        if (second_open == std::string::npos)
            continue;
        const size_t second_close = trimmed.find('}', second_open + 1);
        if (second_close == std::string::npos)
            continue;

        const long start_frame = std::atol(trimmed.substr(1, first_close - 1).c_str());
        const long end_frame = std::atol(trimmed.substr(second_open + 1, second_close - second_open - 1).c_str());
        std::string body = trimmed.substr(second_close + 1);
        // | 是 MicroDVD 的换行符
        std::replace(body.begin(), body.end(), '|', '\n');

        ParsedSubtitleCue cue;
        cue.start_seconds = static_cast<double>(start_frame) / frame_rate;
        cue.end_seconds = static_cast<double>(end_frame) / frame_rate;
        cue.raw_text = body;
        cue.text = FlattenText(StripAngleTags(StripAssOverrides(body)));
        cue.valid = true;
        // {n}{n} 且文本为空 = 清屏指令，不是"空字幕"，不能计入 cue
        if (cue.text.empty() && std::llabs(end_frame - start_frame) <= 1)
            continue;
        out.push_back(std::move(cue));
        saw_any = true;
    }
    return saw_any;
}

bool SubtitleAnalyzer::IsValidUtf8(const std::string& text) {
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t extra = 0;
        if ((c & 0xE0) == 0xC0)
            extra = 1;
        else if ((c & 0xF0) == 0xE0)
            extra = 2;
        else if ((c & 0xF8) == 0xF0)
            extra = 3;
        else
            return false;
        if (i + extra >= n)
            return false;
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string SubtitleAnalyzer::DecodeCea608Field(const uint8_t* data, size_t size) {
    std::string out;
    if (data == nullptr)
        return out;
    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint8_t first = static_cast<uint8_t>(data[i] & 0x7F);
        const uint8_t second = static_cast<uint8_t>(data[i + 1] & 0x7F);
        // 0x10-0x1F 开头的是控制码对（PAC / mid-row / tab offset 等），不是可见字符
        if (first >= 0x10 && first <= 0x1F)
            continue;
        out += Cea608Char(first);
        if (second >= 0x10 && second <= 0x1F)
            continue;
        out += Cea608Char(second);
    }
    return Trim(out);
}

size_t SubtitleAnalyzer::CountVisibleChars(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) {
        if (c >= 0x80 && c < 0xC0)
            continue; // UTF-8 续字节
        if (std::isspace(c))
            continue;
        ++count;
    }
    return count;
}

bool SubtitleAnalyzer::ParsePacketPayload(int codec_id, const uint8_t* data, size_t size, double pts_seconds,
                                          double duration_seconds, ParsedSubtitleCue& out) {
    out = ParsedSubtitleCue{};
    if (data == nullptr || size == 0)
        return false;

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
        static_cast<unsigned char>(payload[1]) == 0xBB && static_cast<unsigned char>(payload[2]) == 0xBF) {
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
               format == model::SubtitleFormat::MovText || format == model::SubtitleFormat::OtherText) {
        std::vector<ParsedSubtitleCue> cues;
        const bool ok =
            (format == model::SubtitleFormat::WebVtt) ? ParseWebVttText(payload, cues) : ParseSrtText(payload, cues);
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

    if (out.start_seconds < 0.0)
        out.start_seconds = (pts_seconds >= 0.0) ? pts_seconds : 0.0;
    if (out.end_seconds < out.start_seconds && duration_seconds > 0.0) {
        out.end_seconds = out.start_seconds + duration_seconds;
    }
    if (out.text.empty() && !out.raw_text.empty()) {
        out.text = FlattenText(StripAngleTags(StripAssOverrides(out.raw_text)));
    }
    return true;
}

void SubtitleAnalyzer::ValidateCues(std::vector<model::SubtitleCue>& cues, std::vector<model::SubtitleIssue>& issues,
                                    const SubtitleOptions& options, double media_duration_seconds,
                                    bool skip_garbled_check) {
    auto add_issue = [&issues](model::SubtitleIssueType type, model::IssueSeverity severity, int stream_index,
                               int cue_index, double start, double end, const std::string& detail) {
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
    std::string prev_text;
    int stream_index = -1;

    for (model::SubtitleCue& cue : cues) {
        stream_index = cue.stream_index;
        char buf[256] = {0};
        const double duration = cue.end_seconds - cue.start_seconds;
        bool flagged = false;

        if (options.check_empty && cue.empty) {
            std::snprintf(buf, sizeof(buf), "第 %d 条字幕没有可见文本", cue.index + 1);
            add_issue(model::SubtitleIssueType::EmptyText, model::IssueSeverity::Warning, stream_index, cue.index,
                      cue.start_seconds, cue.end_seconds, buf);
            flagged = true;
        }

        if (options.check_duration && cue.end_seconds > 0.0) {
            if (duration <= 0.0) {
                std::snprintf(buf, sizeof(buf), "第 %d 条结束时间 %.3fs 不晚于开始时间 %.3fs", cue.index + 1,
                              cue.end_seconds, cue.start_seconds);
                add_issue(model::SubtitleIssueType::InvalidDuration, model::IssueSeverity::Error, stream_index,
                          cue.index, cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            } else if (duration < options.min_cue_duration_seconds) {
                std::snprintf(buf, sizeof(buf), "第 %d 条停留 %.3fs，短于下限 %.3fs", cue.index + 1, duration,
                              options.min_cue_duration_seconds);
                add_issue(model::SubtitleIssueType::TooShort, model::IssueSeverity::Warning, stream_index, cue.index,
                          cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            } else if (duration > options.max_cue_duration_seconds) {
                std::snprintf(buf, sizeof(buf), "第 %d 条停留 %.3fs，长于上限 %.3fs", cue.index + 1, duration,
                              options.max_cue_duration_seconds);
                add_issue(model::SubtitleIssueType::TooLong, model::IssueSeverity::Warning, stream_index, cue.index,
                          cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            }
        }

        if (prev_start >= 0.0) {
            if (options.check_order && cue.start_seconds < prev_start - 1e-6) {
                std::snprintf(buf, sizeof(buf), "第 %d 条起点 %.3fs 早于第 %d 条起点 %.3fs", cue.index + 1,
                              cue.start_seconds, cue.index, prev_start);
                add_issue(model::SubtitleIssueType::NonMonotonic, model::IssueSeverity::Error, stream_index, cue.index,
                          cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            } else if (options.check_overlap && cue.start_seconds < prev_end - options.overlap_tolerance_seconds) {
                // ASS/SSA 的 Dialogue 本来就允许分层：正片字幕 + 屏幕字/特效字可以
                // 同时上屏，两条**不同文本**的事件时间重叠是合法排版，不是时间轴错误。
                // 只有文本完全相同才说明是重复事件（同一句话被下了两次）。
                // 真正的时序错误仍由 NonMonotonic / InvalidDuration / OutOfRange 兜底。
                const bool layered_ass =
                    (cue.format == model::SubtitleFormat::Ass || cue.format == model::SubtitleFormat::Ssa) &&
                    !prev_text.empty() && cue.text != prev_text;
                if (!layered_ass) {
                    std::snprintf(buf, sizeof(buf), "第 %d 条起点 %.3fs 早于上一条终点 %.3fs（重叠 %.3fs）",
                                  cue.index + 1, cue.start_seconds, prev_end, prev_end - cue.start_seconds);
                    add_issue(model::SubtitleIssueType::Overlap, model::IssueSeverity::Warning, stream_index, cue.index,
                              cue.start_seconds, cue.end_seconds, buf);
                    flagged = true;
                }
            }
        }

        if (options.check_reading_speed && duration > 0.2 && !cue.empty) {
            const double cps = static_cast<double>(cue.char_count) / duration;
            if (cps > options.max_chars_per_second) {
                std::snprintf(buf, sizeof(buf), "第 %d 条阅读速度 %.1f 字符/秒，超过上限 %.1f", cue.index + 1, cps,
                              options.max_chars_per_second);
                add_issue(model::SubtitleIssueType::TooFast, model::IssueSeverity::Info, stream_index, cue.index,
                          cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            }
        }

        if (media_duration_seconds > 0.0 && cue.start_seconds > media_duration_seconds + 0.5) {
            std::snprintf(buf, sizeof(buf), "第 %d 条起点 %.3fs 超出媒体时长 %.3fs", cue.index + 1, cue.start_seconds,
                          media_duration_seconds);
            add_issue(model::SubtitleIssueType::OutOfRange, model::IssueSeverity::Warning, stream_index, cue.index,
                      cue.start_seconds, cue.end_seconds, buf);
            flagged = true;
        }

        // 文本质量只做有客观证据的检查：乱码码点、相邻重复、单行超长。
        // 风格类问题（全半角混排、标点缺失）一律不报 —— 正常中英混排会被大面积误伤。
        if (options.check_text_quality && !cue.empty) {
            std::string garbled_reason;
            if (!skip_garbled_check && FindGarbledReason(cue.text, garbled_reason)) {
                std::snprintf(buf, sizeof(buf), "第 %d 条文本疑似乱码：%s", cue.index + 1, garbled_reason.c_str());
                add_issue(model::SubtitleIssueType::GarbledText, model::IssueSeverity::Warning, stream_index, cue.index,
                          cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            }
            if (!prev_text.empty() && cue.text == prev_text) {
                std::snprintf(buf, sizeof(buf), "第 %d 条与上一条文本完全相同，疑似重复上屏", cue.index + 1);
                add_issue(model::SubtitleIssueType::DuplicateCue, model::IssueSeverity::Info, stream_index, cue.index,
                          cue.start_seconds, cue.end_seconds, buf);
                flagged = true;
            }
            if (options.max_line_chars > 0) {
                for (const std::string& line : SplitLines(cue.raw_text)) {
                    const std::string visible = FlattenText(StripAngleTags(StripAssOverrides(line)));
                    const size_t chars = CountVisibleChars(visible);
                    if (chars > options.max_line_chars) {
                        std::snprintf(buf, sizeof(buf), "第 %d 条有 %zu 字符的单行，超过建议上限 %u", cue.index + 1,
                                      chars, options.max_line_chars);
                        add_issue(model::SubtitleIssueType::LineTooLong, model::IssueSeverity::Info, stream_index,
                                  cue.index, cue.start_seconds, cue.end_seconds, buf);
                        flagged = true;
                        break;
                    }
                }
            }
        }

        if (flagged)
            cue.has_issue = true;
        prev_start = cue.start_seconds;
        prev_text = cue.text;
        // prev_end 必须单调不减：字幕时间轴只会往前走。畸形样本（end 早于 start）会把
        // 它拉回起点，之后的重叠率、停留时长、丢帧判定全都跟着倒退 —— 一条坏数据会
        // 放大成一片误报，所以这里只取"不比历史更靠前"的那个值。
        const double cue_end = std::max(cue.end_seconds, cue.start_seconds);
        prev_end = std::max(prev_end, cue_end);
    }
}

void SubtitleAnalyzer::Reset(const SubtitleOptions& options) {
    options_ = options;
    result_ = model::SubtitleAnalysisResult{};
    caption_slots_.clear();
    ReleaseTextDecoders();
    whole_file_streams_.clear();
    unsupported_streams_.clear();
    video_slots_.clear();
    timeline_probes_.clear();
    non_utf8_streams_.clear();
}

SubtitleAnalyzer::~SubtitleAnalyzer() {
    ReleaseTextDecoders();
}

void SubtitleAnalyzer::SetSourcePath(std::string path) {
    source_path_ = std::move(path);
}

SubtitleAnalyzer::CaptionSlot* SubtitleAnalyzer::FindOrCreateCaptionSlot(int stream_index) {
    for (CaptionSlot& slot : caption_slots_) {
        if (slot.stream_index == stream_index)
            return &slot;
    }
    CaptionSlot slot;
    slot.stream_index = stream_index;
    slot.decoder.Reset();
    caption_slots_.push_back(std::move(slot));
    return &caption_slots_.back();
}

void SubtitleAnalyzer::RegisterStreams(const AVFormatContext* fmt) {
    if (fmt == nullptr)
        return;
    result_.streams.clear();

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st == nullptr || st->codecpar == nullptr)
            continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // 附件图片（封面）不是真实视频轨，里面的用户数据没有意义
            if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
                EmbeddedVideoSlot slot;
                slot.stream_index = static_cast<int>(i);
                slot.codec_id = static_cast<int>(st->codecpar->codec_id);
                slot.nal_prefix_bytes = NalLengthPrefixBytes(st->codecpar);
                video_slots_.push_back(slot);
            }
            continue;
        }
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
            continue;

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
                if (c < 0x20 || c > 0x7E)
                    printable = false;
            }
            if (printable)
                info.codec_tag = std::string(tag_buf, 4);
        }

        const AVDictionaryEntry* language = av_dict_get(st->metadata, "language", nullptr, 0);
        if (language != nullptr && language->value != nullptr)
            info.language = language->value;
        const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
        if (title != nullptr && title->value != nullptr)
            info.title = title->value;
        const AVDictionaryEntry* handler = av_dict_get(st->metadata, "handler_name", nullptr, 0);
        if (handler != nullptr && handler->value != nullptr)
            info.handler_name = handler->value;

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

        const int codec_id = static_cast<int>(st->codecpar->codec_id);
        const bool has_decoder = avcodec_find_decoder(static_cast<AVCodecID>(codec_id)) != nullptr;

        if (info.kind == model::SubtitleKind::Bitmap) {
            info.note = "图形字幕：只输出流 metadata 与包时间线（不做 OCR / 像素级合成分析，见 D3 边界）";
            ++result_.bitmap_stream_count;
        } else if (info.kind == model::SubtitleKind::ClosedCaption) {
            info.note = (info.format == model::SubtitleFormat::Cea708)
                            ? "CEA-708：DTVCC 按服务解码（文本与起止时间来自上屏/擦除事件）"
                            : "CEA-608：按 CC1-CC4 通道解码（pop-on/paint-on/roll-up 状态机）";
            ++result_.text_stream_count;
        } else if (info.kind == model::SubtitleKind::Text) {
            if (!IsNativelyParsedFormat(info.format) && !has_decoder) {
                // 既没有原生解析器、FFmpeg 也不认识这个 codec：不能装作解析成功，
                // 必须留下明确的"未解析"结论，否则这类流会被当成"没有问题"。
                info.note = "该字幕格式没有可用的解析器，未做文本级检查";
                unsupported_streams_.push_back(info.stream_index);
                model::SubtitleIssue issue;
                issue.type = model::SubtitleIssueType::UnsupportedFormat;
                issue.severity = model::IssueSeverity::Warning;
                issue.stream_index = info.stream_index;
                issue.cue_index = -1;
                issue.detail = "字幕流 " + std::to_string(info.stream_index) + "（" + info.codec_name +
                               "）格式不支持解析，未做文本级检查";
                result_.issues.push_back(std::move(issue));
            } else {
                ++result_.text_stream_count;
            }
        } else {
            // 认不出承载方式（ARIB 图文、未知 codec 等）：同样要显式说明，而不是静默跳过
            info.note = "无法识别的字幕流类型，未做文本级检查";
            unsupported_streams_.push_back(info.stream_index);
            model::SubtitleIssue issue;
            issue.type = model::SubtitleIssueType::UnsupportedFormat;
            issue.severity = model::IssueSeverity::Info;
            issue.stream_index = info.stream_index;
            issue.cue_index = -1;
            issue.detail = "字幕流 " + std::to_string(info.stream_index) + "（" + info.codec_name +
                           "）类型无法识别，未做文本级检查";
            result_.issues.push_back(std::move(issue));
        }

        result_.streams.push_back(std::move(info));
    }

    // 纯字幕文件（没有音视频流）必须整文件重解析：demuxer 会把空事件、畸形事件
    // 直接丢掉，只靠逐包路径永远看不到它们。
    TryParseWholeFile(fmt);
}

void SubtitleAnalyzer::HandleVideoPacket(const AVPacket* pkt, const AVStream* stream) {
    EmbeddedVideoSlot* slot = nullptr;
    for (EmbeddedVideoSlot& candidate : video_slots_) {
        if (candidate.stream_index == pkt->stream_index) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr || pkt->data == nullptr || pkt->size <= 0)
        return;

    const double tb = (stream->time_base.den > 0) ? av_q2d(stream->time_base) : 0.0;
    const double ts = (pkt->pts != AV_NOPTS_VALUE && tb > 0.0) ? static_cast<double>(pkt->pts) * tb : -1.0;

    std::vector<uint8_t> triplets;
    // demuxer 已经提取过的 A53 side data 优先（MP4 的 c608/c708 轨与部分 TS 封装会带），
    // 避免再从码流里挖一遍造成重复计数。
    const AVPacketSideData* side = av_packet_side_data_get(pkt->side_data, pkt->side_data_elems, AV_PKT_DATA_A53_CC);
    if (side != nullptr && side->data != nullptr && side->size >= 3) {
        if (side->size % 3 == 0)
            triplets.assign(side->data, side->data + side->size);
        else
            TripletsFromCcData(side->data, side->size, triplets);
    }
    if (triplets.empty()) {
        if (slot->codec_id == AV_CODEC_ID_MPEG2VIDEO || slot->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            CollectMpeg2UserData(pkt->data, pkt->size, slot->top_field_first, triplets);
        } else if (slot->codec_id == AV_CODEC_ID_H264 || slot->codec_id == AV_CODEC_ID_HEVC) {
            CollectSeiFromPacket(pkt->data, pkt->size, slot->codec_id, slot->nal_prefix_bytes, triplets);
        }
    }
    if (triplets.empty())
        return;

    // 只有 cc_data 头、没有有效三元组（cc_valid=0）的包很常见，不能因此造出一条空 CC 流
    bool has_valid = false;
    for (size_t i = 0; i + 2 < triplets.size(); i += 3) {
        if ((triplets[i] & 0x04) != 0) {
            has_valid = true;
            break;
        }
    }
    if (!has_valid)
        return;

    PushEmbeddedTriplets(slot->stream_index, stream, triplets.data(), triplets.size(), ts);
}

model::SubtitleStreamInfo* SubtitleAnalyzer::EnsureEmbeddedCaptionStream(const AVStream* stream) {
    for (model::SubtitleStreamInfo& info : result_.streams) {
        if (info.stream_index == stream->index)
            return &info;
    }
    model::SubtitleStreamInfo info;
    info.stream_index = stream->index;
    info.format = model::SubtitleFormat::Cea608;
    info.kind = model::SubtitleKind::ClosedCaption;
    const char* video_codec = avcodec_get_name(stream->codecpar->codec_id);
    info.codec_name = video_codec ? video_codec : "";
    info.embedded = true;
    info.note = "视频内嵌隐藏字幕（MPEG-2 user_data / H.264·HEVC SEI 的 A53 数据），不是独立字幕轨";
    ++result_.text_stream_count;
    result_.streams.push_back(std::move(info));
    return &result_.streams.back();
}

void SubtitleAnalyzer::PushEmbeddedTriplets(int stream_index, const AVStream* stream, const uint8_t* data, size_t size,
                                            double ts) {
    if (data == nullptr || size < 3)
        return;
    model::SubtitleStreamInfo* info = EnsureEmbeddedCaptionStream(stream);
    ++info->packet_count;
    info->byte_count += static_cast<int64_t>(size);
    info->parsed = true;
    CaptionSlot* slot = FindOrCreateCaptionSlot(stream_index);
    slot->decoder.PushTriplets(data, size, ts >= 0.0 ? ts : 0.0);
}

void SubtitleAnalyzer::OnPacket(const AVPacket* pkt, const AVStream* stream) {
    if (pkt == nullptr || stream == nullptr || stream->codecpar == nullptr)
        return;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (options_.parse_text_cues)
            HandleVideoPacket(pkt, stream);
        return;
    }

    model::SubtitleStreamInfo* info = nullptr;
    for (model::SubtitleStreamInfo& s : result_.streams) {
        if (s.stream_index == pkt->stream_index) {
            info = &s;
            break;
        }
    }
    if (info == nullptr)
        return;

    ++info->packet_count;
    info->byte_count += static_cast<int64_t>(pkt->size);

    const double tb = (stream->time_base.den > 0) ? av_q2d(stream->time_base) : 0.0;
    const double ts = (pkt->pts != AV_NOPTS_VALUE && tb > 0.0) ? static_cast<double>(pkt->pts) * tb : -1.0;
    const double duration = (pkt->duration > 0 && tb > 0.0) ? static_cast<double>(pkt->duration) * tb : 0.0;
    if (ts >= 0.0) {
        if (info->packet_count == 1)
            info->start_seconds = ts;
        const double end = ts + ((duration > 0.0) ? duration : 0.0);
        if (end > info->duration_seconds)
            info->duration_seconds = end;
    }

    const bool is_cc = (info->kind == model::SubtitleKind::ClosedCaption);
    if (!options_.parse_text_cues)
        return;

    // 隐藏字幕不能按包解析：608 是"显存状态机"（预装载/上屏/滚动），708 是
    // "DTVCC 包 + service block"。这里只把 cc_data 喂给解码器，cue 在 Finish 时定稿 ——
    // 因为只有扫完整段才知道最后一句什么时候结束。
    if (is_cc) {
        CaptionSlot* slot = FindOrCreateCaptionSlot(info->stream_index);
        const size_t size = static_cast<size_t>(pkt->size);
        if (pkt->data == nullptr || size == 0)
            return;
        // 三元组（MPEG-2 cc_data / MP4 c708）与 2 字节对（MOV c608 样本）靠长度区分：
        // 3 的倍数按三元组解，其余按场 1 的字节对解。FFmpeg 的 eia_608 解码器同口径。
        if (size >= 3 && size % 3 == 0) {
            slot->decoder.PushTriplets(pkt->data, size, ts >= 0.0 ? ts : 0.0);
        } else {
            slot->decoder.PushPairSample(pkt->data, size, ts >= 0.0 ? ts : 0.0);
        }
        info->parsed = true;
        return;
    }
    if (info->kind != model::SubtitleKind::Text)
        return;

    // 整文件已重解析 / 明确不支持：跳过逐包解析，避免同一批 cue 入两遍，
    // 也避免把二进制载荷当成正文。
    if (std::find(whole_file_streams_.begin(), whole_file_streams_.end(), info->stream_index) !=
        whole_file_streams_.end()) {
        return;
    }
    if (std::find(unsupported_streams_.begin(), unsupported_streams_.end(), info->stream_index) !=
        unsupported_streams_.end()) {
        return;
    }

    if (IsNativelyParsedFormat(info->format)) {
        ParsedSubtitleCue parsed;
        if (!ParsePacketPayload(static_cast<int>(stream->codecpar->codec_id), pkt->data, static_cast<size_t>(pkt->size),
                                ts, duration, parsed)) {
            return;
        }
        AppendParsedCue(parsed, *info, ts);
        return;
    }

    // 非原生格式（MicroDVD / SAMI / RealText / STL / JACOsub ...）：交给 FFmpeg 字幕解码器。
    // 各 demuxer 已经把时间轴写进 PTS，解码器负责还原干净文本 —— 自己按字节猜格式
    // 既覆盖不了 STL 这类二进制容器，也容易把时间前缀算进阅读速度。
    AVCodecContext* context = EnsureTextDecoder(stream);
    if (context == nullptr)
        return;
    if (info->note.empty())
        info->note = "由 FFmpeg 字幕解码器解析（时间轴来自容器 PTS）";
    AVSubtitle subtitle{};
    int got = 0;
    const int ret = avcodec_decode_subtitle2(context, &subtitle, &got, pkt);
    if (ret >= 0 && got != 0)
        AppendDecodedSubtitle(subtitle, ts, duration, *info);
    if (got != 0)
        avsubtitle_free(&subtitle);
}

void SubtitleAnalyzer::AppendParsedCue(const ParsedSubtitleCue& parsed, model::SubtitleStreamInfo& info,
                                       double packet_ts) {
    if (info.cue_count >= static_cast<int>(options_.max_cues_per_stream)) {
        // 不能静默停止：报告与 UI 必须知道"后面还有，只是没入库"
        if (!info.truncated) {
            info.truncated = true;
            result_.cues_truncated = true;
        }
        return;
    }

    // 时间轴偏移探针：记录 cue 时间与承载包的 PTS 的偏差。取最小绝对值 ——
    // 正常文件里总有一条 cue 与包 PTS 对齐（偏差 ~0），整体平移才会让全部偏差都很大。
    if (packet_ts >= 0.0) {
        const double delta = parsed.start_seconds - packet_ts;
        TimelineProbe* probe = nullptr;
        for (TimelineProbe& candidate : timeline_probes_) {
            if (candidate.stream_index == info.stream_index) {
                probe = &candidate;
                break;
            }
        }
        if (probe == nullptr) {
            TimelineProbe created;
            created.stream_index = info.stream_index;
            timeline_probes_.push_back(created);
            probe = &timeline_probes_.back();
        }
        if (!probe->has_sample || std::fabs(delta) < probe->min_abs_delta) {
            probe->has_sample = true;
            probe->min_abs_delta = std::fabs(delta);
            probe->sample_delta = delta;
        }
    }

    model::SubtitleCue cue;
    cue.index = info.cue_count;
    cue.stream_index = info.stream_index;
    cue.cue_number = parsed.cue_number;
    cue.start_seconds = parsed.start_seconds;
    cue.end_seconds = parsed.end_seconds;
    cue.duration_seconds =
        (parsed.end_seconds > parsed.start_seconds) ? (parsed.end_seconds - parsed.start_seconds) : 0.0;
    cue.text = parsed.text;
    cue.raw_text = parsed.raw_text;
    cue.language = info.language;
    cue.format = info.format;
    cue.char_count = CountVisibleChars(cue.text);
    cue.empty = (cue.char_count == 0);

    ++info.cue_count;
    info.parsed = true;
    result_.cues.push_back(std::move(cue));
}

AVCodecContext* SubtitleAnalyzer::EnsureTextDecoder(const AVStream* stream) {
    if (stream == nullptr || stream->codecpar == nullptr)
        return nullptr;
    for (TextDecoderSlot& slot : text_decoders_) {
        if (slot.stream_index == stream->index)
            return slot.context;
    }

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr)
        return nullptr;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr)
        return nullptr;
    context->pkt_timebase = stream->time_base;
    // STL 这类格式的帧率/字符集放在 GSI 里，由 demuxer 写进 extradata；不传下去解码会失败
    const int extra_size = stream->codecpar->extradata_size;
    if (stream->codecpar->extradata != nullptr && extra_size > 0) {
        context->extradata =
            static_cast<uint8_t*>(av_mallocz(static_cast<size_t>(extra_size) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (context->extradata != nullptr) {
            std::memcpy(context->extradata, stream->codecpar->extradata, static_cast<size_t>(extra_size));
            context->extradata_size = extra_size;
        }
    }
    if (avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        return nullptr;
    }
    TextDecoderSlot slot;
    slot.stream_index = stream->index;
    slot.context = context;
    text_decoders_.push_back(slot);
    return context;
}

void SubtitleAnalyzer::ReleaseTextDecoders() {
    for (TextDecoderSlot& slot : text_decoders_) {
        if (slot.context != nullptr)
            avcodec_free_context(&slot.context);
    }
    text_decoders_.clear();
}

// AVSubtitle 里的 ASS 行形如 "0,0,Default,,0,0,0,,正文"（ReadOrder/Layer/Style/Name/
// MarginL/MarginR/MarginV/Effect/Text），没有起止时间 —— 时间在 AVSubtitle 的
// start_display_time / end_display_time（相对包 PTS 的毫秒）。
static std::string AssRectText(const AVSubtitleRect& rect) {
    if (rect.type == SUBTITLE_TEXT && rect.text != nullptr)
        return rect.text;
    if (rect.type != SUBTITLE_ASS || rect.ass == nullptr)
        return std::string();
    const std::string line = rect.ass;
    size_t start = 0;
    for (int i = 0; i < 8; ++i) {
        const size_t comma = line.find(',', start);
        if (comma == std::string::npos)
            return std::string();
        start = comma + 1;
    }
    return line.substr(start);
}

void SubtitleAnalyzer::AppendDecodedSubtitle(const AVSubtitle& subtitle, double pts_seconds, double duration_seconds,
                                             model::SubtitleStreamInfo& info) {
    std::string raw;
    for (unsigned i = 0; i < subtitle.num_rects; ++i) {
        const AVSubtitleRect* rect = subtitle.rects[i];
        if (rect == nullptr)
            continue;
        const std::string part = AssRectText(*rect);
        if (part.empty())
            continue;
        if (!raw.empty())
            raw.push_back('\n');
        raw += part;
    }
    // 图形字幕的 rect 没有文本：这条包只是位图，按"无文本"处理会让覆盖率虚高，直接跳过
    if (subtitle.num_rects > 0 && raw.empty())
        return;

    ParsedSubtitleCue cue;
    cue.raw_text = raw;
    cue.text = FlattenText(StripAngleTags(StripAssOverrides(raw)));
    cue.start_seconds = (pts_seconds >= 0.0) ? pts_seconds : 0.0;
    if (subtitle.start_display_time > 0)
        cue.start_seconds += static_cast<double>(subtitle.start_display_time) / 1000.0;
    if (subtitle.end_display_time > 0) {
        cue.end_seconds =
            ((pts_seconds >= 0.0) ? pts_seconds : 0.0) + static_cast<double>(subtitle.end_display_time) / 1000.0;
    } else if (duration_seconds > 0.0) {
        cue.end_seconds = cue.start_seconds + duration_seconds;
    } else {
        cue.end_seconds = cue.start_seconds;
    }
    cue.valid = true;
    AppendParsedCue(cue, info, pts_seconds);
}

bool SubtitleAnalyzer::TryParseWholeFile(const AVFormatContext* fmt) {
    if (!options_.parse_text_cues || fmt == nullptr)
        return false;
    if (source_path_.empty() || source_path_.find("://") != std::string::npos)
        return false;

    // 只处理"纯字幕文件"：有任何音视频流就仍走正常的逐包路径
    int subtitle_streams = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st == nullptr || st->codecpar == nullptr)
            continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            return false;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            ++subtitle_streams;
    }
    if (subtitle_streams != 1)
        return false;

    model::SubtitleStreamInfo* info = nullptr;
    for (model::SubtitleStreamInfo& stream : result_.streams) {
        if (stream.kind == model::SubtitleKind::Text) {
            info = &stream;
            break;
        }
    }
    if (info == nullptr)
        return false;
    if (std::find(unsupported_streams_.begin(), unsupported_streams_.end(), info->stream_index) !=
        unsupported_streams_.end()) {
        return false;
    }

    // 只有这几类有整文件解析器。MovText/tx3g、图形字幕等封装在二进制容器里，
    // 读原始字节当文本判 UTF-8 只会制造假告警，必须挡在读取之前。
    switch (info->format) {
    case model::SubtitleFormat::Srt:
    case model::SubtitleFormat::WebVtt:
    case model::SubtitleFormat::Ass:
    case model::SubtitleFormat::Ssa:
    case model::SubtitleFormat::MicroDvd:
        break;
    default:
        return false;
    }

    std::ifstream file(source_path_, std::ios::binary);
    if (!file)
        return false;
    constexpr size_t kMaxRead = 64u * 1024u * 1024u;
    std::string bytes;
    bytes.resize(kMaxRead);
    file.read(&bytes[0], static_cast<std::streamsize>(kMaxRead));
    bytes.resize(static_cast<size_t>(file.gcount()));
    if (bytes.empty())
        return false;

    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    if (!IsValidUtf8(bytes)) {
        model::SubtitleIssue issue;
        issue.type = model::SubtitleIssueType::NonUtf8Encoding;
        issue.severity = model::IssueSeverity::Warning;
        issue.stream_index = info->stream_index;
        issue.cue_index = -1;
        issue.detail = "字幕流 " + std::to_string(info->stream_index) +
                       " 的文本不是合法 UTF-8（多为 GBK/CP1251 等旧编码），阅读速度与文本判定可能失真";
        result_.issues.push_back(std::move(issue));
        non_utf8_streams_.push_back(info->stream_index);
        info->note = "文本不是合法 UTF-8，字符统计按字节近似";
    }

    std::vector<ParsedSubtitleCue> parsed;
    bool ok = false;
    switch (info->format) {
    case model::SubtitleFormat::Srt:
        ok = ParseSrtText(bytes, parsed);
        break;
    case model::SubtitleFormat::WebVtt:
        ok = ParseWebVttText(bytes, parsed);
        break;
    case model::SubtitleFormat::Ass:
    case model::SubtitleFormat::Ssa:
        ok = ParseAssText(bytes, parsed);
        break;
    case model::SubtitleFormat::MicroDvd:
        ok = ParseMicroDvdText(bytes, options_.microdvd_frame_rate, parsed);
        break;
    default:
        // 其它格式由 FFmpeg 解码器处理，不在这里抢路
        return false;
    }
    if (!ok || parsed.empty())
        return false;

    for (const ParsedSubtitleCue& cue : parsed)
        AppendParsedCue(cue, *info);
    whole_file_streams_.push_back(info->stream_index);
    if (info->note.empty())
        info->note = "纯字幕文件：整文件解析（含空事件与畸形事件）";
    return true;
}

void SubtitleAnalyzer::AppendCaptionCues(CaptionSlot& slot, double end_seconds) {
    slot.decoder.Finish(end_seconds);
    const model::CeaCaptionResult& captions = slot.decoder.result();

    model::SubtitleStreamInfo* info = nullptr;
    for (model::SubtitleStreamInfo& stream : result_.streams) {
        if (stream.stream_index == slot.stream_index) {
            info = &stream;
            break;
        }
    }
    if (info == nullptr)
        return;

    info->cc608 = captions.cc608;
    info->cc708 = captions.cc708;

    // 服务层摘要放在 note 最前面：流表只有"说明"一列能承载它，而"这条流里到底有
    // 哪几个通道/服务、708 有没有丢包"恰恰是审核最先要看的结论。
    std::string summary;
    if (captions.cc608.Any()) {
        summary = "CEA-608 " + captions.cc608.ChannelText() + "（POP-ON " +
                  std::to_string(captions.cc608.pop_on_count) + " / PAINT-ON " +
                  std::to_string(captions.cc608.paint_on_count) + " / ROLL-UP " +
                  std::to_string(captions.cc608.roll_up_count) + "，XDS " + std::to_string(captions.cc608.xds_count) +
                  " 条）";
    }
    if (captions.cc708.Any()) {
        if (!summary.empty())
            summary += "；";
        summary += "CEA-708 " + captions.cc708.ServiceText() + "（DTVCC 包 " +
                   std::to_string(captions.cc708.packet_count) + "，服务块 " +
                   std::to_string(captions.cc708.service_block_count) + "，丢包 " +
                   std::to_string(captions.cc708.sequence_gap_count) + "，窗口 " +
                   std::to_string(captions.cc708.window_count) + "）";
    }
    if (!summary.empty()) {
        info->note = info->note.empty() ? summary : (summary + "｜" + info->note);
    }
    // 解码器的说明追加到流 note 上：既有"图形字幕仅 metadata"那类降级说明，
    // 也有"DTVCC 包序号跳变"这种会影响审核结论的告警，两者都要能看到。
    auto append_note = [info](const std::string& note) {
        if (note.empty())
            return;
        info->note = info->note.empty() ? note : (info->note + "；" + note);
    };
    append_note(captions.cc608.note);
    append_note(captions.cc708.note);

    const int limit = static_cast<int>(options_.max_cues_per_stream);
    for (const model::CaptionCue& caption : captions.cues) {
        if (info->cue_count >= limit) {
            info->truncated = true;
            result_.cues_truncated = true;
            break;
        }
        model::SubtitleCue cue;
        cue.index = info->cue_count;
        cue.stream_index = info->stream_index;
        cue.cue_number = -1;
        cue.start_seconds = caption.start_seconds;
        cue.end_seconds = caption.end_seconds;
        cue.duration_seconds =
            (caption.end_seconds > caption.start_seconds) ? (caption.end_seconds - caption.start_seconds) : 0.0;
        cue.text = caption.text;
        cue.raw_text = caption.text;
        cue.language = info->language;
        cue.format = caption.kind_708 ? model::SubtitleFormat::Cea708 : model::SubtitleFormat::Cea608;
        cue.caption_channel = caption.channel;
        cue.caption_708 = caption.kind_708;
        cue.char_count = CountVisibleChars(cue.text);
        cue.empty = (cue.char_count == 0);
        ++info->cue_count;
        result_.cues.push_back(std::move(cue));
    }
    info->parsed = (info->cue_count > 0);
}

void SubtitleAnalyzer::Finish(double media_duration_seconds) {
    // 隐藏字幕先定稿：708 的 cue 起止只能靠"上屏/擦除"事件推，收尾时才知道最后一句
    // 何时结束（用媒体时长兜底；没有时长就沿用最后一条 cue 的 end）。
    double caption_end = media_duration_seconds;
    if (caption_end <= 0.0 && !result_.cues.empty()) {
        caption_end = result_.cues.back().end_seconds;
    }
    for (CaptionSlot& slot : caption_slots_) {
        AppendCaptionCues(slot, caption_end);
    }

    // 按流分组校验（包是交错来的，同一流的 cue 在数组里并不连续）
    //
    // 隐藏字幕还要再按通道/服务细分：CC1 与 CC2、708 的 S1 与 S2 是**各自独立的时间线**
    // （双语节目本来就同时上屏），混在一起校验会把合法的双语重叠判成 Overlap、
    // 把两条轨的停留时长算成一条的超长。caption_channel 为 0 的普通文本轨不受影响。
    for (const model::SubtitleStreamInfo& stream : result_.streams) {
        std::vector<int> channels;
        for (const model::SubtitleCue& cue : result_.cues) {
            if (cue.stream_index != stream.stream_index)
                continue;
            if (std::find(channels.begin(), channels.end(), cue.caption_channel) == channels.end()) {
                channels.push_back(cue.caption_channel);
            }
        }
        for (int channel : channels) {
            std::vector<model::SubtitleCue> group;
            std::vector<size_t> positions;
            for (size_t i = 0; i < result_.cues.size(); ++i) {
                if (result_.cues[i].stream_index != stream.stream_index)
                    continue;
                if (result_.cues[i].caption_channel != channel)
                    continue;
                group.push_back(result_.cues[i]);
                positions.push_back(i);
            }
            if (group.empty())
                continue;

            const bool skip_garbled = std::find(non_utf8_streams_.begin(), non_utf8_streams_.end(),
                                                stream.stream_index) != non_utf8_streams_.end();
            ValidateCues(group, result_.issues, options_, media_duration_seconds, skip_garbled);
            for (size_t i = 0; i < group.size(); ++i) {
                result_.cues[positions[i]] = group[i];
            }
        }
    }

    // 时间轴整体偏移：cue 文本里的时间与承载包 PTS 差一个常量，说明容器声明的
    // 偏移（MP4 elst / HLS WebVTT 的 X-TIMESTAMP-MAP）与实际时间轴不一致，
    // 播放器上表现为字幕整体提前或延后。只有"最小偏差"都超过容差才算，
    // 避免把"前 10 秒没字幕"这种正常留白误判成偏移。
    if (options_.check_timeline_offset) {
        const double tolerance = options_.timeline_offset_tolerance_seconds;
        for (const model::SubtitleStreamInfo& stream : result_.streams) {
            if (stream.kind != model::SubtitleKind::Text)
                continue;
            double first_cue = -1.0;
            for (const model::SubtitleCue& cue : result_.cues) {
                if (cue.stream_index != stream.stream_index)
                    continue;
                if (first_cue < 0.0 || cue.start_seconds < first_cue)
                    first_cue = cue.start_seconds;
            }
            if (first_cue < 0.0)
                continue;

            const TimelineProbe* probe = nullptr;
            for (const TimelineProbe& candidate : timeline_probes_) {
                if (candidate.stream_index == stream.stream_index) {
                    probe = &candidate;
                    break;
                }
            }
            char buf[256] = {0};
            bool offset = false;
            if (first_cue < -tolerance) {
                std::snprintf(buf, sizeof(buf), "字幕流 %d 出现负时间（首条 %.3fs），时间轴可能整体偏移",
                              stream.stream_index, first_cue);
                offset = true;
            } else if (probe != nullptr && probe->has_sample && probe->min_abs_delta > tolerance) {
                std::snprintf(buf, sizeof(buf),
                              "字幕流 %d 的时间轴与容器时间戳整体相差 %.3fs（疑似 elst / X-TIMESTAMP-MAP 未生效）",
                              stream.stream_index, probe->sample_delta);
                offset = true;
            }
            if (offset) {
                model::SubtitleIssue issue;
                issue.type = model::SubtitleIssueType::TimelineOffset;
                issue.severity = model::IssueSeverity::Warning;
                issue.stream_index = stream.stream_index;
                issue.cue_index = -1;
                issue.start_seconds = first_cue;
                issue.detail = buf;
                result_.issues.push_back(std::move(issue));
            }
        }
    }

    if (options_.check_language) {
        for (const model::SubtitleStreamInfo& stream : result_.streams) {
            // 内嵌 CC 没有独立字幕轨，自然没有 language tag；把它按"缺语言"报出来
            // 只会制造无法修复的告警。
            if (stream.embedded)
                continue;
            // 空串和 und 要分开说：前者是压根没写 tag，后者是写了但等于没写，
            // 两种情况的修法不同（补一个 vs 改成具体语言码）。
            if (IsUnspecifiedLanguage(stream.language)) {
                model::SubtitleIssue issue;
                issue.type = model::SubtitleIssueType::MissingLanguage;
                issue.severity = model::IssueSeverity::Info;
                issue.stream_index = stream.stream_index;
                issue.cue_index = -1;
                issue.detail = "字幕流 " + std::to_string(stream.stream_index) +
                               (stream.language.empty() ? " 没有 language tag"
                                                        : " 的 language tag 是 " + stream.language + "（未指定语言）");
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

    // 文本字幕一条 cue 都没解析出来时必须显式报出：格式不支持、编码不匹配、
    // 解析器失败，最终都会表现成"这条流没有问题"——这是 QC 最危险的假阴性。
    if (options_.parse_text_cues) {
        for (const model::SubtitleStreamInfo& stream : result_.streams) {
            if (stream.kind != model::SubtitleKind::Text)
                continue;
            if (stream.cue_count > 0 || stream.truncated)
                continue;
            if (std::find(unsupported_streams_.begin(), unsupported_streams_.end(), stream.stream_index) !=
                unsupported_streams_.end()) {
                continue; // 已经有"格式不支持"的结论，不重复报
            }
            model::SubtitleIssue issue;
            issue.type = model::SubtitleIssueType::NoCues;
            issue.severity = model::IssueSeverity::Warning;
            issue.stream_index = stream.stream_index;
            issue.cue_index = -1;
            issue.detail = "字幕流 " + std::to_string(stream.stream_index) + "（" + stream.codec_name +
                           "）没有解析出任何字幕，无法做文本级检查";
            result_.issues.push_back(std::move(issue));
        }
    }

    result_.total_cue_count = static_cast<int>(result_.cues.size());
    result_.analyzed = true;
}

} // namespace videoeye
