#include "core/analysis/diagnostics/CeaCaptionDecoder.h"

#include <algorithm>

namespace videoeye {
namespace {

constexpr int kRows = 15;

// ---------------------------------------------------------------------------
// 字符集。608 的基本集不是纯 ASCII：0x2A/0x5C/0x5E/0x5F/0x60/0x7B-0x7F 是重音字母
// 与实心块。这几格错一位，整句中文节目的英文字幕就会冒出乱码块。
// ---------------------------------------------------------------------------
std::string BasicChar(uint8_t value) {
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
    if (value >= 0x20 && value <= 0x7E)
        return std::string(1, static_cast<char>(value));
    return std::string();
}

// 0x11 0x30-0x3F 特殊字符
const char* SpecialChar(uint8_t value) {
    static const char* kTable[16] = {
        "\xC2\xAE",     // ®
        "\xC2\xB0",     // °
        "\xC2\xBD",     // ½
        "\xC2\xBF",     // ¿
        "\xE2\x84\xA2", // ™
        "\xC2\xA2",     // ¢
        "\xC2\xA3",     // £
        "\xE2\x99\xAA", // ♪
        "\xC3\xA0",     // à
        " ",            // 透明空格
        "\xC3\xA8",     // è
        "\xC3\xA2",     // â
        "\xC3\xAA",     // ê
        "\xC3\xAE",     // î
        "\xC3\xB4",     // ô
        "\xC3\xBB",     // û
    };
    if (value > 0x0F)
        return nullptr;
    return kTable[value];
}

// 0x12 0x20-0x3F 扩展字符（西班牙语/法语）
const char* ExtendedLatinChar(uint8_t value) {
    static const char* kTable[32] = {
        "\xC3\x81",
        "\xC3\x89",
        "\xC3\x93",
        "\xC3\x9A",
        "\xC3\x9C",
        "\xC3\xBC",
        "\xE2\x80\x98",
        "\xC2\xA1",
        "*",
        "'",
        "\xE2\x80\x94",
        "\xC2\xA9",
        "\xE2\x84\xA0",
        "\xE2\x80\xA2",
        "\xE2\x80\x9C",
        "\xE2\x80\x9D",
        "\xC3\x80",
        "\xC3\x82",
        "\xC3\x87",
        "\xC3\x88",
        "\xC3\x8A",
        "\xC3\x8B",
        "\xC3\xAB",
        "\xC3\x8E",
        "\xC3\x8F",
        "\xC3\xAF",
        "\xC3\x94",
        "\xC3\x99",
        "\xC3\xB9",
        "\xC3\x9B",
        "\xC2\xAB",
        "\xC2\xBB",
    };
    if (value > 0x1F)
        return nullptr;
    return kTable[value];
}

// 0x13 0x20-0x3F 扩展字符（葡萄牙语/德语/丹麦语）
const char* ExtendedEuroChar(uint8_t value) {
    static const char* kTable[32] = {
        "\xC3\x83",
        "\xC3\xA3",
        "\xC3\x8D",
        "\xC3\x8C",
        "\xC3\xAC",
        "\xC3\x92",
        "\xC3\xB2",
        "\xC3\x95",
        "\xC3\xB5",
        "{",
        "}",
        "\\",
        "^",
        "_",
        "|",
        "~",
        "\xC3\x84",
        "\xC3\xA4",
        "\xC3\x96",
        "\xC3\xB6",
        "\xC3\x9F",
        "\xC2\xA5",
        "\xC2\xA4",
        "\xE2\x94\x82",
        "\xC3\x85",
        "\xC3\xA5",
        "\xC3\x98",
        "\xC3\xB8",
        "\xE2\x94\x8C",
        "\xE2\x94\x90",
        "\xE2\x94\x94",
        "\xE2\x94\x98",
    };
    if (value > 0x1F)
        return nullptr;
    return kTable[value];
}

// 行首的 ASCII 空白（透明空格）在拼接时保留，但整行只有空白就按空行处理
std::string TrimRight(const std::string& text) {
    size_t end = text.size();
    while (end > 0 && text[end - 1] == ' ')
        --end;
    return text.substr(0, end);
}

// 一份显存 -> 上屏文本（行内尾部空格去掉，空行不占位）
std::string DisplayText(const std::vector<std::string>& rows) {
    std::string text;
    for (const std::string& line : rows) {
        const std::string trimmed = TrimRight(line);
        if (trimmed.empty())
            continue;
        if (!text.empty())
            text += '\n';
        text += trimmed;
    }
    return text;
}

// 列号是"字符列"，缓冲区是 UTF-8 字节：混用会把多字节字符劈开
// （实测："Á" 被写成 0xC3 + 下一个字符，整句乱码）。
size_t ByteOffsetForColumn(const std::string& line, int col) {
    if (col <= 0)
        return 0;
    size_t offset = 0;
    int count = 0;
    while (offset < line.size() && count < col) {
        ++offset;
        while (offset < line.size() && (static_cast<unsigned char>(line[offset]) & 0xC0) == 0x80) {
            ++offset;
        }
        ++count;
    }
    return offset;
}

int CharCount(const std::string& line) {
    int count = 0;
    for (unsigned char c : line) {
        if ((c & 0xC0) == 0x80)
            continue;
        ++count;
    }
    return count;
}

} // namespace

void Cea608Decoder::Reset() {
    for (Channel& channel : channels_) {
        channel = Channel{};
        channel.displayed.assign(kRows, std::string());
        channel.nondisplayed.assign(kRows, std::string());
    }
    field_channel_[0] = false;
    field_channel_[1] = false;
    for (int& code : last_control_)
        code = -1;
    cues_.clear();
    stats_ = model::Cea608Stats{};
    touched_ = false;
}

Cea608Decoder::Channel& Cea608Decoder::ChannelFor(int index) {
    const int clamped = std::max(0, std::min(3, index));
    Channel& channel = channels_[clamped];
    if (channel.displayed.empty())
        channel.displayed.assign(kRows, std::string());
    if (channel.nondisplayed.empty())
        channel.nondisplayed.assign(kRows, std::string());
    return channel;
}

void Cea608Decoder::PushPair(uint8_t b1, uint8_t b2, int field, double ts_seconds) {
    const uint8_t first = static_cast<uint8_t>(b1 & 0x7F);
    const uint8_t second = static_cast<uint8_t>(b2 & 0x7F);
    const int field_index = (field == 2) ? 1 : 0;
    if (field_index == 1)
        stats_.field2_used = true;

    // 全 0 对是填充（部分封装用它补齐到偶数长度），不是字幕内容
    if (first == 0 && second == 0) {
        ++stats_.invalid_pairs;
        return;
    }
    // 场 2 的 0x01-0x0E 是 XDS 包（节目信息/评级/CPCM），绝不能当字幕字符
    if (field_index == 1 && first >= 0x01 && first <= 0x0E) {
        ++stats_.xds_count;
        return;
    }

    touched_ = true;
    const int base = field_index * 2;

    // ---- 控制类（0x10-0x1F）：先定通道，再分派 ----
    if (first >= 0x10 && first <= 0x1F) {
        const int channel_offset = ((first & 0x08) != 0) ? 1 : 0;
        const int channel_index = base + channel_offset;
        field_channel_[field_index] = (channel_offset != 0);
        stats_.channel_mask |= (1 << channel_index);
        // 重复控制码抑制：同一通道连续收到**完全相同**的控制字节对时只执行一次
        // （608 规范要求控制码连发两遍抗误码）。不抑制的话 EOC/EDM 会被执行两遍。
        const int packed = (static_cast<int>(first) << 8) | static_cast<int>(second);
        if (last_control_[channel_index] == packed) {
            ++stats_.duplicate_control_count;
            return;
        }
        last_control_[channel_index] = packed;
        HandleControl(ChannelFor(channel_index), channel_index, first, second, ts_seconds);
        return;
    }

    // ---- 可打印字符：写到当前通道的光标位置 ----
    const int channel_index = base + (field_channel_[field_index] ? 1 : 0);
    stats_.channel_mask |= (1 << channel_index);
    // 控制码的"连发"语义只对紧邻的重复控制码成立：中间来了可见字符就断开。
    last_control_[channel_index] = -1;
    Channel& channel = ChannelFor(channel_index);
    const std::string text = BasicChar(first);
    if (!text.empty())
        WriteChar(channel, text, ts_seconds);
    const std::string text2 = BasicChar(second);
    if (!text2.empty())
        WriteChar(channel, text2, ts_seconds);
    TrackPending(channel, ts_seconds);
}

void Cea608Decoder::HandleControl(Channel& channel, int channel_index, uint8_t b1, uint8_t b2, double ts_seconds) {
    const int code = b1 & 0x07;
    switch (code) {
    case 0x00: // 0x10/0x18：base row 11
        HandlePac(channel, b1, b2);
        break;
    case 0x01: // 0x11/0x19：行内属性（0x20-0x2F）/ 特殊字符（0x30-0x3F）/ PAC
        if (b2 >= 0x20 && b2 <= 0x2F) {
            ++stats_.mid_row_count;
        } else if (b2 >= 0x30 && b2 <= 0x3F) {
            const char* text = SpecialChar(static_cast<uint8_t>(b2 - 0x30));
            ++stats_.special_char_count;
            if (text != nullptr)
                WriteChar(channel, text, ts_seconds);
            TrackPending(channel, ts_seconds);
        } else {
            HandlePac(channel, b1, b2);
        }
        break;
    case 0x02:   // 0x12/0x1A：西班牙语/法语扩展字符
    case 0x03: { // 0x13/0x1B：葡萄牙语/德语/丹麦语扩展字符
        if (b2 < 0x20 || b2 > 0x3F)
            return;
        const char* text = (code == 0x02) ? ExtendedLatinChar(static_cast<uint8_t>(b2 - 0x20))
                                          : ExtendedEuroChar(static_cast<uint8_t>(b2 - 0x20));
        ++stats_.extended_char_count;
        // 扩展字符是"退格 + 替换"语义：先吃掉上一格
        Backspace(channel);
        if (text != nullptr)
            WriteChar(channel, text, ts_seconds);
        TrackPending(channel, ts_seconds);
        break;
    }
    case 0x04: { // 0x14/0x1C：杂项控制 / PAC
        if (b2 >= 0x40) {
            HandlePac(channel, b1, b2);
            return;
        }
        ++stats_.control_count;
        // 先提交"变化前"的画面：EDM/CR/RU 会改显存，不先提交就会把旧画面
        // 的结束时间挪到事件之后（实测差一整句）
        CommitChannel(channel, channel_index, ts_seconds);
        switch (b2) {
        case 0x20: // RCL：pop-on 装载
            channel.mode = Mode::PopOn;
            break;
        case 0x21: // BS
            Backspace(channel);
            break;
        case 0x24: // DER
            DeleteToEndOfRow(channel);
            break;
        case 0x25:   // RU2
        case 0x26:   // RU3
        case 0x27: { // RU4
            ++stats_.roll_up_count;
            channel.mode = Mode::RollUp;
            channel.rollup_rows = b2 - 0x23;
            channel.row = kRows - 1;
            channel.col = 0;
            break;
        }
        case 0x28: // FON：闪烁，忽略
            break;
        case 0x29: // RDC：paint-on 直显
            ++stats_.paint_on_count;
            channel.mode = Mode::PaintOn;
            break;
        case 0x2A: // TR：text mode 重启（不是字幕）
            ClearDisplayed(channel);
            ClearNonDisplayed(channel);
            break;
        case 0x2B: // RTD：text display
            break;
        case 0x2C: // EDM：擦除显示内存
            ClearDisplayed(channel);
            break;
        case 0x2D: // CR
            CarriageReturn(channel);
            break;
        case 0x2E: // ENM：擦除预装载内存
            ClearNonDisplayed(channel);
            break;
        case 0x2F: // EOC：pop-on 上屏（交换两块显存）
            ++stats_.pop_on_count;
            channel.mode = Mode::PopOn;
            std::swap(channel.displayed, channel.nondisplayed);
            channel.nondisplayed.assign(kRows, std::string());
            channel.row = kRows - 1;
            channel.col = 0;
            break;
        default:
            break;
        }
        CommitChannel(channel, channel_index, ts_seconds);
        break;
    }
    case 0x05: { // 0x15/0x1D：TO1-TO3（制表偏移）与杂项；只计数不影响文本
        if (b2 >= 0x20 && b2 <= 0x2F) {
            ++stats_.control_count;
            if (b2 >= 0x21 && b2 <= 0x23) {
                channel.col = std::min(31, channel.col + static_cast<int>(b2 - 0x20));
            }
        } else if (b2 >= 0x40) {
            HandlePac(channel, b1, b2);
        }
        break;
    }
    case 0x06: // 0x16/0x1E：保留
        ++stats_.control_count;
        break;
    case 0x07: { // 0x17/0x1F：TO1-TO3 / 背景属性
        if (b2 >= 0x21 && b2 <= 0x23) {
            channel.col = std::min(31, channel.col + static_cast<int>(b2 - 0x20));
        }
        ++stats_.control_count;
        break;
    }
    default:
        break;
    }
}

void Cea608Decoder::HandlePac(Channel& channel, uint8_t b1, uint8_t b2) {
    if (b2 < 0x40 || b2 > 0x7F)
        return;
    const int code = b1 & 0x07;
    // 行号映射（CEA-608 Table 52）：0x11->1/2、0x12->3/4 ... 0x17->13/14；
    // 0x10 是"第 11 行"的老写法。第 15 行只能由 roll-up 基行进，没有 PAC。
    int row = 11;
    if (code != 0) {
        row = (code - 1) * 2 + (((b2 & 0x20) != 0) ? 2 : 1);
    }
    stats_.max_row = std::max(stats_.max_row, row);
    ++stats_.pac_count;
    channel.row = std::max(0, std::min(kRows - 1, row - 1));
    channel.col = 0;
}

void Cea608Decoder::WriteChar(Channel& channel, const std::string& utf8, double ts_seconds) {
    (void)ts_seconds;
    if (utf8.empty())
        return;
    std::vector<std::string>& buffer = (channel.mode == Mode::PopOn) ? channel.nondisplayed : channel.displayed;
    if (buffer.size() != static_cast<size_t>(kRows))
        buffer.assign(kRows, std::string());
    const int row = std::max(0, std::min(kRows - 1, channel.row));
    std::string& line = buffer[static_cast<size_t>(row)];
    const int col_chars = std::max(0, channel.col);
    const int existing = CharCount(line);
    if (col_chars > existing) {
        // 列定位：不足补空格。608 没有"插入"语义，同一格重写就是替换一个字符。
        line.append(static_cast<size_t>(col_chars - existing), ' ');
        line += utf8;
    } else {
        const size_t byte_col = ByteOffsetForColumn(line, col_chars);
        size_t cut_end = byte_col + 1;
        while (cut_end < line.size() && (static_cast<unsigned char>(line[cut_end]) & 0xC0) == 0x80) {
            ++cut_end;
        }
        line.replace(byte_col, cut_end - byte_col, utf8);
    }
    channel.col += 1;
}

void Cea608Decoder::Backspace(Channel& channel) {
    std::vector<std::string>& buffer = (channel.mode == Mode::PopOn) ? channel.nondisplayed : channel.displayed;
    if (buffer.size() != static_cast<size_t>(kRows))
        return;
    const int row = std::max(0, std::min(kRows - 1, channel.row));
    std::string& line = buffer[static_cast<size_t>(row)];
    if (line.empty()) {
        channel.col = std::max(0, channel.col - 1);
        return;
    }
    // 按 UTF-8 码点回退一格
    size_t cut = line.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
        --cut;
    line.erase(cut);
    // 列是"字符列"：WriteChar 每个字符 +1，这里也必须 -1（多字节字符只占 1 列）
    channel.col = std::max(0, channel.col - 1);
}

void Cea608Decoder::DeleteToEndOfRow(Channel& channel) {
    std::vector<std::string>& buffer = (channel.mode == Mode::PopOn) ? channel.nondisplayed : channel.displayed;
    if (buffer.size() != static_cast<size_t>(kRows))
        return;
    const int row = std::max(0, std::min(kRows - 1, channel.row));
    std::string& line = buffer[static_cast<size_t>(row)];
    if (channel.col < CharCount(line)) {
        line.erase(ByteOffsetForColumn(line, channel.col));
    }
}

void Cea608Decoder::CarriageReturn(Channel& channel) {
    if (channel.mode == Mode::RollUp) {
        // 滚动：把基行以上的内容整体上移一行，最底行清空
        const int rows = std::max(2, std::min(4, channel.rollup_rows));
        const int top = std::max(0, kRows - rows);
        for (int row = top; row < kRows - 1; ++row) {
            channel.displayed[static_cast<size_t>(row)] = channel.displayed[static_cast<size_t>(row + 1)];
        }
        channel.displayed[kRows - 1].clear();
        channel.row = kRows - 1;
        channel.col = 0;
        return;
    }
    channel.row = std::min(kRows - 1, channel.row + 1);
    channel.col = 0;
}

void Cea608Decoder::ClearDisplayed(Channel& channel) {
    channel.displayed.assign(kRows, std::string());
}

void Cea608Decoder::ClearNonDisplayed(Channel& channel) {
    channel.nondisplayed.assign(kRows, std::string());
}

void Cea608Decoder::TrackPending(Channel& channel, double ts_seconds) {
    const std::string text = DisplayText(channel.displayed);
    // pending_ts 记的是"画面开始变成新内容"的时刻，而不是事件时刻：roll-up 里
    // 新一行是逐字符写进来的，结束上一条 cue 的时间应该是第一个字符到达时。
    if (channel.pending_text == channel.active_text)
        channel.pending_ts = ts_seconds;
    channel.pending_text = text;
}

void Cea608Decoder::CommitChannel(Channel& channel, int channel_index, double ts_seconds) {
    const std::string text = DisplayText(channel.displayed);
    if (channel.pending_text == channel.active_text)
        channel.pending_ts = ts_seconds;
    if (text == channel.active_text) {
        channel.pending_text = text;
        return;
    }
    CloseActive(channel, channel_index, channel.pending_ts);
    if (!text.empty()) {
        channel.active_text = text;
        channel.active_start = channel.pending_ts;
        channel.active_mode = channel.mode;
    }
    channel.pending_text = text;
    channel.pending_ts = ts_seconds;
}

void Cea608Decoder::CloseActive(Channel& channel, int channel_index, double ts_seconds) {
    if (channel.active_text.empty())
        return;
    model::CaptionCue cue;
    cue.start_seconds = channel.active_start;
    cue.end_seconds = std::max(ts_seconds, channel.active_start);
    cue.text = channel.active_text;
    cue.channel = channel_index + 1; // CC1..CC4
    cue.kind_708 = false;
    switch (channel.active_mode) {
    case Mode::PopOn:
        cue.mode = "POP-ON";
        break;
    case Mode::PaintOn:
        cue.mode = "PAINT-ON";
        break;
    case Mode::RollUp:
        cue.mode = "ROLL-UP";
        break;
    }
    cues_.push_back(std::move(cue));
    channel.active_text.clear();
}

void Cea608Decoder::Finish(double end_ts_seconds) {
    for (int i = 0; i < 4; ++i) {
        // 先把"还没提交的画面"提交出来（最后一句可能从未遇到结构事件）
        CommitChannel(channels_[i], i, end_ts_seconds);
        CloseActive(channels_[i], i, end_ts_seconds);
    }
    if (touched_ && stats_.channel_mask == 0) {
        stats_.note = "收到了 cc_data，但没有识别出任何 608 控制码或可打印字符";
    }
}

} // namespace videoeye
