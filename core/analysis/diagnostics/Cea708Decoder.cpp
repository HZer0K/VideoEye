#include "core/analysis/diagnostics/CeaCaptionDecoder.h"

#include <algorithm>

namespace videoeye {
namespace {

// 码点 -> UTF-8（口径与项目其它地方一致：只做编码，不判断字体是否可用）
std::string Utf8Encode(std::uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

// G2/G3 扩展字符集只做最小映射：这几个是实际会出现在字幕里的（省略号、空格、
// 商标），其余返回 0 表示"跳过"。宁可少一个字，也不要凭空造字。
const char* G2Char(uint8_t code) {
    switch (code) {
    case 0x20:
        return " "; // 透明空格
    case 0x21:
        return "\xC2\xA0"; // 不换行空格
    case 0x25:
        return "\xE2\x80\xA6"; // …
    case 0x2A:
        return "\xC5\xA0"; // Š
    case 0x2C:
        return "\xC5\x92"; // Œ
    case 0x30:
        return "\xE2\x96\x88"; // █
    case 0x31:
        return "\xE2\x80\x98"; // ‘
    case 0x32:
        return "\xE2\x80\x99"; // ’
    case 0x33:
        return "\xE2\x80\x9C"; // “
    case 0x34:
        return "\xE2\x80\x9D"; // ”
    case 0x35:
        return "\xE2\x80\xA2"; // •
    case 0x39:
        return "\xE2\x84\xA2"; // ™
    case 0x3A:
        return "\xC5\xA1"; // š
    case 0x3C:
        return "\xC5\x93"; // œ
    case 0x3D:
        return "\xE2\x84\xA0"; // ℠
    case 0x3F:
        return "\xC5\xB8"; // Ÿ
    default:
        return nullptr;
    }
}

std::string TrimRightSpaces(const std::string& text) {
    size_t end = text.size();
    while (end > 0 && text[end - 1] == ' ')
        --end;
    return text.substr(0, end);
}

} // namespace

void Cea708Decoder::Window::Reset() {
    defined = false;
    visible = false;
    row_count = 0;
    col_count = 0;
    rows.clear();
    pen_row = 0;
    pen_col = 0;
}

std::string Cea708Decoder::Window::BuildText() const {
    std::string text;
    for (const std::string& row : rows) {
        const std::string trimmed = TrimRightSpaces(row);
        if (trimmed.empty())
            continue;
        if (!text.empty())
            text += '\n';
        text += trimmed;
    }
    return text;
}

void Cea708Decoder::Reset() {
    for (Service& service : services_)
        service = Service{};
    packet_.clear();
    expected_size_ = 0;
    last_sequence_ = -1;
    cues_.clear();
    stats_ = model::Cea708Stats{};
}

void Cea708Decoder::PushCcData(uint8_t d0, uint8_t d1, bool packet_start, double ts_seconds) {
    if (packet_start) {
        const int sequence = (d0 >> 6) & 0x03;
        const int size_code = d0 & 0x3F;
        // ❗ 低 6 bit 是 packet_size_code，**不是**载荷字节数（CEA-708 §8.10.1）：
        //      packet_size = 2 * packet_size_code - 1
        //    code == 0 表示 127 字节（特例，不能按 2*0-1 = -1 算）。
        //   当成字节数会让包边界整体错位，ParsePacket 拿到的是垃圾，
        //   标准封包的素材会一条字幕都解不出来。
        const std::size_t size = (size_code == 0) ? 127u : static_cast<std::size_t>(size_code * 2 - 1);
        if (last_sequence_ >= 0 && sequence != ((last_sequence_ + 1) & 0x03)) {
            ++stats_.sequence_gap_count;
        }
        last_sequence_ = sequence;
        packet_.clear(); // 上一个包没凑满就被新包头打断：残包按丢包处理
        expected_size_ = size;
        packet_.push_back(d1); // 包头后的第一个载荷字节就在这个三元组里
        if (packet_.size() >= expected_size_) {
            ParsePacket(ts_seconds);
            packet_.clear();
            expected_size_ = 0;
        }
        return;
    }

    if (expected_size_ == 0)
        return; // 群组外孤立的 packet data，按规范丢弃
    packet_.push_back(d0);
    if (packet_.size() >= expected_size_) {
        ParsePacket(ts_seconds);
        packet_.clear();
        expected_size_ = 0;
        return;
    }
    packet_.push_back(d1);
    if (packet_.size() >= expected_size_) {
        ParsePacket(ts_seconds);
        packet_.clear();
        expected_size_ = 0;
    }
}

void Cea708Decoder::ParsePacket(double ts_seconds) {
    ++stats_.packet_count;
    std::size_t index = 0;
    while (index < packet_.size()) {
        const uint8_t header = packet_[index++];
        int service_number = (header & 0xE0) >> 5;
        int block_size = header & 0x1F;
        if (service_number == 7) {
            if (index >= packet_.size())
                break;
            service_number = packet_[index++] & 0x3F;
            // 扩展服务号占了块内第一个字节，块长里把它扣掉
            if (block_size > 0)
                --block_size;
        }
        if (block_size <= 0)
            continue; // null service block
        if (index + static_cast<std::size_t>(block_size) > packet_.size()) {
            block_size = static_cast<int>(packet_.size() - index); // 截断保护
        }
        if (block_size <= 0)
            break;
        ++stats_.service_block_count;
        if (service_number > 0 && service_number < 64) {
            ParseServiceBlock(service_number, packet_.data() + index, static_cast<std::size_t>(block_size), ts_seconds);
        }
        index += static_cast<std::size_t>(block_size);
    }
}

void Cea708Decoder::ParseServiceBlock(int service_number, const uint8_t* data, std::size_t size, double ts_seconds) {
    Service& service = services_[service_number];
    if (!service.seen) {
        service.seen = true;
        stats_.services.push_back(service_number);
        std::sort(stats_.services.begin(), stats_.services.end());
    }
    ExecuteCommands(service, data, size, ts_seconds);
    SyncService(service, service_number, ts_seconds);
}

void Cea708Decoder::HandleDefineWindow(Service& service, int window_id, const uint8_t* args) {
    if (window_id < 0 || window_id > 7)
        return;
    Window& window = service.windows[window_id];
    // DefineWindow 的 6 字节参数（CEA-708 §8.10.4）：
    //   [0] 0b00 visible row_lock column_lock priority(3)
    //   [1] 0b0 relative_positioning anchor_vertical(6)
    //   [2] anchor_horizontal(8)
    //   [3] 0b0000 row_count(4)
    //   [4] 0b00 column_count(6)
    //   [5] 0b00 window_style(3) pen_style(3)
    // 这里只取"可见"与"行列数"：锚点/样式/优先级不影响文本内容，也不参与本项目的
    // 画面合成（我们没有 708 渲染器）。
    const bool visible = (args[0] & 0x20) != 0;
    // ❗ row_count / column_count 在 args[3] / args[4]，不是 args[1] / args[2]。
    const int row_count = std::max(1, static_cast<int>(args[3] & 0x0F));
    const int col_count = std::max(1, static_cast<int>(args[4] & 0x3F));
    window.defined = true;
    window.visible = visible;
    if (window.row_count != row_count || window.col_count != col_count) {
        window.row_count = row_count;
        window.col_count = col_count;
        window.rows.assign(static_cast<size_t>(row_count), std::string());
        window.pen_row = 0;
        window.pen_col = 0;
    }
    service.current_window = window_id;
    ++stats_.window_count;
}

Cea708Decoder::Window* Cea708Decoder::ActiveWindow(Service& service) {
    const int id = std::max(0, std::min(7, service.current_window));
    Window& window = service.windows[id];
    if (!window.defined) {
        // 没见过 DefineWindow 就往当前窗口写字：按"默认 32x15 单窗口"兜底，
        // 否则整条流会因为没有窗口而一个字符都出不来（比丢内容更糟）。
        window.defined = true;
        window.visible = true;
        window.row_count = 15;
        window.col_count = 32;
        window.rows.assign(15, std::string());
    }
    return &window;
}

void Cea708Decoder::WriteRaw(Service& service, const std::string& utf8) {
    if (utf8.empty())
        return;
    Window* window = ActiveWindow(service);
    if (window == nullptr)
        return;
    if (window->rows.empty()) {
        window->rows.assign(static_cast<size_t>(std::max(1, window->row_count)), std::string());
    }
    const int row = std::max(0, std::min(window->row_count - 1, window->pen_row));
    window->rows[static_cast<size_t>(row)] += utf8;
    // 一个表项按一个字符算列宽（G2 里既有单字节也有三字节，按码点计更稳）
    ++window->pen_col;
    ++stats_.char_count;
    if (window->col_count > 0 && window->pen_col >= window->col_count) {
        window->pen_col = 0;
        window->pen_row = std::min(window->row_count - 1, window->pen_row + 1);
    }
}

void Cea708Decoder::WriteChar(Service& service, std::uint32_t codepoint) {
    WriteRaw(service, Utf8Encode(codepoint));
}

void Cea708Decoder::HandleExt1(Service& service, const uint8_t* data, std::size_t size, std::size_t& index) {
    if (index >= size) {
        index = size;
        return;
    }
    const int code = data[index++];
    // EXT1 之后的字节决定进哪个扩展集（CEA-708 §8.10.2）：
    //   0x00-0x1F = C2（1 字节参数）  0x20-0x7F = G2（单字节字符）
    //   0x80-0x9F = C3（4 字节参数）  0xA0-0xFF = G3（单字节字符）
    // ❗ 以前把 0x10-0x17 当 G2、0x18-0x1F 当 G3，于是真正的 G2 字符永远出不来，
    // 而 C2 保留命令倒被当成字符写了出去。
    if (code >= 0x20 && code <= 0x7F) { // G2
        const char* text = G2Char(static_cast<uint8_t>(code));
        if (text != nullptr)
            WriteRaw(service, text);
        return;
    }
    if (code >= 0xA0) { // G3：基本是阿拉伯/西里尔，暂不映射
        if (code == 0xA0)
            WriteChar(service, ' ');
        return;
    }
    ++stats_.command_count;
    if (code <= 0x1F) { // C2
        index += std::min<std::size_t>(1, size - index);
        return;
    }
    index += std::min<std::size_t>(4, size - index); // C3
}

void Cea708Decoder::HandleC1(Service& service, const uint8_t* data, std::size_t size, std::size_t& index, int command) {
    auto need = [&](std::size_t n) { return index + n <= size; };

    // CEA-708 表 7（C1 指令集）：
    //   0x80-0x87 保留（5 字节）        0x88 DLY(1)   0x89 DLC(1)   0x8A DLM(1)
    //   0x8B DSR(1)  0x8C RST(1)        0x8D SPA(3)   0x8E SPC(3)   0x8F SPL(2)
    //   0x90 SWA(5)  0x91-0x97 保留(5)  0x98-0x9F DF0-DF7 DefineWindow(6)
    // ❗ DefineWindow 在 0x98-0x9F，不在 0x80-0x87（那里是保留区）。
    if (command >= 0x98) { // DF0-DF7 DefineWindow
        if (!need(6)) {
            index = size;
            return;
        }
        HandleDefineWindow(service, command & 0x07, data + index);
        index += 6;
        return;
    }
    switch (command) {
    case 0x88: // DLY：延时，本工具不按时间轴暂停
        ++stats_.command_count;
        index += need(1) ? 1 : 0;
        return;
    case 0x89: { // DLC：删除该窗口的文本
        ++stats_.command_count;
        if (!need(1)) {
            index = size;
            return;
        }
        const int id = data[index] & 0x07;
        Window& window = service.windows[id];
        window.rows.assign(static_cast<size_t>(std::max(1, window.row_count)), std::string());
        window.pen_row = 0;
        window.pen_col = 0;
        ++index;
        return;
    }
    case 0x8A: { // DLM：0x00 删全部窗口，0x01 删当前窗口
        ++stats_.command_count;
        if (!need(1)) {
            index = size;
            return;
        }
        const int mode = data[index];
        if (mode == 0x00) {
            for (Window& window : service.windows)
                window.Reset();
        } else {
            const int id = std::max(0, std::min(7, service.current_window));
            service.windows[id].Reset();
        }
        ++index;
        return;
    }
    case 0x8B: // DSR：延时取消，跳过
        ++stats_.command_count;
        index += need(1) ? 1 : 0;
        return;
    case 0x8C: { // RST：重置（0x00 = 全部服务）
        ++stats_.command_count;
        if (!need(1)) {
            index = size;
            return;
        }
        const int mode = data[index];
        if (mode == 0x00) {
            for (Service& each : services_) {
                for (Window& window : each.windows)
                    window.Reset();
                each.current_window = 0;
            }
        } else {
            for (Window& window : service.windows)
                window.Reset();
            service.current_window = 0;
        }
        ++index;
        return;
    }
    case 0x8D: // SPA：笔属性，3 字节
    case 0x8E: // SPC：笔颜色，3 字节
        ++stats_.command_count;
        index += need(3) ? 3 : (size - index);
        return;
    case 0x8F: { // SPL：设置笔位置（窗口 + 行/列）
        ++stats_.command_count;
        if (!need(2)) {
            index = size;
            return;
        }
        const int id = data[index] & 0x07;
        const int row = (data[index + 1] & 0xF0) >> 4;
        const int col = data[index + 1] & 0x3F;
        service.current_window = id;
        Window& window = service.windows[id];
        if (window.row_count > 0) {
            window.pen_row = std::max(0, std::min(window.row_count - 1, row));
        }
        window.pen_col = std::max(0, col);
        index += 2;
        return;
    }
    case 0x90: // SWA：窗口属性，5 字节
        ++stats_.command_count;
        index += need(5) ? 5 : (size - index);
        return;
    default:
        break;
    }
    // 0x80-0x87、0x91-0x97 是 C1 保留命令，各带 5 字节参数
    ++stats_.command_count;
    index += need(5) ? 5 : (size - index);
}

void Cea708Decoder::ExecuteCommands(Service& service, const uint8_t* data, std::size_t size, double ts_seconds) {
    (void)ts_seconds;
    std::size_t index = 0;
    while (index < size) {
        const uint8_t code = data[index++];
        if (code <= 0x1F) { // C0
            switch (code) {
            case 0x00: // NUL
            case 0x03: // ETX
                break;
            case 0x08: { // BS
                Window* window = ActiveWindow(service);
                if (window != nullptr && !window->rows.empty()) {
                    const int row = std::max(0, std::min(window->row_count - 1, window->pen_row));
                    std::string& line = window->rows[static_cast<size_t>(row)];
                    if (!line.empty()) {
                        size_t cut = line.size() - 1;
                        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
                            --cut;
                        }
                        line.erase(cut);
                    }
                    window->pen_col = std::max(0, window->pen_col - 1);
                }
                ++stats_.command_count;
                break;
            }
            case 0x0C: { // FF：清当前窗口
                Window* window = ActiveWindow(service);
                if (window != nullptr) {
                    window->rows.assign(static_cast<size_t>(std::max(1, window->row_count)), std::string());
                    window->pen_row = 0;
                    window->pen_col = 0;
                }
                ++stats_.command_count;
                break;
            }
            case 0x0D: { // CR：换行
                Window* window = ActiveWindow(service);
                if (window != nullptr) {
                    window->pen_row = std::min(window->row_count - 1, window->pen_row + 1);
                    window->pen_col = 0;
                }
                ++stats_.command_count;
                break;
            }
            case 0x0E: { // HCR：回到行首
                Window* window = ActiveWindow(service);
                if (window != nullptr)
                    window->pen_col = 0;
                ++stats_.command_count;
                break;
            }
            case 0x10: { // EXT1
                HandleExt1(service, data, size, index);
                break;
            }
            case 0x18: // P16：16 位字符，跳过
                index += std::min<std::size_t>(2, size - index);
                break;
            default:
                break;
            }
        } else if (code <= 0x7F) { // G0 = ASCII
            WriteChar(service, code);
        } else if (code <= 0x9F) { // C1
            HandleC1(service, data, size, index, code);
        } else { // G1 = Latin-1
            WriteChar(service, code);
        }
    }
}

void Cea708Decoder::SyncService(Service& service, int service_number, double ts_seconds) {
    std::string text;
    int visible_window = -1;
    for (int id = 0; id < 8; ++id) {
        const Window& window = service.windows[id];
        if (!window.defined || !window.visible)
            continue;
        const std::string window_text = window.BuildText();
        if (window_text.empty())
            continue;
        if (!text.empty())
            text += '\n';
        text += window_text;
        if (visible_window < 0)
            visible_window = id;
    }
    if (text == service.active_text)
        return;

    CloseActive(service, service_number, ts_seconds);
    if (!text.empty()) {
        service.active_text = text;
        service.active_start = ts_seconds;
        service.active_window = visible_window;
    }
}

void Cea708Decoder::CloseActive(Service& service, int service_number, double ts_seconds) {
    if (service.active_text.empty())
        return;
    model::CaptionCue cue;
    cue.start_seconds = service.active_start;
    cue.end_seconds = std::max(ts_seconds, service.active_start);
    cue.text = service.active_text;
    cue.channel = service_number;
    cue.kind_708 = true;
    cue.mode = "WIN" + std::to_string(std::max(0, service.active_window));
    cues_.push_back(std::move(cue));
    ++stats_.cues_per_service[service_number];
    service.active_text.clear();
}

void Cea708Decoder::Finish(double end_ts_seconds) {
    for (int service = 0; service < 64; ++service) {
        if (!services_[service].seen)
            continue;
        CloseActive(services_[service], service, end_ts_seconds);
    }
    if (stats_.packet_count == 0) {
        stats_.note = "cc_data 里没有完整的 DTVCC 包";
    } else if (stats_.services.empty()) {
        stats_.note = "有 DTVCC 包，但没有任何非空 service block";
    }
    if (stats_.sequence_gap_count > 0) {
        const std::string note =
            "DTVCC 包序号有 " + std::to_string(stats_.sequence_gap_count) + " 处跳变（可能丢包，字幕会缺句）";
        stats_.note = stats_.note.empty() ? note : (stats_.note + "；" + note);
    }
}

// ---------------------------------------------------------------------------
// 门面
// ---------------------------------------------------------------------------

void CeaCaptionDecoder::Reset() {
    cc608_.Reset();
    cc708_.Reset();
    result_ = model::CeaCaptionResult{};
    finished_ = false;
}

void CeaCaptionDecoder::PushPairSample(const uint8_t* data, std::size_t size, double ts_seconds) {
    if (data == nullptr)
        return;
    // MOV/MP4 的 c608 样本：每 2 字节是场 1 的一个字节对（cc_type 位不在样本里）
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        cc608_.PushPair(data[i], data[i + 1], 1, ts_seconds);
        finished_ = false;
    }
}

void CeaCaptionDecoder::PushTriplets(const uint8_t* data, std::size_t size, double ts_seconds) {
    if (data == nullptr)
        return;
    for (std::size_t i = 0; i + 2 < size; i += 3) {
        const uint8_t flags = data[i];
        const bool valid = (flags & 0x04) != 0;
        const int cc_type = flags & 0x03;
        PushTriplet(valid ? 1 : 0, static_cast<uint8_t>(cc_type), data[i + 1], data[i + 2], ts_seconds);
    }
}

void CeaCaptionDecoder::PushTriplet(uint8_t cc_valid, uint8_t cc_type, uint8_t d0, uint8_t d1, double ts_seconds) {
    // cc_valid=0 表示"该字节对无意义"（填充），按规范整对丢弃
    if (cc_valid == 0)
        return;
    // cc_type（ATSC A/53 cc_data 三元组）：0 = 608 场 1，1 = 608 场 2，
    //   2 = DTVCC **packet data**，3 = DTVCC **packet start**。
    // ❗ 这两个值以前写反了：按 2=起始、3=续包 解，标准封包的 DTVCC 流
    // 会把包头当载荷、载荷当包头，结果一条字幕都出不来。
    switch (cc_type) {
    case 0:
        cc608_.PushPair(d0, d1, 1, ts_seconds);
        break;
    case 1:
        cc608_.PushPair(d0, d1, 2, ts_seconds);
        break;
    case 2:
        cc708_.PushCcData(d0, d1, false, ts_seconds);
        break;
    case 3:
        cc708_.PushCcData(d0, d1, true, ts_seconds);
        break;
    default:
        break;
    }
    finished_ = false;
}

void CeaCaptionDecoder::MergeResults(double end_ts_seconds) {
    cc608_.Finish(end_ts_seconds);
    cc708_.Finish(end_ts_seconds);

    result_ = model::CeaCaptionResult{};
    result_.cc608 = cc608_.stats();
    result_.cc708 = cc708_.stats();
    result_.cues.reserve(cc608_.cues().size() + cc708_.cues().size());
    for (const model::CaptionCue& cue : cc608_.cues())
        result_.cues.push_back(cue);
    for (const model::CaptionCue& cue : cc708_.cues())
        result_.cues.push_back(cue);
    // 按时间排序：608 与 708 是两条独立时间线，混在一起后必须重排才能当字幕序列用
    std::stable_sort(
        result_.cues.begin(), result_.cues.end(),
        [](const model::CaptionCue& a, const model::CaptionCue& b) { return a.start_seconds < b.start_seconds; });
    result_.decoded = (result_.cc608.Any() || result_.cc708.Any());
}

void CeaCaptionDecoder::Finish(double end_ts_seconds) {
    if (finished_)
        return;
    finished_ = true;
    MergeResults(end_ts_seconds);
}

} // namespace videoeye
