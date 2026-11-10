#include "core/repair/RepairInitSegmentBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/repair/RepairBoxWriter.h"
#include "core/repair/RepairUtils.h"
#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {
namespace repair {
namespace {

// ---- RBSP 位读取（SPS 的最小解析）----
class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size) : data_(data), size_(size) {
    }

    bool ok() const {
        return ok_;
    }
    std::size_t BitsUsed() const {
        return bit_;
    }

    uint32_t U(std::size_t bits) {
        uint32_t value = 0;
        for (std::size_t i = 0; i < bits; ++i) {
            if (bit_ >= size_ * 8u) {
                ok_ = false;
                return 0;
            }
            const std::size_t byte_index = bit_ / 8u;
            const uint32_t bit_index = 7u - static_cast<uint32_t>(bit_ % 8u);
            value = (value << 1u) | ((data_[byte_index] >> bit_index) & 1u);
            ++bit_;
        }
        return value;
    }

    bool Bit() {
        return U(1) != 0;
    }

    uint32_t Ue() {
        uint32_t zeros = 0;
        while (ok_ && Bit() == false) {
            ++zeros;
            if (zeros > 31) {
                ok_ = false;
                return 0;
            }
        }
        if (!ok_)
            return 0;
        uint32_t value = 0;
        for (uint32_t i = 0; i < zeros; ++i)
            value = (value << 1u) | U(1);
        return value + ((1u << zeros) - 1u);
    }

    int32_t Se() {
        const uint32_t code = Ue();
        if (!ok_)
            return 0;
        return (code & 1u) ? static_cast<int32_t>((code + 1u) / 2u) : -static_cast<int32_t>(code / 2u);
    }

    bool Skip(std::size_t bits) {
        for (std::size_t i = 0; i < bits; ++i) {
            if (U(1) == 0 && !ok_)
                return false;
        }
        return ok_;
    }

private:
    const uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t bit_ = 0;
    bool ok_ = true;
};

std::vector<uint8_t> Rbsp(const std::vector<uint8_t>& nal, std::size_t header_size) {
    std::vector<uint8_t> out;
    if (nal.size() <= header_size)
        return out;
    out.reserve(nal.size() - header_size);
    int zeros = 0;
    for (std::size_t i = header_size; i < nal.size(); ++i) {
        const uint8_t byte = nal[i];
        if (zeros >= 2 && byte == 0x03) {
            // 去掉 emulation_prevention_three_byte；前面的两个零仍是 RBSP 内容。
            continue;
        }
        out.push_back(byte);
        zeros = (byte == 0x00) ? zeros + 1 : 0;
    }
    return out;
}

struct AvcSps {
    uint8_t profile = 0;
    uint8_t compatibility = 0;
    uint8_t level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

bool ParseAvcSps(const std::vector<uint8_t>& nal, AvcSps* sps) {
    if (nal.size() < 4)
        return false;
    sps->profile = nal[1];
    sps->compatibility = nal[2];
    sps->level = nal[3];

    const std::vector<uint8_t> rbsp = Rbsp(nal, 1);
    BitReader reader(rbsp.data(), rbsp.size());
    reader.U(8); // profile_idc
    reader.U(8); // constraint flags
    reader.U(8); // level_idc
    reader.Ue(); // seq_parameter_set_id

    uint32_t chroma_format_idc = 1;
    bool separate_colour_plane = false;
    if (sps->profile == 100 || sps->profile == 110 || sps->profile == 122 || sps->profile == 244 ||
        sps->profile == 44 || sps->profile == 83 || sps->profile == 86 || sps->profile == 118 || sps->profile == 128 ||
        sps->profile == 138 || sps->profile == 139 || sps->profile == 134 || sps->profile == 135) {
        chroma_format_idc = reader.Ue();
        if (chroma_format_idc == 3)
            separate_colour_plane = reader.Bit();
        reader.Ue();  // bit_depth_luma_minus8
        reader.Ue();  // bit_depth_chroma_minus8
        reader.Bit(); // qpprime_y_zero_transform_bypass_flag
        if (reader.Bit()) {
            // 序列缩放矩阵：当前自建链路没有证据说明 ScalingList 存在时仍可解码，
            // 但解析它需要完整语法；宁可拒绝，不写一个可能错的 avcC。
            return false;
        }
    }

    reader.Ue(); // log2_max_frame_num_minus4
    const uint32_t pic_order_cnt_type = reader.Ue();
    if (pic_order_cnt_type == 0) {
        reader.Ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        reader.Bit(); // delta_pic_order_always_zero_flag
        reader.Se();  // offset_for_non_ref_pic
        reader.Se();  // offset_for_top_to_bottom_field
        const uint32_t cycle = reader.Ue();
        if (cycle > 255)
            return false;
        for (uint32_t i = 0; i < cycle; ++i)
            reader.Se();
    }
    reader.Ue();  // max_num_ref_frames
    reader.Bit(); // gaps_in_frame_num_value_allowed_flag
    const uint32_t width_mbs = reader.Ue() + 1;
    const uint32_t height_map_units = reader.Ue() + 1;
    const bool frame_mbs_only = reader.Bit();
    if (!frame_mbs_only)
        reader.Bit(); // mb_adaptive_frame_field_flag
    reader.Bit();     // direct_8x8_inference_flag
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (reader.Bit()) {
        crop_left = reader.Ue();
        crop_right = reader.Ue();
        crop_top = reader.Ue();
        crop_bottom = reader.Ue();
    }
    if (!reader.ok())
        return false;

    const uint32_t chroma_array_type = separate_colour_plane ? 0 : chroma_format_idc;
    uint32_t crop_unit_x = 1, crop_unit_y = 1;
    if (chroma_array_type == 1 || chroma_array_type == 2) {
        crop_unit_x = 2;
        crop_unit_y = 2 * (frame_mbs_only ? 1u : 2u);
    } else if (chroma_array_type == 3) {
        crop_unit_x = 1;
        crop_unit_y = frame_mbs_only ? 1u : 2u;
    }
    sps->width = width_mbs * 16u - (crop_left + crop_right) * crop_unit_x;
    sps->height = (frame_mbs_only ? 1u : 2u) * height_map_units * 16u - (crop_top + crop_bottom) * crop_unit_y;
    return sps->width > 0 && sps->height > 0;
}

struct HevcSps {
    uint8_t profile_space = 0;
    uint8_t tier = 0;
    uint8_t profile_idc = 0;
    uint8_t level = 0;
    std::array<uint8_t, 4> compatibility{};
    std::array<uint8_t, 6> constraints{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t chroma_format = 1;
    uint8_t bit_depth_luma = 8;
    uint8_t bit_depth_chroma = 8;
};

bool ParseHevcProfileTierLevel(BitReader* reader, uint32_t max_sub_layers_minus1, HevcSps* sps) {
    sps->profile_space = static_cast<uint8_t>(reader->U(2));
    sps->tier = static_cast<uint8_t>(reader->U(1));
    sps->profile_idc = static_cast<uint8_t>(reader->U(5));
    for (std::size_t i = 0; i < sps->compatibility.size(); ++i)
        sps->compatibility[i] = static_cast<uint8_t>(reader->U(8));
    for (std::size_t i = 0; i < sps->constraints.size(); ++i)
        sps->constraints[i] = static_cast<uint8_t>(reader->U(8));
    sps->level = static_cast<uint8_t>(reader->U(8));

    std::array<bool, 7> profile_present{};
    std::array<bool, 7> level_present{};
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        profile_present[i] = reader->Bit();
        level_present[i] = reader->Bit();
    }
    if (max_sub_layers_minus1 > 0) {
        // reserved_zero_2bits[i]，i 从 max_sub_layers_minus1 到 7。
        reader->Skip(static_cast<std::size_t>(8 - max_sub_layers_minus1) * 2u);
    }
    for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        if (profile_present[i])
            reader->Skip(88); // 与 general 层同宽的 profile 字段
        if (level_present[i])
            reader->Skip(8);
    }
    return reader->ok();
}

bool ParseHevcSps(const std::vector<uint8_t>& nal, HevcSps* sps) {
    if (nal.size() < 3)
        return false;
    const std::vector<uint8_t> rbsp = Rbsp(nal, 2);
    BitReader reader(rbsp.data(), rbsp.size());
    reader.U(4); // sps_video_parameter_set_id
    const uint32_t max_sub_layers_minus1 = reader.U(3);
    if (max_sub_layers_minus1 > 7)
        return false;
    reader.U(1); // sps_temporal_id_nesting_flag
    if (!ParseHevcProfileTierLevel(&reader, max_sub_layers_minus1, sps))
        return false;
    reader.Ue(); // sps_seq_parameter_set_id
    const uint32_t chroma_format_idc = reader.Ue();
    if (chroma_format_idc == 3)
        reader.U(1); // separate_colour_plane_flag
    sps->chroma_format = static_cast<uint8_t>(std::min<uint32_t>(chroma_format_idc, 3));
    sps->width = reader.Ue();
    sps->height = reader.Ue();
    if (reader.Bit()) {
        reader.Ue(); // conf_win_left_offset
        reader.Ue(); // conf_win_right_offset
        reader.Ue(); // conf_win_top_offset
        reader.Ue(); // conf_win_bottom_offset
    }
    sps->bit_depth_luma = static_cast<uint8_t>(8 + reader.Ue());
    sps->bit_depth_chroma = static_cast<uint8_t>(8 + reader.Ue());
    return reader.ok() && sps->width > 0 && sps->height > 0;
}

struct MdatRange {
    uint64_t payload = 0;
    uint64_t payload_size = 0;
};

std::vector<MdatRange> CollectMdats(const IsobmffFile& parsed) {
    std::vector<MdatRange> mdats;
    for (const auto& box : parsed.top_level) {
        if (box.type != "mdat" || box.size < box.header_size)
            continue;
        mdats.push_back(MdatRange{box.offset + box.header_size, box.size - box.header_size});
    }
    return mdats;
}

// 取该分片里这条轨第一个样本的起始偏移。与 FragmentForensics::DataStart 保持同一语义。
uint64_t FragmentDataStart(const IsobmffFragment& fragment, const std::vector<MdatRange>& mdats) {
    if (fragment.trun_data_offset_present) {
        const uint64_t base = fragment.base_data_offset_present ? fragment.base_data_offset : fragment.offset;
        if (fragment.trun_data_offset < 0) {
            const uint64_t abs = static_cast<uint64_t>(-fragment.trun_data_offset);
            return base > abs ? base - abs : base;
        }
        return base + static_cast<uint64_t>(fragment.trun_data_offset);
    }
    for (const auto& mdat : mdats) {
        if (mdat.payload >= fragment.offset + fragment.size)
            return mdat.payload;
    }
    return std::numeric_limits<uint64_t>::max();
}

bool ReadRegion(const std::string& path, uint64_t offset, uint64_t size, std::vector<uint8_t>* out) {
    if (size == 0)
        return false;
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in)
        return false;
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in)
        return false;
    out->resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size));
    const std::streamsize got = in.gcount();
    out->resize(static_cast<std::size_t>(std::max<std::streamsize>(0, got)));
    return !out->empty();
}

struct VideoTrackModel {
    uint32_t track_id = 0;
    std::string codec; // avc1 / hvc1
    uint64_t sample_count = 0;
    uint64_t duration_ticks = 0;
    uint64_t payload_bytes = 0;
    uint32_t nal_length_size = 0;
    uint32_t timescale = 0;

    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> vps;
    AvcSps avc;
    HevcSps hevc;
    uint32_t width = 0;
    uint32_t height = 0;
};

bool ExtractParameterSets(const std::string& path, const IsobmffFile& parsed, const std::vector<MdatRange>& mdats,
                          const RepairFragmentTrackEvidence& evidence, VideoTrackModel* track, std::string* error) {
    if (evidence.nal_length_size < 1 || evidence.nal_length_size > 4) {
        *error = "轨道 " + std::to_string(evidence.track_id) + " 的 NAL 长度前缀宽度不可用";
        return false;
    }
    track->nal_length_size = evidence.nal_length_size;

    const bool hevc = evidence.bitstream == "hevc";
    for (const auto& fragment : parsed.fragments) {
        if (fragment.track_id != evidence.track_id)
            continue;
        const uint64_t start = FragmentDataStart(fragment, mdats);
        if (start == std::numeric_limits<uint64_t>::max())
            continue;
        const uint64_t cap = 4u * 1024u * 1024u;
        const uint64_t want = fragment.total_size > 0 ? std::min(fragment.total_size, cap) : cap;
        std::vector<uint8_t> data;
        if (!ReadRegion(path, start, want, &data))
            continue;

        std::size_t pos = 0;
        while (pos + evidence.nal_length_size <= data.size()) {
            uint32_t length = 0;
            for (uint32_t i = 0; i < evidence.nal_length_size; ++i)
                length = (length << 8u) | data[pos + i];
            pos += evidence.nal_length_size;
            if (length == 0 || pos + length > data.size())
                break;
            const uint8_t* nal = data.data() + pos;
            if (hevc) {
                if (length < 2)
                    break;
                const uint32_t type = (nal[0] >> 1u) & 0x3Fu;
                if (type == 32 && track->vps.empty())
                    track->vps.assign(nal, nal + length);
                else if (type == 33 && track->sps.empty())
                    track->sps.assign(nal, nal + length);
                else if (type == 34 && track->pps.empty())
                    track->pps.assign(nal, nal + length);
            } else {
                const uint32_t type = nal[0] & 0x1Fu;
                if (type == 7 && track->sps.empty())
                    track->sps.assign(nal, nal + length);
                else if (type == 8 && track->pps.empty())
                    track->pps.assign(nal, nal + length);
            }
            pos += length;
            const bool complete = track->sps.size() > 0 && track->pps.size() > 0 && (!hevc || track->vps.size() > 0);
            if (complete)
                return true;
        }
    }
    *error = hevc ? "轨道 " + std::to_string(evidence.track_id) + " 的分片里没有同时出现 VPS/SPS/PPS，无法自建 hvcC"
                  : "轨道 " + std::to_string(evidence.track_id) + " 的分片里没有同时出现 SPS/PPS，无法自建 avcC";
    return false;
}

void WriteIdentityMatrix(RepairBoxWriter* writer) {
    writer->U32(0x00010000);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0x00010000);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0);
    writer->U32(0x40000000);
}

void WriteAvcC(RepairBoxWriter* writer, const VideoTrackModel& track) {
    const std::size_t box = writer->BeginBox("avcC");
    writer->U8(1);
    writer->U8(track.avc.profile);
    writer->U8(track.avc.compatibility);
    writer->U8(track.avc.level);
    writer->U8(static_cast<uint8_t>(0xFCu | (track.nal_length_size - 1u)));
    writer->U8(0xE1); // 1 个 SPS
    writer->U16(static_cast<uint16_t>(track.sps.size()));
    writer->Raw(track.sps);
    writer->U8(1); // 1 个 PPS
    writer->U16(static_cast<uint16_t>(track.pps.size()));
    writer->Raw(track.pps);
    writer->EndBox(box);
}

void WriteHevcC(RepairBoxWriter* writer, const VideoTrackModel& track) {
    const std::size_t box = writer->BeginBox("hvcC");
    writer->U8(1);
    writer->U8(
        static_cast<uint8_t>((track.hevc.profile_space << 6u) | (track.hevc.tier << 5u) | track.hevc.profile_idc));
    writer->Raw(track.hevc.compatibility.data(), track.hevc.compatibility.size());
    writer->Raw(track.hevc.constraints.data(), track.hevc.constraints.size());
    writer->U8(track.hevc.level);
    writer->U16(0xF000); // min_spatial_segmentation_idc = 0
    writer->U8(0xFC);    // parallelismType = 0
    writer->U8(static_cast<uint8_t>(0xFCu | (track.hevc.chroma_format & 0x03u)));
    writer->U8(static_cast<uint8_t>(0xF8u | ((track.hevc.bit_depth_luma - 8u) & 0x07u)));
    writer->U8(static_cast<uint8_t>(0xF8u | ((track.hevc.bit_depth_chroma - 8u) & 0x07u)));
    writer->U16(0); // avgFrameRate
    // constantFrameRate=0, numTemporalLayers=1, temporalIdNested=1, lengthSizeMinusOne
    writer->U8(static_cast<uint8_t>((1u << 3u) | (1u << 2u) | (track.nal_length_size - 1u)));
    writer->U8(3); // VPS / SPS / PPS
    const auto write_array = [&](uint8_t type, const std::vector<uint8_t>& nal) {
        writer->U8(static_cast<uint8_t>(0x80u | type));
        writer->U16(1);
        writer->U16(static_cast<uint16_t>(nal.size()));
        writer->Raw(nal);
    };
    write_array(32, track.vps);
    write_array(33, track.sps);
    write_array(34, track.pps);
    writer->EndBox(box);
}

void WriteVideoSampleEntry(RepairBoxWriter* writer, const VideoTrackModel& track) {
    const std::size_t entry = writer->BeginBox(track.codec == "hvc1" ? "hvc1" : "avc1");
    writer->Zeros(6);
    writer->U16(1); // data_reference_index
    writer->U16(0); // pre_defined
    writer->U16(0); // reserved
    writer->U32(0);
    writer->U32(0);
    writer->U32(0);
    writer->U16(static_cast<uint16_t>(track.width));
    writer->U16(static_cast<uint16_t>(track.height));
    writer->U32(0x00480000); // 72 dpi
    writer->U32(0x00480000);
    writer->U32(0);
    writer->U16(1);      // frame_count
    writer->Zeros(32);   // compressorname
    writer->U16(0x0018); // depth
    writer->U16(0xFFFF); // pre_defined = -1
    if (track.codec == "hvc1")
        WriteHevcC(writer, track);
    else
        WriteAvcC(writer, track);
    writer->EndBox(entry);
}

void WriteTrack(RepairBoxWriter* writer, const VideoTrackModel& track, uint64_t movie_duration) {
    const std::size_t trak = writer->BeginBox("trak");

    const std::size_t tkhd = writer->BeginFullBox("tkhd", 0, 0x000007);
    writer->U32(0); // creation_time
    writer->U32(0); // modification_time
    writer->U32(track.track_id);
    writer->U32(0); // reserved
    writer->U32(static_cast<uint32_t>(movie_duration));
    writer->U32(0);
    writer->U32(0);
    writer->U16(0); // layer
    writer->U16(0); // alternate_group
    writer->U16(0); // volume
    writer->U16(0); // reserved
    WriteIdentityMatrix(writer);
    writer->U32(track.width << 16u);
    writer->U32(track.height << 16u);
    writer->EndBox(tkhd);

    const std::size_t mdia = writer->BeginBox("mdia");
    const std::size_t mdhd = writer->BeginFullBox("mdhd", 0, 0);
    writer->U32(0);
    writer->U32(0);
    writer->U32(track.timescale);
    writer->U32(static_cast<uint32_t>(track.duration_ticks));
    writer->U16(0x55C4); // 'und'
    writer->U16(0);
    writer->EndBox(mdhd);

    const std::size_t hdlr = writer->BeginFullBox("hdlr", 0, 0);
    writer->U32(0);
    writer->FourCC("vide");
    writer->U32(0);
    writer->U32(0);
    writer->U32(0);
    const char name[] = "VideoHandler";
    writer->Raw(reinterpret_cast<const uint8_t*>(name), sizeof(name) - 1);
    writer->U8(0);
    writer->EndBox(hdlr);

    const std::size_t minf = writer->BeginBox("minf");
    const std::size_t vmhd = writer->BeginFullBox("vmhd", 0, 1);
    writer->U16(0); // graphicsmode
    writer->U16(0);
    writer->U16(0);
    writer->U16(0);
    writer->EndBox(vmhd);

    const std::size_t dinf = writer->BeginBox("dinf");
    const std::size_t dref = writer->BeginFullBox("dref", 0, 0);
    writer->U32(1);
    const std::size_t url = writer->BeginFullBox("url ", 0, 1);
    writer->EndBox(url);
    writer->EndBox(dref);
    writer->EndBox(dinf);

    const std::size_t stbl = writer->BeginBox("stbl");
    const std::size_t stsd = writer->BeginFullBox("stsd", 0, 0);
    writer->U32(1);
    WriteVideoSampleEntry(writer, track);
    writer->EndBox(stsd);

    const std::size_t stts = writer->BeginFullBox("stts", 0, 0);
    writer->U32(0);
    writer->EndBox(stts);
    const std::size_t stsc = writer->BeginFullBox("stsc", 0, 0);
    writer->U32(0);
    writer->EndBox(stsc);
    const std::size_t stsz = writer->BeginFullBox("stsz", 0, 0);
    writer->U32(0); // sample_size
    writer->U32(0); // sample_count
    writer->EndBox(stsz);
    const std::size_t stco = writer->BeginFullBox("stco", 0, 0);
    writer->U32(0);
    writer->EndBox(stco);
    writer->EndBox(stbl); // stbl

    writer->EndBox(minf);
    writer->EndBox(mdia);
    writer->EndBox(trak);
}

} // namespace

RepairSynthesizedInit RepairInitSegmentBuilder::Build(const std::string& source_path, const IsobmffFile& parsed,
                                                      const RepairFragmentEvidence& forensics,
                                                      const std::atomic<bool>* cancel, const RepairRunBudget* budget) {
    RepairSynthesizedInit out;
    if (const std::string stop = BudgetStopMessage(budget, "初始化段自建阶段"); !stop.empty())
        out.error = stop;
    else if (cancel != nullptr && cancel->load())
        out.error = "已取消";
    else if (!forensics.analyzed || forensics.tracks.empty())
        out.error = "源分片取证未完成或没有轨道证据";
    else if (forensics.sidx_timescale == 0)
        out.error = "源分片缺少 sidx 时基证据；拒绝猜测 timescale（错误推断会静默改变播放速度）";
    else
        out.ok = true;
    if (!out.ok)
        return out;

    std::vector<VideoTrackModel> tracks;
    for (const auto& evidence : forensics.tracks) {
        if (const std::string stop = BudgetStopMessage(budget, "初始化段自建阶段"); !stop.empty()) {
            out.ok = false;
            out.error = stop;
            return out;
        }
        if (evidence.payload_layout != "nal_length_prefixed" ||
            (evidence.bitstream != "h264" && evidence.bitstream != "hevc")) {
            out.ok = false;
            out.error = "轨道 " + std::to_string(evidence.track_id) +
                        " 不是可自建的 H.264/H.265 视频轨（当前不支持音频/其它编码）";
            return out;
        }
        if (evidence.sample_count == 0 || evidence.declared_duration_ticks == 0) {
            out.ok = false;
            out.error = "轨道 " + std::to_string(evidence.track_id) +
                        " 未声明样本数或样本时长（依赖 trex 默认值），无法安全自建初始化段";
            return out;
        }
        if (evidence.declared_payload_bytes == 0 || evidence.nal_length_size == 0) {
            out.ok = false;
            out.error = "轨道 " + std::to_string(evidence.track_id) + " 缺少样本大小或 NAL 长度前缀证据";
            return out;
        }

        VideoTrackModel track;
        track.track_id = evidence.track_id;
        track.codec = evidence.bitstream == "hevc" ? "hvc1" : "avc1";
        track.sample_count = evidence.sample_count;
        track.duration_ticks = evidence.declared_duration_ticks;
        track.payload_bytes = evidence.declared_payload_bytes;
        track.timescale = forensics.sidx_timescale;

        const double track_seconds = static_cast<double>(track.duration_ticks) / track.timescale;
        if (forensics.estimated_duration_seconds > 0.0) {
            const double tolerance = std::max(0.25, forensics.estimated_duration_seconds * 0.10);
            if (std::fabs(track_seconds - forensics.estimated_duration_seconds) > tolerance) {
                out.ok = false;
                out.error = "轨道 " + std::to_string(track.track_id) + " 的声明时长 " + FormatSeconds(track_seconds) +
                            " 与 sidx 估算 " + FormatSeconds(forensics.estimated_duration_seconds) +
                            " 不一致；不能借用该 timescale";
                return out;
            }
        }

        std::string error;
        if (!ExtractParameterSets(source_path, parsed, CollectMdats(parsed), evidence, &track, &error)) {
            out.ok = false;
            out.error = error;
            return out;
        }
        if (evidence.bitstream == "hevc") {
            if (!ParseHevcSps(track.sps, &track.hevc)) {
                out.ok = false;
                out.error = "轨道 " + std::to_string(track.track_id) + " 的 SPS 无法解析";
                return out;
            }
            track.width = track.hevc.width;
            track.height = track.hevc.height;
        } else {
            if (!ParseAvcSps(track.sps, &track.avc)) {
                out.ok = false;
                out.error = "轨道 " + std::to_string(track.track_id) + " 的 SPS 无法解析";
                return out;
            }
            track.width = track.avc.width;
            track.height = track.avc.height;
        }
        tracks.push_back(std::move(track));
    }

    std::sort(tracks.begin(), tracks.end(),
              [](const VideoTrackModel& a, const VideoTrackModel& b) { return a.track_id < b.track_id; });

    const uint32_t movie_timescale = forensics.sidx_timescale;
    double duration_seconds = forensics.estimated_duration_seconds;
    if (duration_seconds <= 0.0) {
        for (const auto& track : tracks)
            duration_seconds = std::max(duration_seconds, static_cast<double>(track.duration_ticks) / track.timescale);
    }
    const uint64_t movie_duration = static_cast<uint64_t>(std::llround(duration_seconds * movie_timescale));
    if (movie_duration == 0) {
        out.ok = false;
        out.error = "无法得到正数时长";
        return out;
    }

    RepairBoxWriter writer;
    const std::size_t ftyp = writer.BeginBox("ftyp");
    writer.FourCC("isom");
    writer.U32(512);
    writer.FourCC("isom");
    writer.FourCC("iso6");
    writer.FourCC("mp41");
    writer.FourCC("dash");
    writer.EndBox(ftyp);

    const std::size_t moov = writer.BeginBox("moov");
    const std::size_t mvhd = writer.BeginFullBox("mvhd", 0, 0);
    writer.U32(0);
    writer.U32(0);
    writer.U32(movie_timescale);
    writer.U32(static_cast<uint32_t>(movie_duration));
    writer.U32(0x00010000); // rate
    writer.U16(0x0100);     // volume
    writer.U16(0);          // reserved
    writer.U32(0);
    writer.U32(0);
    WriteIdentityMatrix(&writer);
    writer.U32(0);
    writer.U32(0);
    writer.U32(0);
    writer.U32(0);
    writer.U32(0);
    writer.U32(0);
    writer.U32(tracks.empty() ? 1 : tracks.back().track_id + 1); // next_track_ID
    writer.EndBox(mvhd);

    for (const auto& track : tracks)
        WriteTrack(&writer, track, movie_duration);

    const std::size_t mvex = writer.BeginBox("mvex");
    const std::size_t mehd = writer.BeginFullBox("mehd", 0, 0);
    writer.U32(static_cast<uint32_t>(movie_duration));
    writer.EndBox(mehd);
    for (const auto& track : tracks) {
        const uint64_t avg_duration =
            track.sample_count > 0 ? std::max<uint64_t>(1, track.duration_ticks / track.sample_count) : 0;
        const std::size_t trex = writer.BeginFullBox("trex", 0, 0);
        writer.U32(track.track_id);
        writer.U32(1); // default_sample_description_index
        writer.U32(static_cast<uint32_t>(avg_duration));
        writer.U32(0); // default_sample_size：分片已逐样本声明大小
        writer.U32(0); // default_sample_flags
        writer.EndBox(trex);
    }
    writer.EndBox(mvex);
    writer.EndBox(moov);

    out.bytes = writer.TakeBytes();
    out.movie_timescale = movie_timescale;
    out.estimated_duration_seconds = duration_seconds;
    out.timescale_basis = "sidx timescale=" + std::to_string(movie_timescale);
    for (const auto& track : tracks) {
        RepairReferenceTrack item;
        item.track_id = track.track_id;
        item.timescale = track.timescale;
        item.codec = track.codec;
        out.tracks.push_back(item);
        out.track_ids.push_back(track.track_id);
        if (out.codec_summary.empty())
            out.codec_summary = "轨道 " + std::to_string(track.track_id) + " video(" + track.codec + ")";
        else
            out.codec_summary += ", 轨道 " + std::to_string(track.track_id) + " video(" + track.codec + ")";
    }
    out.limitations.push_back("初始化段由 in-band 参数集自建，未经同源参考段交叉验证");
    out.limitations.push_back("timescale 来自 sidx；若源 sidx 本身被改写，产物播放速度会随之改变");
    out.limitations.push_back("仅重建视频轨；音频轨需要 ADTS/AudioSpecificConfig 才能自建，当前版本会直接拒绝");
    out.limitations.push_back("未重建 edts/ctts 等编辑信息，B 帧显示顺序由解码器与 ctts 缺省值决定");
    return out;
}

} // namespace repair
} // namespace videoeye
