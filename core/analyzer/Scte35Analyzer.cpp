#include "core/analyzer/Scte35Analyzer.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace videoeye {
namespace analyzer {
namespace {

constexpr uint8_t kScte35TableId = 0xFC;
constexpr uint32_t kCueiIdentifier = 0x43554549;   // "CUEI"
constexpr uint16_t kSegmentationDescriptorTag = 0x02;
constexpr int64_t kPtsModulus = INT64_C(1) << 33;
constexpr double kScte35Timebase = 90000.0;

// 简易大端比特读取器。越界读会被记住（overflow_），返回 0，
// 调用方靠 overflow() 判断"这段 section 被截断"。
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    void Skip(int bits) { pos_ += static_cast<size_t>(bits); }
    void ByteAlign() { pos_ = (pos_ + 7) & ~static_cast<size_t>(7); }

    uint64_t Read(int bits) {
        uint64_t value = 0;
        for (int i = 0; i < bits; ++i) {
            const size_t bit = pos_++;
            if (bit >= size_ * 8) {
                overflow_ = true;
                value <<= 1;
                continue;
            }
            const uint8_t byte = data_[bit >> 3];
            const uint8_t bit_value = static_cast<uint8_t>((byte >> (7 - (bit & 7))) & 1u);
            value = (value << 1) | bit_value;
        }
        return value;
    }

    bool ReadBytes(size_t count, std::vector<uint8_t>& out) {
        ByteAlign();
        if (pos_ / 8 + count > size_) {
            overflow_ = true;
            return false;
        }
        out.assign(data_ + pos_ / 8, data_ + pos_ / 8 + count);
        pos_ += count * 8;
        return true;
    }

    bool overflow() const { return overflow_; }
    size_t BitPos() const { return pos_; }
    size_t BytePos() const { return pos_ / 8; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool overflow_ = false;
};

// 33 bit PTS + pts_adjustment，取模 2^33 后折算成秒
double Scte35PtsToSeconds(uint64_t pts_time, int64_t pts_adjustment) {
    const int64_t sum = (static_cast<int64_t>(pts_time) + pts_adjustment) % kPtsModulus;
    const int64_t wrapped = (sum < 0) ? (sum + kPtsModulus) : sum;
    return static_cast<double>(wrapped) / kScte35Timebase;
}

std::string HexSummary(const std::vector<uint8_t>& bytes, size_t max_bytes = 8) {
    std::string out;
    const size_t n = (bytes.size() < max_bytes) ? bytes.size() : max_bytes;
    char buf[8] = {0};
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
        out += buf;
    }
    if (bytes.size() > n) out += "...";
    return out;
}

// UPID 的可读摘要：纯打印字符直接显示，否则给十六进制
std::string UpidSummary(uint8_t type, const std::vector<uint8_t>& bytes) {
    bool printable = !bytes.empty();
    for (uint8_t b : bytes) {
        if (b < 0x20 || b > 0x7E) {
            printable = false;
            break;
        }
    }
    std::string out = "type=0x";
    char buf[16] = {0};
    std::snprintf(buf, sizeof(buf), "%02X", type);
    out += buf;
    out += printable ? (" 值=" + std::string(bytes.begin(), bytes.end()))
                     : (" 值=0x" + HexSummary(bytes, 12));
    return out;
}

// splice_time(): time_specified_flag(1) + reserved(6) + [pts_time(33)]
bool ReadSpliceTime(BitReader& reader, int64_t pts_adjustment, double& seconds_out,
                    bool& has_time_out) {
    const uint64_t time_specified = reader.Read(1);
    reader.Skip(6);
    has_time_out = (time_specified != 0);
    if (!has_time_out) return true;
    const uint64_t pts_time = reader.Read(33);
    if (reader.overflow()) return false;
    seconds_out = Scte35PtsToSeconds(pts_time, pts_adjustment);
    return true;
}

// break_duration(): auto_return(1) + reserved(6) + duration(33)
bool ReadBreakDuration(BitReader& reader, bool& auto_return, double& duration_seconds,
                       bool& has_duration) {
    auto_return = reader.Read(1) != 0;
    reader.Skip(6);
    const uint64_t duration = reader.Read(33);
    if (reader.overflow()) return false;
    duration_seconds = static_cast<double>(duration) / kScte35Timebase;
    has_duration = true;
    return true;
}

bool ParseSegmentationDescriptor(BitReader& reader, size_t end_byte,
                                 model::Scte35Segmentation& seg) {
    seg.present = true;
    seg.segmentation_event_id = static_cast<uint32_t>(reader.Read(32));
    seg.has_event_id = true;
    seg.cancel = reader.Read(1) != 0;
    reader.Skip(7);
    if (seg.cancel) {
        // cancel 之后没有后续字段，直接跳到描述符末尾
        reader.ByteAlign();
        if (reader.BytePos() < end_byte) reader.Skip(static_cast<int>((end_byte - reader.BytePos()) * 8));
        return true;
    }

    const bool program_segmentation = reader.Read(1) != 0;
    const bool duration_flag = reader.Read(1) != 0;
    const bool delivery_not_restricted = reader.Read(1) != 0;
    if (!delivery_not_restricted) {
        reader.Skip(5);   // web_delivery / no_regional_blackout / archive_allowed / device_restrictions
    } else {
        reader.Skip(5);
    }

    if (!program_segmentation) {
        const uint64_t component_count = reader.Read(8);
        for (uint64_t i = 0; i < component_count; ++i) {
            reader.Skip(8);   // component_tag
            reader.Skip(7);
            reader.Read(33);  // pts_offset
            if (reader.overflow()) return false;
        }
    }

    if (duration_flag) {
        const uint64_t duration = reader.Read(40);
        if (reader.overflow()) return false;
        seg.duration_seconds = static_cast<double>(duration) / kScte35Timebase;
        seg.has_duration = true;
    }

    if (reader.BytePos() + 2 > end_byte) return true;   // 后续字段被截断，能拿到的先记下
    const uint8_t upid_type = static_cast<uint8_t>(reader.Read(8));
    const uint8_t upid_length = static_cast<uint8_t>(reader.Read(8));
    std::vector<uint8_t> upid;
    if (upid_length > 0) {
        if (!reader.ReadBytes(upid_length, upid)) return true;
        seg.upid_summary = UpidSummary(upid_type, upid);
    }

    if (reader.BytePos() + 3 > end_byte) return true;
    seg.type_id = static_cast<uint32_t>(reader.Read(8));
    seg.segment_num = static_cast<uint8_t>(reader.Read(8));
    seg.segments_expected = static_cast<uint8_t>(reader.Read(8));

    const char* name = model::Scte35SegmentationTypeName(seg.type_id);
    if (name != nullptr) {
        seg.type_name = name;
    } else {
        char buf[32] = {0};
        std::snprintf(buf, sizeof(buf), "未收录类型 (0x%02X)", seg.type_id);
        seg.type_name = buf;
    }
    return true;
}

bool ParseCommand(BitReader& reader, uint8_t command_type, int command_length,
                  model::Scte35Cue& cue) {
    const size_t command_end = reader.BytePos() + static_cast<size_t>(command_length);

    switch (command_type) {
        case 0x00: {   // splice_null
            cue.command = model::Scte35Command::SpliceNull;
            break;
        }
        case 0x04: {   // splice_schedule：可能有多条事件，这里只取第一条的关键字段
            cue.command = model::Scte35Command::SpliceSchedule;
            const uint64_t event_count = reader.Read(8);
            for (uint64_t i = 0; i < event_count && i < 1; ++i) {
                cue.event_id = static_cast<uint32_t>(reader.Read(32));
                cue.has_event_id = true;
                cue.cancel_indicator = reader.Read(1) != 0;
                reader.Skip(7);
                if (!cue.cancel_indicator) {
                    cue.out_of_network = reader.Read(1) != 0;
                    cue.program_splice = reader.Read(1) != 0;
                    cue.duration_flag = reader.Read(1) != 0;
                    reader.Skip(5);
                    if (cue.program_splice) {
                        const uint64_t utc = reader.Read(32);   // utc_splice_time（秒，GPS 纪元）
                        cue.has_splice_time = true;
                        cue.splice_time_seconds = static_cast<double>(utc);
                    }
                    if (!cue.program_splice) {
                        const uint64_t component_count = reader.Read(8);
                        for (uint64_t c = 0; c < component_count; ++c) {
                            reader.Skip(8);
                            reader.Skip(7);
                            reader.Read(33);
                        }
                    }
                    if (cue.duration_flag) {
                        reader.Read(1);
                        reader.Skip(6);
                        const uint64_t duration = reader.Read(33);
                        cue.break_duration_seconds = static_cast<double>(duration) / kScte35Timebase;
                        cue.has_duration = true;
                    }
                    cue.unique_program_id = static_cast<uint16_t>(reader.Read(16));
                    cue.has_unique_program_id = true;
                    cue.avail_num = static_cast<uint8_t>(reader.Read(8));
                    cue.avails_expected = static_cast<uint8_t>(reader.Read(8));
                    cue.has_avail = true;
                }
            }
            break;
        }
        case 0x05: {   // splice_insert
            cue.command = model::Scte35Command::SpliceInsert;
            cue.event_id = static_cast<uint32_t>(reader.Read(32));
            cue.has_event_id = true;
            cue.cancel_indicator = reader.Read(1) != 0;
            reader.Skip(7);
            if (cue.cancel_indicator) break;

            cue.out_of_network = reader.Read(1) != 0;
            cue.program_splice = reader.Read(1) != 0;
            cue.duration_flag = reader.Read(1) != 0;
            cue.splice_immediate = reader.Read(1) != 0;
            reader.Skip(4);

            if (cue.program_splice && !cue.splice_immediate) {
                if (!ReadSpliceTime(reader, cue.pts_adjustment, cue.splice_time_seconds,
                                    cue.has_splice_time)) {
                    cue.parse_error = "splice_time 读取越界";
                    return false;
                }
            }
            if (!cue.program_splice) {
                const uint64_t component_count = reader.Read(8);
                for (uint64_t c = 0; c < component_count; ++c) {
                    reader.Skip(8);      // component_tag
                    double component_time = 0.0;
                    bool has_time = false;
                    if (!ReadSpliceTime(reader, cue.pts_adjustment, component_time, has_time)) break;
                    if (has_time && !cue.has_splice_time) {
                        cue.splice_time_seconds = component_time;
                        cue.has_splice_time = true;
                    }
                }
            }
            if (cue.duration_flag) {
                if (!ReadBreakDuration(reader, cue.auto_return, cue.break_duration_seconds,
                                       cue.has_duration)) {
                    cue.parse_error = "break_duration 读取越界";
                    return false;
                }
            }
            cue.unique_program_id = static_cast<uint16_t>(reader.Read(16));
            cue.has_unique_program_id = true;
            cue.avail_num = static_cast<uint8_t>(reader.Read(8));
            cue.avails_expected = static_cast<uint8_t>(reader.Read(8));
            cue.has_avail = true;
            break;
        }
        case 0x06: {   // time_signal
            cue.command = model::Scte35Command::TimeSignal;
            if (!ReadSpliceTime(reader, cue.pts_adjustment, cue.splice_time_seconds,
                                cue.has_splice_time)) {
                cue.parse_error = "splice_time 读取越界";
                return false;
            }
            break;
        }
        case 0x07: {   // bandwidth_reservation（无字段）
            cue.command = model::Scte35Command::BandwidthReservation;
            break;
        }
        case 0xFF: {   // private_command
            cue.command = model::Scte35Command::PrivateCommand;
            const uint32_t identifier = static_cast<uint32_t>(reader.Read(32));
            (void)identifier;   // 通常是 "CUEI"，仅登记命令类型
            break;
        }
        default:
            cue.parse_error = "未知的 splice_command_type";
            return false;
    }

    if (reader.BytePos() > command_end && command_length > 0) {
        // 略微越界通常是"命令长度与字段长度不完全吻合"，不算致命，只做提示
        if (cue.parse_error.empty()) cue.parse_error = "命令体长度与字段长度不完全吻合";
    }
    // 对齐到命令体末尾，后面才是 descriptor_loop_length
    reader.ByteAlign();
    if (command_length > 0 && reader.BytePos() < command_end) {
        reader.Skip(static_cast<int>((command_end - reader.BytePos()) * 8));
    }
    return true;
}

}  // namespace

uint32_t Scte35Analyzer::Crc32Mpeg2(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (static_cast<uint32_t>(data[i]) << 24);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x04C11DB7u;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

bool Scte35Analyzer::ParseSection(const uint8_t* data, size_t size, model::Scte35Cue& cue,
                                  const Scte35Options& options) {
    cue = model::Scte35Cue{};
    if (data == nullptr || size < 8) {
        cue.parse_error = "载荷过短（不足 8 字节）";
        return false;
    }

    size_t start = 0;
    if (data[0] != kScte35TableId) {
        if (!options.scan_for_section_start) {
            cue.parse_error = "table_id 不是 0xFC";
            return false;
        }
        const size_t limit = (size < 16) ? size : 16;
        bool found = false;
        for (size_t i = 1; i + 3 < limit; ++i) {
            if (data[i] == kScte35TableId) {
                start = i;
                found = true;
                break;
            }
        }
        if (!found) {
            cue.parse_error = "未找到 table_id 0xFC";
            return false;
        }
    }

    BitReader reader(data + start, size - start);

    cue.table_id = static_cast<uint8_t>(reader.Read(8));
    reader.Skip(1);   // section_syntax_indicator
    reader.Skip(1);   // private_indicator
    reader.Skip(2);   // sap_type
    const uint64_t section_length = reader.Read(12);
    if (section_length < 11) {
        cue.parse_error = "section_length 过小";
        return false;
    }
    // section_length 从 protocol_version 起算（含末尾 CRC）
    const size_t section_end = 3 + static_cast<size_t>(section_length);
    if (section_end > size - start) {
        cue.parse_error = "section_length 超出可用字节数";
        return false;
    }

    cue.protocol_version = static_cast<uint8_t>(reader.Read(8));
    const uint64_t encrypted = reader.Read(1);
    reader.Skip(6);   // encryption_algorithm
    const uint64_t pts_adjustment = reader.Read(33);
    cue.pts_adjustment = static_cast<int64_t>(pts_adjustment);
    reader.Skip(8);   // cw_index
    cue.tier = static_cast<uint16_t>(reader.Read(12));
    const uint64_t command_length = reader.Read(12);
    const uint64_t command_type = reader.Read(8);
    if (reader.overflow()) {
        cue.parse_error = "section 头部越界";
        return false;
    }

    if (encrypted != 0) {
        cue.parse_error = "加密的 SCTE-35 暂不支持解析";
        return false;
    }

    if (!ParseCommand(reader, static_cast<uint8_t>(command_type),
                      static_cast<int>(command_length), cue)) {
        if (cue.parse_error.empty()) cue.parse_error = "命令体解析失败";
        return false;
    }

    // descriptor_loop_length
    const uint64_t descriptor_loop_length = reader.Read(16);
    const size_t descriptor_end = reader.BytePos() + static_cast<size_t>(descriptor_loop_length);
    while (reader.BytePos() + 5 <= descriptor_end && !reader.overflow()) {
        const uint16_t tag = static_cast<uint16_t>(reader.Read(16));
        const uint64_t descriptor_length = reader.Read(8);
        const size_t descriptor_body_end = reader.BytePos() + static_cast<size_t>(descriptor_length);
        if (descriptor_body_end > descriptor_end) {
            cue.parse_error = "描述符长度越界";
            break;
        }
        const uint32_t identifier = static_cast<uint32_t>(reader.Read(32));
        ++cue.descriptor_count;

        if (tag == kSegmentationDescriptorTag && identifier == kCueiIdentifier) {
            model::Scte35Segmentation seg;
            if (!ParseSegmentationDescriptor(reader, descriptor_body_end, seg)) {
                cue.parse_error = "segmentation_descriptor 解析越界";
            }
            // duration 以 segmentation_duration 为准，缺了再用 break_duration
            cue.segmentation.push_back(seg);
        } else {
            // 其它描述符（avail / DTMF / time）暂不解析，按长度跳过
            reader.ByteAlign();
            if (reader.BytePos() < descriptor_body_end) {
                reader.Skip(static_cast<int>((descriptor_body_end - reader.BytePos()) * 8));
            }
        }
        reader.ByteAlign();
        if (reader.BytePos() < descriptor_body_end) {
            reader.Skip(static_cast<int>((descriptor_body_end - reader.BytePos()) * 8));
        }
    }

    if (options.verify_crc && section_end >= 4) {
        const uint32_t crc_expected = static_cast<uint32_t>(
            (static_cast<uint32_t>(data[start + section_end - 4]) << 24) |
            (static_cast<uint32_t>(data[start + section_end - 3]) << 16) |
            (static_cast<uint32_t>(data[start + section_end - 2]) << 8) |
            static_cast<uint32_t>(data[start + section_end - 1]));
        const uint32_t crc_actual = Crc32Mpeg2(data + start, section_end - 4);
        cue.crc_checked = true;
        cue.crc_valid = (crc_actual == crc_expected);
    }

    cue.valid = true;
    return true;
}

}  // namespace analyzer
}  // namespace videoeye
