#include "core/analysis/codec/ParameterSetTracker.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/analysis/codec/Av1BitstreamParser.h"
#include "core/analysis/codec/H264BitstreamParser.h"
#include "core/analysis/codec/HevcBitstreamParser.h"
#include "core/media/codec/ExtradataParser.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace videoeye {
namespace {

constexpr int kH264Sps = 7;
constexpr int kH264Pps = 8;

constexpr int kHevcVps = 32;
constexpr int kHevcSps = 33;
constexpr int kHevcPps = 34;

bool IsH264Slice(int nal_type) {
    // 1 = 非 IDR 片, 2/3/4 = 分区 A/B/C, 5 = IDR 片
    return nal_type == 1 || nal_type == 2 || nal_type == 3 || nal_type == 4 || nal_type == 5;
}

bool IsHevcSlice(int nal_type) {
    return nal_type >= 0 && nal_type <= 31;
}

bool IsHevcIrap(int nal_type) {
    return nal_type >= 16 && nal_type <= 23;
}

// 极简 RBSP 位读取（只需要 ue(v) 与定长位）
class RbspBits {
public:
    RbspBits(const uint8_t* data, size_t size) : data_(data), size_(size) {
    }

    bool ReadBit(uint32_t& value) {
        if (bit_pos_ >= size_ * 8)
            return false;
        const size_t byte = bit_pos_ / 8;
        value = (data_[byte] >> (7 - (bit_pos_ % 8))) & 1u;
        ++bit_pos_;
        return true;
    }

    bool ReadBits(int bits, uint32_t& value) {
        uint32_t result = 0;
        for (int i = 0; i < bits; ++i) {
            uint32_t bit = 0;
            if (!ReadBit(bit))
                return false;
            result = (result << 1) | bit;
        }
        value = result;
        return true;
    }

    // ue(v)：前导零个数 + 1 位 1，再读前导零个位
    bool ReadUe(uint32_t& value) {
        int leading = 0;
        while (true) {
            uint32_t bit = 0;
            if (!ReadBit(bit))
                return false;
            if (bit == 1)
                break;
            ++leading;
            if (leading > 31) // 畸形流：ue(v) 前导零不可能是 31 个以上
                return false;
        }
        uint32_t rest = 0;
        if (leading > 0 && !ReadBits(leading, rest))
            return false;
        value = (1u << leading) - 1u + rest;
        return true;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t bit_pos_ = 0;
};

std::string ChromaText(int chroma_format_idc) {
    switch (chroma_format_idc) {
    case 0:
        return "4:0:0";
    case 1:
        return "4:2:0";
    case 2:
        return "4:2:2";
    case 3:
        return "4:4:4";
    default:
        return std::string();
    }
}

// "1920x1080 / 8 bit / 4:2:0"；缺项就少写，不填默认值
std::string ConfigText(int width, int height, int bit_depth, int chroma_format_idc) {
    std::string out;
    if (width > 0 && height > 0) {
        out += std::to_string(width) + "x" + std::to_string(height);
    }
    if (bit_depth > 0) {
        if (!out.empty())
            out += " / ";
        out += std::to_string(bit_depth) + " bit";
    }
    const std::string chroma = ChromaText(chroma_format_idc);
    if (!chroma.empty()) {
        if (!out.empty())
            out += " / ";
        out += chroma;
    }
    return out;
}

// FNV-1a 64：只用来做"内容是否变化"的等价判断，不需要密码学强度
std::string DigestOf(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

} // namespace

void ParameterSetTracker::Reset(int codec_id, int stream_index, const Options& options) {
    codec_id_ = codec_id;
    stream_index_ = stream_index;
    options_ = options;
    framing_ = Framing::Auto;
    prefix_bytes_ = 0;
    framing_decided_ = false;
    finished_ = false;
    disabled_reason_.clear();

    timeline_ = model::ParameterTimeline{};
    timeline_.stream_index = stream_index;

    switch (static_cast<AVCodecID>(codec_id)) {
    case AV_CODEC_ID_H264:
        enabled_ = true;
        timeline_.codec_name = "h264";
        break;
    case AV_CODEC_ID_HEVC:
        enabled_ = true;
        timeline_.codec_name = "hevc";
        break;
    case AV_CODEC_ID_AV1:
        enabled_ = true;
        timeline_.codec_name = "av1";
        break;
    default:
        enabled_ = false;
        disabled_reason_ = "该编码未实现参数变化追踪（当前覆盖 H.264 / HEVC / AV1）";
        break;
    }
}

void ParameterSetTracker::SetFraming(Framing framing, int prefix_bytes) {
    framing_ = framing;
    prefix_bytes_ = prefix_bytes;
    framing_decided_ = (framing != Framing::Auto) || prefix_bytes > 0;
}

bool ParameterSetTracker::DetectLengthPrefixed(const uint8_t* data, size_t size, int& prefix_bytes) const {
    // 先看有没有起始码：有就是 Annex B，不要再猜
    for (size_t i = 0; i + 3 <= size && i < 64; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (data[i + 2] == 0 && i + 3 < size && data[i + 3] == 1)))
            return false;
    }
    static const int kWidths[3] = {4, 2, 1};
    for (int width : kWidths) {
        if (ExtradataParser::LooksLikeLengthPrefixed(data, size, width)) {
            prefix_bytes = width;
            return true;
        }
    }
    return false;
}

void ParameterSetTracker::Feed(const uint8_t* data, size_t size, double pts_seconds, int64_t packet_index,
                               int64_t file_offset) {
    if (!enabled_ || data == nullptr || size == 0)
        return;
    if (options_.max_packets > 0 && timeline_.packets_scanned >= options_.max_packets) {
        timeline_.truncated = true;
        return;
    }
    ++timeline_.packets_scanned;
    timeline_.analyzed = true;

    switch (static_cast<AVCodecID>(codec_id_)) {
    case AV_CODEC_ID_H264:
        FeedH264(data, size, pts_seconds, packet_index, file_offset);
        break;
    case AV_CODEC_ID_HEVC:
        FeedHevc(data, size, pts_seconds, packet_index, file_offset);
        break;
    case AV_CODEC_ID_AV1:
        FeedAv1(data, size, pts_seconds, packet_index, file_offset);
        break;
    default:
        break;
    }
}

void ParameterSetTracker::FeedH264(const uint8_t* data, size_t size, double pts, int64_t packet_index,
                                   int64_t file_offset) {
    std::vector<NalUnit> nals;
    bool length_prefixed = false;
    int width = prefix_bytes_;
    if (framing_ == Framing::LengthPrefixed) {
        length_prefixed = true;
    } else if (framing_ == Framing::Auto && !framing_decided_) {
        int detected = 0;
        if (DetectLengthPrefixed(data, size, detected)) {
            length_prefixed = true;
            width = detected;
            prefix_bytes_ = detected;
            framing_decided_ = true;
        } else {
            framing_decided_ = true;
        }
    } else if (framing_ == Framing::Auto) {
        length_prefixed = (prefix_bytes_ > 0);
        width = prefix_bytes_;
    }

    if (length_prefixed) {
        if (width <= 0)
            return;
        nals = ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::H264, width, nullptr);
    } else {
        nals = ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::H264);
    }

    for (const NalUnit& nal : nals) {
        const int type = static_cast<int>(nal.type);
        if (type == kH264Sps) {
            RegisterH264Sps(nal, pts, packet_index, file_offset);
        } else if (type == kH264Pps) {
            RegisterH264Pps(nal, pts, packet_index, file_offset);
        } else if (IsH264Slice(type)) {
            // slice_header(): first_mb_in_slice ue(v), slice_type ue(v), pic_parameter_set_id ue(v)
            RbspBits bits(nal.data.data(), nal.data.size());
            uint32_t first_mb = 0;
            uint32_t slice_type = 0;
            uint32_t pps_id = 0;
            if (!bits.ReadUe(first_mb) || !bits.ReadUe(slice_type) || !bits.ReadUe(pps_id))
                continue;
            // 只把 first_mb_in_slice == 0 的 slice 记为"一帧"，否则一帧多 slice 会把帧数放大
            AddReference(model::ParameterSetKind::Pps, static_cast<int>(pps_id), first_mb == 0, pts, packet_index);
        }
    }
}

void ParameterSetTracker::FeedHevc(const uint8_t* data, size_t size, double pts, int64_t packet_index,
                                   int64_t file_offset) {
    std::vector<NalUnit> nals;
    bool length_prefixed = false;
    int width = prefix_bytes_;
    if (framing_ == Framing::LengthPrefixed) {
        length_prefixed = true;
    } else if (framing_ == Framing::Auto && !framing_decided_) {
        int detected = 0;
        if (DetectLengthPrefixed(data, size, detected)) {
            length_prefixed = true;
            width = detected;
            prefix_bytes_ = detected;
            framing_decided_ = true;
        } else {
            framing_decided_ = true;
        }
    } else if (framing_ == Framing::Auto) {
        length_prefixed = (prefix_bytes_ > 0);
        width = prefix_bytes_;
    }

    if (length_prefixed) {
        if (width <= 0)
            return;
        nals = ExtradataParser::ExtractLengthPrefixedNalUnits(data, size, NalSyntax::Hevc, width, nullptr);
    } else {
        nals = ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::Hevc);
    }

    for (const NalUnit& nal : nals) {
        const int type = static_cast<int>(nal.type);
        if (type == kHevcVps) {
            RegisterHevcVps(nal, pts, packet_index, file_offset);
        } else if (type == kHevcSps) {
            RegisterHevcSps(nal, pts, packet_index, file_offset);
        } else if (type == kHevcPps) {
            RegisterHevcPps(nal, pts, packet_index, file_offset);
        } else if (IsHevcSlice(type)) {
            // slice_segment_header(): first_slice_segment_in_pic_flag(1)
            //   IRAP 时还有 no_output_of_prior_pics_flag(1)
            //   然后 slice_pic_parameter_set_id ue(v)
            RbspBits bits(nal.data.data(), nal.data.size());
            uint32_t first_slice = 0;
            if (!bits.ReadBit(first_slice))
                continue;
            if (IsHevcIrap(type)) {
                uint32_t no_output = 0;
                if (!bits.ReadBit(no_output))
                    continue;
            }
            uint32_t pps_id = 0;
            if (!bits.ReadUe(pps_id))
                continue;
            AddReference(model::ParameterSetKind::Pps, static_cast<int>(pps_id), first_slice == 1, pts, packet_index);
        }
    }
}

// ---- 单个参数集 NAL 的登记（扫包与扫 extradata 共用）----

bool ParameterSetTracker::RegisterH264Sps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset) {
    const model::H264SpsInfo sps = H264BitstreamParser::ParseFromNalUnit(nal);
    if (!sps.present)
        return false;
    const std::string summary = ConfigText(sps.width(), sps.height(), sps.BitDepthLuma(), sps.chroma_format_idc);
    RegisterSet(model::ParameterSetKind::Sps, sps.seq_parameter_set_id, summary, nal.data.data(), nal.data.size(), pts,
                packet_index, file_offset);
    return true;
}

bool ParameterSetTracker::RegisterH264Pps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset) {
    RbspBits bits(nal.data.data(), nal.data.size());
    uint32_t pps_id = 0;
    uint32_t sps_id = 0;
    if (!bits.ReadUe(pps_id) || !bits.ReadUe(sps_id))
        return false;
    RegisterSet(model::ParameterSetKind::Pps, static_cast<int>(pps_id), "引用 SPS(id=" + std::to_string(sps_id) + ")",
                nal.data.data(), nal.data.size(), pts, packet_index, file_offset);
    return true;
}

bool ParameterSetTracker::RegisterHevcVps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset) {
    RbspBits bits(nal.data.data(), nal.data.size());
    uint32_t vps_id = 0;
    if (!bits.ReadBits(4, vps_id))
        return false;
    RegisterSet(model::ParameterSetKind::Vps, static_cast<int>(vps_id), "VPS id=" + std::to_string(vps_id),
                nal.data.data(), nal.data.size(), pts, packet_index, file_offset);
    return true;
}

bool ParameterSetTracker::RegisterHevcSps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset) {
    const model::HevcSpsInfo sps = HevcBitstreamParser::ParseSpfFromNalUnit(nal);
    if (!sps.present)
        return false;
    const std::string summary = ConfigText(sps.width(), sps.height(), sps.BitDepthLuma(), sps.chroma_format_idc);
    RegisterSet(model::ParameterSetKind::Sps, sps.sps_seq_parameter_set_id, summary, nal.data.data(), nal.data.size(),
                pts, packet_index, file_offset);
    return true;
}

bool ParameterSetTracker::RegisterHevcPps(const NalUnit& nal, double pts, int64_t packet_index, int64_t file_offset) {
    RbspBits bits(nal.data.data(), nal.data.size());
    uint32_t pps_id = 0;
    uint32_t sps_id = 0;
    if (!bits.ReadUe(pps_id) || !bits.ReadUe(sps_id))
        return false;
    RegisterSet(model::ParameterSetKind::Pps, static_cast<int>(pps_id), "引用 SPS(id=" + std::to_string(sps_id) + ")",
                nal.data.data(), nal.data.size(), pts, packet_index, file_offset);
    return true;
}

bool ParameterSetTracker::FeedExtradata(const uint8_t* data, size_t size) {
    if (!enabled_ || data == nullptr || size == 0)
        return false;
    const ExtradataResult parsed = ExtradataParser::Parse(data, size);
    if (!parsed.valid)
        return false;

    // extradata 既没有 PTS，也不在媒体数据区里 —— 两个定位信息都按"未知"记。
    // 传 0 会被报告读成"文件开头"，传 -1 才是"未知"。
    constexpr double kNoPts = -1.0;
    constexpr int64_t kNoPosition = -1;

    int registered = 0;
    if (codec_id_ == AV_CODEC_ID_H264) {
        for (const NalUnit& nal : parsed.nal_units) {
            const int type = static_cast<int>(nal.type);
            if (type == kH264Sps) {
                if (RegisterH264Sps(nal, kNoPts, kNoPosition, kNoPosition))
                    ++registered;
            } else if (type == kH264Pps) {
                if (RegisterH264Pps(nal, kNoPts, kNoPosition, kNoPosition))
                    ++registered;
            }
        }
    } else if (codec_id_ == AV_CODEC_ID_HEVC) {
        for (const NalUnit& nal : parsed.nal_units) {
            const int type = static_cast<int>(nal.type);
            if (type == kHevcVps) {
                if (RegisterHevcVps(nal, kNoPts, kNoPosition, kNoPosition))
                    ++registered;
            } else if (type == kHevcSps) {
                if (RegisterHevcSps(nal, kNoPts, kNoPosition, kNoPosition))
                    ++registered;
            } else if (type == kHevcPps) {
                if (RegisterHevcPps(nal, kNoPts, kNoPosition, kNoPosition))
                    ++registered;
            }
        }
    } else if (codec_id_ == AV_CODEC_ID_AV1) {
        for (const ObuUnit& obu : parsed.obu_units) {
            if (!Av1BitstreamParser::IsSequenceHeaderObu(obu))
                continue;
            const model::Av1SequenceHeaderInfo sh = Av1BitstreamParser::ParseFromObuUnit(obu);
            if (!sh.present)
                continue;
            std::string summary = std::to_string(sh.FrameWidth()) + "x" + std::to_string(sh.FrameHeight());
            if (sh.BitDepth() > 0)
                summary += " / " + std::to_string(sh.BitDepth()) + " bit";
            const std::string chroma = sh.ChromaFormatString();
            if (!chroma.empty())
                summary += " / " + chroma;
            RegisterSet(model::ParameterSetKind::Av1SequenceHeader, 0, summary, obu.data.data(), obu.data.size(),
                        kNoPts, kNoPosition, kNoPosition);
            ++registered;
        }
    }
    return registered > 0;
}

void ParameterSetTracker::FeedAv1(const uint8_t* data, size_t size, double pts, int64_t packet_index,
                                  int64_t file_offset) {
    const std::vector<ObuUnit> obus = ExtradataParser::ExtractObuUnits(data, size);
    for (const ObuUnit& obu : obus) {
        if (!Av1BitstreamParser::IsSequenceHeaderObu(obu))
            continue;
        const model::Av1SequenceHeaderInfo sh = Av1BitstreamParser::ParseFromObuUnit(obu);
        if (!sh.present)
            continue;
        // AV1 的序列头没有 id（一份流只有一个生效），统一记 id=0；
        // 内容变了就是"被替换"，语义与 H.264 的 SPS 替换一致。
        // 色度格式用 ChromaFormatString() 直接取，别自己从 subsampling 位推。
        std::string summary = std::to_string(sh.FrameWidth()) + "x" + std::to_string(sh.FrameHeight());
        if (sh.BitDepth() > 0)
            summary += " / " + std::to_string(sh.BitDepth()) + " bit";
        const std::string chroma = sh.ChromaFormatString();
        if (!chroma.empty())
            summary += " / " + chroma;
        RegisterSet(model::ParameterSetKind::Av1SequenceHeader, 0, summary, obu.data.data(), obu.data.size(), pts,
                    packet_index, file_offset);
    }
}

model::ParameterSetEntry* ParameterSetTracker::FindEntry(model::ParameterSetKind kind, int id) {
    for (model::ParameterSetEntry& entry : timeline_.parameter_sets) {
        if (entry.kind == kind && entry.id == id)
            return &entry;
    }
    return nullptr;
}

void ParameterSetTracker::RegisterSet(model::ParameterSetKind kind, int id, const std::string& summary,
                                      const uint8_t* payload, size_t size, double pts, int64_t packet_index,
                                      int64_t file_offset) {
    ++timeline_.parameter_set_units;
    const std::string digest = DigestOf(payload, size);

    model::ParameterSetEntry* existing = FindEntry(kind, id);
    if (existing == nullptr) {
        if (static_cast<int>(timeline_.parameter_sets.size()) >= options_.max_parameter_sets) {
            timeline_.truncated = true;
            if (timeline_.notes.empty())
                timeline_.notes.push_back("参数集数量超过上限 " + std::to_string(options_.max_parameter_sets) +
                                          "，后续参数集不再登记");
            return;
        }
        model::ParameterSetEntry entry;
        entry.kind = kind;
        entry.id = id;
        entry.digest = digest;
        entry.summary = summary;
        entry.first_packet_index = packet_index;
        entry.first_file_offset = file_offset;
        entry.occurrences = 1;
        if (pts >= 0.0) {
            entry.has_time = true;
            entry.first_time_seconds = pts;
        }
        timeline_.parameter_sets.push_back(entry);

        // 新增了一份 id —— 若它与"第一份同类参数集"的配置不同，这就是一次配置切换。
        // （同一 id 的重复发送不会走到这里，所以不会误报。）
        if (kind == model::ParameterSetKind::Sps || kind == model::ParameterSetKind::Av1SequenceHeader) {
            std::string baseline;
            int baseline_id = -1;
            for (const model::ParameterSetEntry& other : timeline_.parameter_sets) {
                if (&other == &timeline_.parameter_sets.back())
                    break;
                if (other.kind == kind && !other.summary.empty()) {
                    baseline = other.summary;
                    baseline_id = other.id;
                    break;
                }
            }
            if (!baseline.empty() && !summary.empty() && baseline != summary) {
                model::ParameterChangeEvent event;
                event.stream_index = stream_index_;
                event.kind = kind;
                event.parameter_set_id = id;
                event.has_time = (pts >= 0.0);
                event.time_seconds = (pts >= 0.0 ? pts : 0.0);
                event.packet_index = packet_index;
                event.file_offset = file_offset;
                event.source = std::string(model::ParameterSetKindName(kind)) + "(id=" + std::to_string(id) + ")";
                event.old_config = baseline;
                event.new_config = summary;
                event.changes = "新增了不同配置的参数集（原 id=" + std::to_string(baseline_id) + "）：" + baseline +
                                " -> " + summary;
                event.replaced_same_id = false;
                if (static_cast<int>(timeline_.changes.size()) < options_.max_changes) {
                    timeline_.changes.push_back(std::move(event));
                } else {
                    timeline_.truncated = true;
                }
            }
        }
        return;
    }

    ++existing->occurrences;
    if (existing->digest == digest)
        return; // 重复发送同一份参数集（IDR 前的常规行为），不是变化

    // 同一 (类型, id) 的内容被替换 —— 最危险的一类：
    // 之前解出的帧和之后的帧用的不是同一套配置，解码器与播放器行为都会异常。
    model::ParameterChangeEvent event;
    event.stream_index = stream_index_;
    event.kind = kind;
    event.parameter_set_id = id;
    event.has_time = (pts >= 0.0);
    event.time_seconds = (pts >= 0.0 ? pts : 0.0);
    event.packet_index = packet_index;
    event.file_offset = file_offset;
    event.source = std::string(model::ParameterSetKindName(kind)) + "(id=" + std::to_string(id) + ")";
    event.old_config = existing->summary;
    event.new_config = summary;
    if (existing->summary.empty() || summary.empty() || existing->summary == summary) {
        event.changes = std::string(model::ParameterSetKindName(kind)) + " id=" + std::to_string(id) +
                        " 的字节内容变了（可见参数未变，可能是 profile/level/帧率等其它字段）";
    } else {
        event.changes = existing->summary + " -> " + summary;
    }
    event.replaced_same_id = true;

    existing->summary = summary;
    existing->digest = digest;
    ++existing->replacements;

    if (static_cast<int>(timeline_.changes.size()) < options_.max_changes) {
        timeline_.changes.push_back(std::move(event));
    } else {
        timeline_.truncated = true;
    }
}

void ParameterSetTracker::AddReference(model::ParameterSetKind kind, int id, bool first_slice_of_picture, double pts,
                                       int64_t packet_index) {
    model::ParameterReferenceStat* stat = nullptr;
    for (model::ParameterReferenceStat& existing : timeline_.references) {
        if (existing.kind == kind && existing.id == id) {
            stat = &existing;
            break;
        }
    }
    if (stat == nullptr) {
        model::ParameterReferenceStat fresh;
        fresh.kind = kind;
        fresh.id = id;
        timeline_.references.push_back(fresh);
        stat = &timeline_.references.back();
    }
    ++stat->slice_count;
    if (first_slice_of_picture)
        ++stat->frame_count;
    if (stat->first_reference_packet < 0 || packet_index < stat->first_reference_packet) {
        stat->first_reference_packet = packet_index;
        if (pts >= 0.0) {
            stat->has_time = true;
            stat->first_reference_time = pts;
        }
    }
}

void ParameterSetTracker::Finish() {
    if (finished_)
        return;
    finished_ = true;
    if (!enabled_)
        return;

    timeline_.missing_reference_count = 0;
    int late_transmits = 0;
    for (model::ParameterReferenceStat& stat : timeline_.references) {
        const model::ParameterSetEntry* entry = FindEntry(stat.kind, stat.id);
        stat.transmitted = (entry != nullptr);
        if (!stat.transmitted) {
            // 引用了从未传输的参数集：解码器拿不到它，这一帧不可能正确解出。
            timeline_.missing_reference_count += (stat.frame_count > 0 ? stat.frame_count : stat.slice_count);
            if (timeline_.notes.size() < 8) {
                timeline_.notes.push_back("有 " + std::to_string(stat.frame_count) + " 帧引用了从未传输的 " +
                                          model::ParameterSetKindName(stat.kind) + "(id=" + std::to_string(stat.id) +
                                          ")");
            }
            continue;
        }
        if (entry->first_packet_index >= 0 && stat.first_reference_packet >= 0 &&
            stat.first_reference_packet < entry->first_packet_index) {
            stat.referenced_before_transmit = true;
            ++late_transmits;
        }
    }
    if (late_transmits > 0) {
        timeline_.notes.push_back("有 " + std::to_string(late_transmits) +
                                  " 份参数集第一次被引用时尚未传输（传输晚于引用，起播可能失败）");
    }
    if (timeline_.parameter_sets.empty() && timeline_.packets_scanned > 0) {
        timeline_.notes.push_back("扫描了 " + std::to_string(timeline_.packets_scanned) +
                                  " 个包，但没有解出任何参数集（可能参数集全在 extradata/初始化段里）");
    }
}

} // namespace videoeye
