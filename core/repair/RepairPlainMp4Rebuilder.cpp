#include "core/repair/RepairPlainMp4Rebuilder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "core/domain/io/Utf8Path.h"
#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairBoxWriter.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairMdatSelfCheck.h"
#include "core/repair/RepairSampleScan.h"
#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {
namespace repair {
namespace {

constexpr uint32_t kMaxAuBytes = 256u * 1024u * 1024u; // 单帧上限，防止坏长度字段把扫描拖死
// 语法解析只需 NAL 前缀：ADTS 头 ≤ 9 字节，slice 首宏块 / SPS / PPS 都在前 64 KiB 内。
// 视频帧载荷本身不进内存，AU 边界靠长度前缀推进。
constexpr std::size_t kParsedNalBytes = 64u * 1024u;

bool Cancelled(const std::atomic<bool>* cancel) {
    return cancel != nullptr && cancel->load();
}

// 共享预算优先：引擎传下来的 deadline 覆盖「取消 + 超时」，独立调用才退回 cancel。
bool Stopped(const std::atomic<bool>* cancel, const RepairRunBudget* budget, std::string* error) {
    if (budget != nullptr) {
        if (!budget->Checkpoint())
            return false;
        *error = budget->Canceled() ? "已取消" : "超出本次修复预算";
        return true;
    }
    if (Cancelled(cancel)) {
        *error = "已取消";
        return true;
    }
    return false;
}

bool ReadAt(std::ifstream& in, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in)
        return false;
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return in.gcount() == static_cast<std::streamsize>(size);
}

struct PlainBox {
    std::string type;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t header = 0;
};

struct MdatInfo {
    std::uint64_t payload = 0;
    std::uint64_t payload_size = 0;
    bool found = false;
    std::uint32_t count = 0;
};

struct PlainSample {
    std::uint64_t source_offset = 0;
    std::uint32_t size = 0;
    bool sync = true;
};

struct ScanResult {
    bool ok = false;
    bool video = false;
    bool audio = false;
    std::string error;
    std::string kind;
    std::vector<PlainSample> samples;
    std::uint64_t total_bytes = 0;
    std::uint32_t timescale = 0;
    std::uint32_t sample_delta = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t profile = 0;
    std::uint8_t compatibility = 0;
    std::uint8_t level = 0;
    std::vector<std::uint8_t> sps;
    std::vector<std::vector<std::uint8_t>> pps;
    std::uint16_t sample_rate = 0;
    std::uint8_t channels = 0;
    std::uint8_t aac_object_type = 0;
    std::uint8_t audio_sfi = 0;
    std::vector<std::string> limitations;
    std::string codec_summary;
    // 样本层事实源：samples 只是写 moov 需要的 (offset,size,sync) 视图，
    // 样本数 / 逐轨 / 逐区间统计一律由这个账本求和。
    RepairSampleLedger ledger;
};

bool ScanTopLevel(const std::string& path, std::vector<PlainBox>* boxes, MdatInfo* mdat, std::uint64_t* file_size,
                  std::string* error) {
    std::ifstream in(ToNativePath(path), std::ios::binary);
    if (!in) {
        *error = "无法读取源文件: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::uint64_t size = static_cast<std::uint64_t>(in.tellg());
    *file_size = size;

    std::uint64_t offset = 0;
    while (offset + 8 <= size) {
        std::uint8_t header[16] = {};
        if (!ReadAt(in, offset, header, 8))
            break;
        std::uint64_t box_size = (static_cast<std::uint64_t>(header[0]) << 24) |
                                 (static_cast<std::uint64_t>(header[1]) << 16) |
                                 (static_cast<std::uint64_t>(header[2]) << 8) | static_cast<std::uint64_t>(header[3]);
        std::string type(reinterpret_cast<const char*>(header + 4), 4);
        std::uint64_t header_size = 8;
        if (box_size == 1) {
            if (!ReadAt(in, offset + 8, header, 8))
                break;
            box_size = 0;
            for (int i = 0; i < 8; ++i)
                box_size = (box_size << 8) | header[i];
            header_size = 16;
        } else if (box_size == 0) {
            box_size = size - offset;
        }
        if (box_size < header_size || box_size > size - offset) {
            *error = "顶层盒 " + type + " 大小越界（offset=" + std::to_string(offset) + "）";
            return false;
        }
        boxes->push_back(PlainBox{type, offset, box_size, header_size});
        if (type == "mdat") {
            ++mdat->count;
            if (!mdat->found) {
                mdat->found = true;
                mdat->payload = offset + header_size;
                mdat->payload_size = box_size - header_size;
            }
        }
        offset += box_size;
    }
    return true;
}

// ---- RBSP 位读取 ----
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {
    }
    bool ok() const {
        return ok_;
    }
    std::uint32_t U(std::size_t bits) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < bits; ++i) {
            if (bit_ >= size_ * 8u) {
                ok_ = false;
                return 0;
            }
            const std::size_t byte_index = bit_ / 8u;
            const std::uint32_t bit_index = 7u - static_cast<std::uint32_t>(bit_ % 8u);
            value = (value << 1u) | ((data_[byte_index] >> bit_index) & 1u);
            ++bit_;
        }
        return value;
    }
    bool Bit() {
        return U(1) != 0;
    }
    std::uint32_t Ue() {
        std::uint32_t zeros = 0;
        while (ok_ && !Bit()) {
            ++zeros;
            if (zeros > 31) {
                ok_ = false;
                return 0;
            }
        }
        if (!ok_)
            return 0;
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < zeros; ++i)
            value = (value << 1u) | U(1);
        return value + ((1u << zeros) - 1u);
    }
    void Skip(std::size_t bits) {
        for (std::size_t i = 0; i < bits && ok_; ++i)
            U(1);
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t bit_ = 0;
    bool ok_ = true;
};

std::vector<std::uint8_t> Rbsp(const std::uint8_t* nal, std::size_t size, std::size_t header_size) {
    std::vector<std::uint8_t> out;
    if (size <= header_size)
        return out;
    out.reserve(size - header_size);
    int zeros = 0;
    for (std::size_t i = header_size; i < size; ++i) {
        const std::uint8_t byte = nal[i];
        if (zeros >= 2 && byte == 0x03)
            continue;
        out.push_back(byte);
        zeros = (byte == 0x00) ? zeros + 1 : 0;
    }
    return out;
}

// SPS 最小解析：到 VUI timing_info 为止（后续字段对本重建无意义）。
bool ParsePlainSps(const std::vector<std::uint8_t>& nal, ScanResult* scan, std::string* error) {
    if (nal.size() < 4) {
        *error = "SPS 长度不足";
        return false;
    }
    scan->profile = nal[1];
    scan->compatibility = nal[2];
    scan->level = nal[3];
    scan->sps = nal;

    const std::vector<std::uint8_t> rbsp = Rbsp(nal.data(), nal.size(), 1);
    BitReader reader(rbsp.data(), rbsp.size());
    reader.U(8);
    reader.U(8);
    reader.U(8);
    reader.Ue(); // seq_parameter_set_id
    const std::uint8_t profile = scan->profile;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        reader.Ue();  // chroma_format_idc
        reader.Ue();  // bit_depth_luma_minus8
        reader.Ue();  // bit_depth_chroma_minus8
        reader.Bit(); // qpprime_y_zero_transform_bypass_flag
        if (reader.Bit()) {
            *error = "SPS 含 scaling matrix，当前重建不解析该语法（避免写出错误的 avcC）";
            return false;
        }
    }
    reader.Ue(); // log2_max_frame_num_minus4
    const std::uint32_t poc_type = reader.Ue();
    if (poc_type == 0) {
        reader.Ue();
    } else if (poc_type == 1) {
        reader.Bit();
        reader.Ue(); // offset_for_non_ref_pic (se)
        reader.Ue(); // offset_for_top_to_bottom_field (se)
        const std::uint32_t cycle = reader.Ue();
        if (cycle > 255) {
            *error = "SPS 的 poc cycle 异常";
            return false;
        }
        for (std::uint32_t i = 0; i < cycle; ++i)
            reader.Ue();
    }
    reader.Ue();  // max_num_ref_frames
    reader.Bit(); // gaps_in_frame_num_value_allowed_flag
    reader.Ue();  // pic_width_in_mbs_minus1
    reader.Ue();  // pic_height_in_map_units_minus1
    const bool frame_mbs_only = reader.Bit();
    if (!frame_mbs_only)
        reader.Bit(); // mb_adaptive_frame_field_flag
    reader.Bit();     // direct_8x8_inference_flag
    if (reader.Bit()) {
        reader.Ue();
        reader.Ue();
        reader.Ue();
        reader.Ue();
    }
    if (!reader.ok()) {
        *error = "SPS 解析中断";
        return false;
    }
    if (!reader.Bit()) {
        *error = "SPS 没有 VUI，缺少帧率证据（num_units_in_tick/time_scale），拒绝推断时基";
        return false;
    }

    // VUI
    if (reader.Bit()) { // aspect_ratio_info_present_flag
        const std::uint32_t idc = reader.U(8);
        if (idc == 255) {
            reader.U(16);
            reader.U(16);
        }
    }
    if (reader.Bit()) // overscan_info_present_flag
        reader.Bit();
    if (reader.Bit()) { // video_signal_type_present_flag
        reader.U(3);
        reader.Bit();
        if (reader.Bit()) {
            reader.U(8);
            reader.U(8);
            reader.U(8);
        }
    }
    if (reader.Bit()) { // chroma_loc_info_present_flag
        reader.Ue();
        reader.Ue();
    }
    if (!reader.Bit()) {
        *error = "SPS VUI 没有 timing_info，缺少帧率证据，拒绝推断时基";
        return false;
    }
    const std::uint32_t num_units_in_tick = reader.U(32);
    const std::uint32_t time_scale = reader.U(32);
    reader.Bit(); // fixed_frame_rate_flag
    if (!reader.ok() || num_units_in_tick == 0 || time_scale == 0) {
        *error = "SPS VUI 的 timing_info 无效";
        return false;
    }
    scan->timescale = time_scale;
    scan->sample_delta = 2u * num_units_in_tick;
    if (scan->sample_delta == 0) {
        *error = "SPS VUI 推出的样本时长为 0";
        return false;
    }
    return true;
}

bool SliceFirstMbIsZero(const std::uint8_t* nal, std::size_t size) {
    if (size < 2)
        return false;
    BitReader reader(nal + 1, size - 1);
    const std::uint32_t first_mb = reader.Ue();
    return reader.ok() && first_mb == 0;
}

// ---- ADTS ----
const std::uint32_t kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000,  7350};

bool ParseAdtsHeader(const std::uint8_t* data, std::size_t available, std::uint32_t* frame_length,
                     std::uint8_t* header_length, std::uint8_t* object_type, std::uint8_t* sample_rate_index,
                     std::uint8_t* channels) {
    if (available < 7)
        return false;
    if ((data[0] & 0xFF) != 0xFF || (data[1] & 0xF0) != 0xF0)
        return false;
    const bool protection_absent = (data[1] & 0x01) != 0;
    *header_length = protection_absent ? 7 : 9;
    if (available < *header_length)
        return false;
    *object_type = static_cast<std::uint8_t>(((data[2] >> 6) & 0x03) + 1);
    *sample_rate_index = static_cast<std::uint8_t>((data[2] >> 2) & 0x0F);
    *channels = static_cast<std::uint8_t>(((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03));
    *frame_length = (static_cast<std::uint32_t>(data[3] & 0x03) << 11) | (static_cast<std::uint32_t>(data[4]) << 3) |
                    ((data[5] >> 5) & 0x07);
    if (*sample_rate_index >= 13 || *channels == 0)
        return false;
    if (*frame_length < *header_length + 1)
        return false;
    return true;
}

ScanResult ScanAdts(SampleByteSource& src, const ScanWindow& window, const std::atomic<bool>* cancel,
                    const RepairRunBudget* budget) {
    ScanResult result;
    result.audio = true;
    result.kind = "aac";

    // 载荷按有界分块读取：帧头只需 16 字节窗口，帧体不进内存。
    // 旧的「整个 mdat 读入 + 1 GiB 上限」在这里被彻底去掉。
    std::uint64_t pos = 0;
    std::uint8_t header_len = 0;
    std::uint8_t object_type = 0;
    std::uint8_t sfi = 0;
    std::uint8_t channels = 0;
    std::uint32_t first_rate = 0;
    while (pos < src.Size()) {
        // 旧判据 pos/1MiB != (pos+1)/1MiB 只在 pos 恰好停在块边界前一字节时成立；
        // pos 按整帧 / 整 NAL 长度跳跃，实测 2 MiB、每帧 8 字节的 ADTS 样本
        // 预置 cancel=true 仍跑完全程。改成逐迭代检查点（最严格的迭代式判据），
        // 并同时吃共享预算，而不是只在外层重建结束后才补查一次。
        if (Stopped(cancel, budget, &result.error))
            return result;
        std::uint32_t frame_len = 0;
        std::array<std::uint8_t, 16> header{};
        const std::size_t header_bytes =
            static_cast<std::size_t>(std::min<std::uint64_t>(header.size(), src.Size() - pos));
        if (!src.Read(pos, header.data(), header_bytes)) {
            result.error = src.Error().empty() ? std::string("读取 ADTS 帧头失败") : src.Error();
            result.samples.clear();
            result.audio = false;
            return result;
        }
        if (!ParseAdtsHeader(header.data(), header_bytes, &frame_len, &header_len, &object_type, &sfi, &channels)) {
            result.error = "载荷不是连续的 ADTS AAC 帧（offset=" + std::to_string(pos) + "）";
            result.samples.clear();
            result.audio = false;
            return result;
        }
        const std::uint32_t rate = kSampleRates[sfi];
        if (first_rate == 0)
            first_rate = rate;
        if (rate != first_rate) {
            result.error = "ADTS 帧采样率不恒定，无法用单一 mdhd timescale 重建";
            result.samples.clear();
            result.audio = false;
            return result;
        }
        if (frame_len > src.Size() - pos) {
            result.error = "ADTS 帧长度越界（offset=" + std::to_string(pos) + "）";
            result.samples.clear();
            result.audio = false;
            return result;
        }
        const std::uint64_t sample_offset = window.file_offset + pos + header_len;
        const std::uint32_t sample_size = frame_len - header_len;
        result.samples.push_back(PlainSample{sample_offset, sample_size, true});

        model::RecoveredSample sample;
        sample.source_offset = sample_offset;
        sample.size = sample_size;
        sample.track_id = window.track_id;
        sample.stream_index = window.stream_index;
        sample.segment_index = window.segment_index;
        sample.config_id = window.config_id;
        sample.decode_ordinal = result.ledger.Total();
        sample.display_ordinal = result.ledger.Total(); // AAC 无重排，解码序即显示序
        sample.is_rap = true;
        sample.rap_evidence = "adts_sync";
        const double step = first_rate > 0 ? 1024.0 / static_cast<double>(first_rate) : 0.0;
        sample.dts_seconds = static_cast<double>(result.ledger.Total()) * step;
        sample.pts_seconds = sample.dts_seconds;
        sample.duration_seconds = step;
        sample.timestamp_evidence = model::SampleTimestampEvidence::Inferred;
        sample.inference_basis = "ADTS 恒定 1024 采样/帧 + 帧头采样率";
        sample.confidence = model::SampleConfidence::Inferred;
        result.ledger.Add(sample);
        pos += frame_len;
    }
    if (result.ledger.Total() < 2) {
        result.error = "ADTS 帧数量过少，无法确认是完整音频载荷";
        result.samples.clear();
        result.audio = false;
        return result;
    }

    result.ok = true;
    result.timescale = first_rate;
    result.sample_rate = static_cast<std::uint16_t>(first_rate);
    result.sample_delta = 1024;
    result.channels = channels;
    result.aac_object_type = object_type;
    result.audio_sfi = sfi;
    result.total_bytes = result.ledger.TotalBytes();
    std::ostringstream summary;
    summary << "ADTS AAC（" << first_rate << " Hz / " << static_cast<int>(channels) << " 声道，AOT "
            << static_cast<int>(object_type) << "）";
    result.codec_summary = summary.str();
    for (const auto& limitation : result.ledger.Limitations())
        result.limitations.push_back(limitation);
    result.limitations.push_back("mdat 载荷按有界分块扫描，帧体不整段读入内存");
    result.limitations.push_back("AAC 样本按 ADTS 帧边界切分；CRC 校验位所在帧头已剥离");
    result.limitations.push_back("未重建 edts/ctts，时间轴为恒定 1024 采样/帧");
    return result;
}

ScanResult ScanAvcc(SampleByteSource& src, const ScanWindow& window, const std::atomic<bool>* cancel,
                    const RepairRunBudget* budget) {
    ScanResult result;
    result.video = true;
    result.kind = "h264";

    // 每个 NAL 只读语法解析所需的前缀；视频帧载荷本身不进内存，
    // AU 边界靠 length-prefix 推进，所以窗口多大都不会把整段 mdat 读进来。
    std::uint64_t pos = 0;
    std::uint64_t au_start = 0;
    std::uint32_t au_size = 0;
    bool au_has_vcl = false;
    bool au_sync = false;
    bool has_sps = false;
    bool has_pps = false;

    auto close_au = [&](std::uint64_t boundary) {
        if (au_size > 0) {
            const std::uint64_t sample_offset = window.file_offset + au_start;
            result.samples.push_back(PlainSample{sample_offset, au_size, au_sync});

            model::RecoveredSample sample;
            sample.source_offset = sample_offset;
            sample.size = au_size;
            sample.track_id = window.track_id;
            sample.stream_index = window.stream_index;
            sample.segment_index = window.segment_index;
            sample.config_id = window.config_id;
            sample.decode_ordinal = result.ledger.Total();
            // 无 ctts / POC 证据，显示顺序保持 unknown，绝不填成解码序。
            sample.is_rap = au_sync;
            if (au_sync)
                sample.rap_evidence = "idr_nal";
            const double step = result.timescale > 0
                                    ? static_cast<double>(result.sample_delta) / static_cast<double>(result.timescale)
                                    : 0.0;
            sample.dts_seconds = static_cast<double>(result.ledger.Total()) * step;
            sample.duration_seconds = step;
            sample.timestamp_evidence = model::SampleTimestampEvidence::Inferred;
            sample.inference_basis = "SPS VUI 帧率 + 恒定样本时长，未重建 ctts";
            sample.confidence = model::SampleConfidence::Inferred;
            result.ledger.Add(sample);
            result.total_bytes = result.ledger.TotalBytes();
        }
        au_size = 0;
        au_has_vcl = false;
        au_sync = false;
        au_start = boundary;
    };

    while (pos < src.Size()) {
        // 旧判据 pos/1MiB != (pos+1)/1MiB 只在 pos 恰好停在块边界前一字节时成立；
        // pos 按整帧 / 整 NAL 长度跳跃，实测 2 MiB、每帧 8 字节的 ADTS 样本
        // 预置 cancel=true 仍跑完全程。改成逐迭代检查点（最严格的迭代式判据），
        // 并同时吃共享预算，而不是只在外层重建结束后才补查一次。
        if (Stopped(cancel, budget, &result.error))
            return result;
        if (src.Size() - pos < 4) {
            result.error = "载荷尾部长度前缀不完整（offset=" + std::to_string(pos) + "）";
            result.video = false;
            result.samples.clear();
            return result;
        }
        std::array<std::uint8_t, 4> length_bytes{};
        if (!src.Read(pos, length_bytes.data(), length_bytes.size())) {
            result.error = src.Error().empty() ? std::string("读取 NAL 长度前缀失败") : src.Error();
            result.video = false;
            result.samples.clear();
            return result;
        }
        const std::uint32_t nal_size =
            (static_cast<std::uint32_t>(length_bytes[0]) << 24) | (static_cast<std::uint32_t>(length_bytes[1]) << 16) |
            (static_cast<std::uint32_t>(length_bytes[2]) << 8) | static_cast<std::uint32_t>(length_bytes[3]);
        if (nal_size == 0 || nal_size > src.Size() - pos - 4 || nal_size > kMaxAuBytes) {
            result.error =
                "NAL 长度非法（offset=" + std::to_string(pos) + "，length=" + std::to_string(nal_size) + "）";
            result.video = false;
            result.samples.clear();
            return result;
        }
        // 只取语法前缀：slice 的 first_mb_in_slice、SPS / PPS 都在这里面。
        const std::size_t parse_len =
            static_cast<std::size_t>(std::min<std::uint32_t>(nal_size, static_cast<std::uint32_t>(kParsedNalBytes)));
        std::vector<std::uint8_t> nal_buffer(parse_len);
        if (!src.Read(pos + 4, nal_buffer.data(), parse_len)) {
            result.error = src.Error().empty() ? std::string("读取 NAL 语法前缀失败") : src.Error();
            result.video = false;
            result.samples.clear();
            return result;
        }
        const std::uint8_t* nal = nal_buffer.data();
        const std::uint8_t nal_type = nal[0] & 0x1F;

        if (nal_type == 9) { // AUD：只有当前 AU 已有 VCL 时才切分
            if (au_has_vcl)
                close_au(pos);
        } else if (nal_type == 1 || nal_type == 5) {
            const bool first_mb_zero = SliceFirstMbIsZero(nal, parse_len);
            if (first_mb_zero && au_has_vcl)
                close_au(pos);
            au_has_vcl = true;
            if (nal_type == 5)
                au_sync = true;
        } else {
            if (au_has_vcl)
                close_au(pos);
            if (nal_type == 7 && !has_sps) {
                if (parse_len < nal_size) {
                    result.error = "SPS 超过有界解析上限，拒绝用截断参数集重建 avcC";
                    result.video = false;
                    result.samples.clear();
                    return result;
                }
                std::vector<std::uint8_t> sps(nal_buffer.begin(), nal_buffer.end());
                std::string sps_error;
                if (!ParsePlainSps(sps, &result, &sps_error)) {
                    result.error = sps_error;
                    result.video = false;
                    result.samples.clear();
                    return result;
                }
                has_sps = true;
            } else if (nal_type == 8) {
                if (parse_len < nal_size) {
                    result.error = "PPS 超过有界解析上限，拒绝用截断参数集重建 avcC";
                    result.video = false;
                    result.samples.clear();
                    return result;
                }
                std::vector<std::uint8_t> pps(nal_buffer.begin(), nal_buffer.end());
                bool duplicate = false;
                for (const auto& existing : result.pps) {
                    if (existing == pps)
                        duplicate = true;
                }
                if (!duplicate && result.pps.size() < 8)
                    result.pps.push_back(std::move(pps));
                has_pps = true;
            }
        }
        au_size += nal_size + 4;
        pos += nal_size + 4;
    }
    close_au(pos);

    if (!has_sps || !has_pps) {
        result.error = "载荷中没有找到完整的 SPS/PPS，无法生成 avcC";
        result.video = false;
        result.samples.clear();
        return result;
    }
    if (result.samples.empty()) {
        result.error = "没有切出任何视频访问单元";
        result.video = false;
        return result;
    }

    // 分辨率从 SPS 里拿：扫描阶段只解析了 VUI，宽高需要单独再解一次。
    result.ok = true;
    std::ostringstream summary;
    summary << "H.264（profile " << static_cast<int>(result.profile) << " / level " << static_cast<int>(result.level)
            << "，时基 " << result.timescale << " ticks，样本时长 " << result.sample_delta << "）";
    result.codec_summary = summary.str();
    for (const auto& limitation : result.ledger.Limitations())
        result.limitations.push_back(limitation);
    result.limitations.push_back("mdat 载荷按有界分块扫描，每个 NAL 只读语法前缀，帧体不整段读入内存");
    result.limitations.push_back("样本边界由 NAL length-prefix 与 first_mb_in_slice 推断，不是原始 stsz");
    result.limitations.push_back("未重建 edts/ctts；若源流含 B 帧，显示顺序可能不完全等价");
    return result;
}

// 单独解析宽高（SPS 里 VUI 之前的字段）。
bool ParseSpsDimensions(const std::vector<std::uint8_t>& nal, std::uint32_t* width, std::uint32_t* height,
                        std::string* error) {
    if (nal.size() < 4) {
        *error = "SPS 长度不足";
        return false;
    }
    const std::vector<std::uint8_t> rbsp = Rbsp(nal.data(), nal.size(), 1);
    BitReader reader(rbsp.data(), rbsp.size());
    reader.U(8);
    reader.U(8);
    reader.U(8);
    reader.Ue();
    const std::uint8_t profile = nal[1];
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        reader.Ue();
        reader.Ue();
        reader.Ue();
        reader.Bit();
        if (reader.Bit()) {
            *error = "SPS 含 scaling matrix";
            return false;
        }
    }
    reader.Ue();
    const std::uint32_t poc_type = reader.Ue();
    if (poc_type == 0) {
        reader.Ue();
    } else if (poc_type == 1) {
        reader.Bit();
        reader.Ue();
        reader.Ue();
        const std::uint32_t cycle = reader.Ue();
        if (cycle > 255) {
            *error = "SPS poc cycle 异常";
            return false;
        }
        for (std::uint32_t i = 0; i < cycle; ++i)
            reader.Ue();
    }
    reader.Ue();
    reader.Bit();
    const std::uint32_t width_mbs = reader.Ue() + 1;
    const std::uint32_t height_map_units = reader.Ue() + 1;
    const bool frame_mbs_only = reader.Bit();
    if (!frame_mbs_only)
        reader.Bit();
    reader.Bit();
    std::uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (reader.Bit()) {
        crop_left = reader.Ue();
        crop_right = reader.Ue();
        crop_top = reader.Ue();
        crop_bottom = reader.Ue();
    }
    if (!reader.ok()) {
        *error = "SPS 宽高解析中断";
        return false;
    }
    // 这里不解析 chroma_format_idc 的裁剪单位差异（主流 4:2:0 足够），
    // 若 profile 是 4:2:2/4:4:4 也只按 4:2:0 裁剪单位估算。
    const std::uint32_t crop_unit_x = 2;
    const std::uint32_t crop_unit_y = 2 * (frame_mbs_only ? 1u : 2u);
    *width = width_mbs * 16u - (crop_left + crop_right) * crop_unit_x;
    *height = (frame_mbs_only ? 1u : 2u) * height_map_units * 16u - (crop_top + crop_bottom) * crop_unit_y;
    return *width > 0 && *height > 0;
}

// ---- moov 构造 ----
void WriteEsds(RepairBoxWriter* writer, const ScanResult& scan, std::uint32_t track_id) {
    // AudioSpecificConfig：AOT(5) + sfi(4) + channelConfiguration(4) + 3 位零。
    std::vector<std::uint8_t> asc;
    asc.push_back(static_cast<std::uint8_t>((scan.aac_object_type << 3) | (scan.audio_sfi >> 1)));
    asc.push_back(static_cast<std::uint8_t>(((scan.audio_sfi & 0x01) << 7) | (scan.channels << 3)));

    auto put_u16 = [](std::vector<std::uint8_t>* out, std::uint16_t value) {
        out->push_back(static_cast<std::uint8_t>(value >> 8));
        out->push_back(static_cast<std::uint8_t>(value));
    };
    auto put_u32 = [](std::vector<std::uint8_t>* out, std::uint32_t value) {
        out->push_back(static_cast<std::uint8_t>(value >> 24));
        out->push_back(static_cast<std::uint8_t>(value >> 16));
        out->push_back(static_cast<std::uint8_t>(value >> 8));
        out->push_back(static_cast<std::uint8_t>(value));
    };
    // MPEG-4 descriptor：tag + 长度（1~4 字节可变） + 内容。
    auto make_descriptor = [](std::uint8_t tag, const std::vector<std::uint8_t>& body) {
        std::vector<std::uint8_t> out;
        out.push_back(tag);
        const std::uint32_t size = static_cast<std::uint32_t>(body.size());
        if (size < 128) {
            out.push_back(static_cast<std::uint8_t>(size));
        } else {
            out.push_back(static_cast<std::uint8_t>(0x80 | ((size >> 21) & 0x7F)));
            out.push_back(static_cast<std::uint8_t>(0x80 | ((size >> 14) & 0x7F)));
            out.push_back(static_cast<std::uint8_t>(0x80 | ((size >> 7) & 0x7F)));
            out.push_back(static_cast<std::uint8_t>(size & 0x7F));
        }
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };

    std::vector<std::uint8_t> dec_specific = make_descriptor(0x05, asc);
    std::vector<std::uint8_t> decoder_body;
    decoder_body.push_back(0x40); // MPEG-4 audio
    decoder_body.push_back(0x15); // audio stream
    decoder_body.push_back(0);
    decoder_body.push_back(0);
    decoder_body.push_back(0); // bufferSizeDB
    put_u32(&decoder_body, 0); // maxBitrate
    put_u32(&decoder_body, 0); // avgBitrate
    decoder_body.insert(decoder_body.end(), dec_specific.begin(), dec_specific.end());
    std::vector<std::uint8_t> decoder_config = make_descriptor(0x04, decoder_body);

    std::vector<std::uint8_t> sl_config = make_descriptor(0x06, std::vector<std::uint8_t>{0x02});

    std::vector<std::uint8_t> es_body;
    put_u16(&es_body, static_cast<std::uint16_t>(track_id));
    es_body.push_back(0);
    es_body.insert(es_body.end(), decoder_config.begin(), decoder_config.end());
    es_body.insert(es_body.end(), sl_config.begin(), sl_config.end());
    const std::vector<std::uint8_t> es = make_descriptor(0x03, es_body);

    const std::size_t box = writer->BeginFullBox("esds", 0, 0);
    writer->Raw(es);
    writer->EndBox(box);
}

void WriteAvcC(RepairBoxWriter* writer, const ScanResult& scan) {
    const std::size_t box = writer->BeginBox("avcC");
    writer->U8(1);
    writer->U8(scan.profile);
    writer->U8(scan.compatibility);
    writer->U8(scan.level);
    writer->U8(0xFC | 3); // lengthSizeMinusOne = 3
    writer->U8(static_cast<std::uint8_t>(0xE0 | 1));
    writer->U16(static_cast<std::uint16_t>(scan.sps.size()));
    writer->Raw(scan.sps);
    writer->U8(static_cast<std::uint8_t>(scan.pps.size()));
    for (const auto& pps : scan.pps) {
        writer->U16(static_cast<std::uint16_t>(pps.size()));
        writer->Raw(pps);
    }
    writer->EndBox(box);
}

void WriteVideoSampleEntry(RepairBoxWriter* writer, const ScanResult& scan) {
    const std::size_t entry = writer->BeginBox("avc1");
    writer->Zeros(6);
    writer->U16(1);
    writer->U16(0);
    writer->U16(0);
    writer->Zeros(12);
    writer->U16(static_cast<std::uint16_t>(scan.width));
    writer->U16(static_cast<std::uint16_t>(scan.height));
    writer->U32(0x00480000);
    writer->U32(0x00480000);
    writer->U32(0);
    writer->U16(1);
    writer->Zeros(32);
    writer->U16(0x0018);
    writer->U16(0xFFFF);
    WriteAvcC(writer, scan);
    writer->EndBox(entry);
}

void WriteAudioSampleEntry(RepairBoxWriter* writer, const ScanResult& scan, std::uint32_t track_id) {
    const std::size_t entry = writer->BeginBox("mp4a");
    writer->Zeros(6);
    writer->U16(1);
    writer->Zeros(8);
    writer->U16(scan.channels);
    writer->U16(0x0010);
    writer->U16(0);
    writer->U16(0);
    writer->U32(static_cast<std::uint32_t>(scan.sample_rate) << 16);
    WriteEsds(writer, scan, track_id);
    writer->EndBox(entry);
}

void WriteMatrix(RepairBoxWriter* writer) {
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

std::vector<std::uint8_t> BuildMoov(const ScanResult& scan, std::uint32_t track_id, std::uint64_t duration,
                                    std::uint64_t chunk_offset) {
    RepairBoxWriter writer;
    const std::size_t moov = writer.BeginBox("moov");

    // ---- mvhd ----
    {
        const std::size_t box = writer.BeginFullBox("mvhd", 0, 0);
        writer.U32(0);
        writer.U32(0);
        writer.U32(scan.timescale);
        writer.U32(static_cast<std::uint32_t>(duration));
        writer.U32(0x00010000);
        writer.U16(0x0100);
        writer.Zeros(2 + 8);
        WriteMatrix(&writer);
        writer.Zeros(24);
        writer.U32(track_id + 1);
        writer.EndBox(box);
    }

    // ---- trak ----
    {
        const std::size_t trak = writer.BeginBox("trak");
        {
            const std::size_t box = writer.BeginFullBox("tkhd", 0, 3);
            writer.U32(0);
            writer.U32(0);
            writer.U32(track_id);
            writer.U32(0);
            writer.U32(static_cast<std::uint32_t>(duration));
            writer.Zeros(8);
            writer.U16(0);
            writer.U16(0);
            writer.U16(scan.video ? 0 : 0x0100);
            writer.U16(0);
            WriteMatrix(&writer);
            writer.U32(scan.video ? scan.width << 16 : 0);
            writer.U32(scan.video ? scan.height << 16 : 0);
            writer.EndBox(box);
        }
        {
            const std::size_t mdia = writer.BeginBox("mdia");
            {
                const std::size_t box = writer.BeginFullBox("mdhd", 0, 0);
                writer.U32(0);
                writer.U32(0);
                writer.U32(scan.timescale);
                writer.U32(static_cast<std::uint32_t>(duration));
                writer.U16(0x55C4); // und
                writer.U16(0);
                writer.EndBox(box);
            }
            {
                const std::size_t box = writer.BeginFullBox("hdlr", 0, 0);
                writer.U32(0);
                writer.FourCC(scan.video ? "vide" : "soun");
                writer.Zeros(12);
                const std::string handler_name = scan.video ? "VideoHandler" : "SoundHandler";
                writer.Raw(std::vector<std::uint8_t>(handler_name.begin(), handler_name.end()));
                writer.U8(0);
                writer.EndBox(box);
            }
            {
                const std::size_t minf = writer.BeginBox("minf");
                if (scan.video) {
                    const std::size_t vmhd = writer.BeginFullBox("vmhd", 0, 1);
                    writer.U16(0);
                    writer.U16(0);
                    writer.U16(0);
                    writer.U16(0);
                    writer.EndBox(vmhd);
                } else {
                    const std::size_t smhd = writer.BeginFullBox("smhd", 0, 0);
                    writer.U16(0);
                    writer.U16(0);
                    writer.EndBox(smhd);
                }
                {
                    const std::size_t dinf = writer.BeginBox("dinf");
                    const std::size_t dref = writer.BeginFullBox("dref", 0, 0);
                    writer.U32(1);
                    const std::size_t url = writer.BeginFullBox("url ", 0, 1);
                    writer.EndBox(url);
                    writer.EndBox(dref);
                    writer.EndBox(dinf);
                }
                {
                    const std::size_t stbl = writer.BeginBox("stbl");
                    {
                        const std::size_t stsd = writer.BeginFullBox("stsd", 0, 0);
                        writer.U32(1);
                        if (scan.video)
                            WriteVideoSampleEntry(&writer, scan);
                        else
                            WriteAudioSampleEntry(&writer, scan, track_id);
                        writer.EndBox(stsd);
                    }
                    {
                        const std::size_t stts = writer.BeginFullBox("stts", 0, 0);
                        writer.U32(1);
                        writer.U32(static_cast<std::uint32_t>(scan.samples.size()));
                        writer.U32(scan.sample_delta);
                        writer.EndBox(stts);
                    }
                    {
                        const std::size_t stsc = writer.BeginFullBox("stsc", 0, 0);
                        writer.U32(1);
                        writer.U32(1);
                        writer.U32(static_cast<std::uint32_t>(scan.samples.size()));
                        writer.U32(1);
                        writer.EndBox(stsc);
                    }
                    {
                        const std::size_t stsz = writer.BeginFullBox("stsz", 0, 0);
                        writer.U32(0);
                        writer.U32(static_cast<std::uint32_t>(scan.samples.size()));
                        for (const auto& sample : scan.samples)
                            writer.U32(sample.size);
                        writer.EndBox(stsz);
                    }
                    if (chunk_offset > 0xFFFFFFFFull) {
                        const std::size_t co64 = writer.BeginFullBox("co64", 0, 0);
                        writer.U32(1);
                        writer.U64(chunk_offset);
                        writer.EndBox(co64);
                    } else {
                        const std::size_t stco = writer.BeginFullBox("stco", 0, 0);
                        writer.U32(1);
                        writer.U32(static_cast<std::uint32_t>(chunk_offset));
                        writer.EndBox(stco);
                    }
                    if (scan.video) {
                        std::vector<std::uint32_t> sync_samples;
                        for (std::size_t i = 0; i < scan.samples.size(); ++i) {
                            if (scan.samples[i].sync)
                                sync_samples.push_back(static_cast<std::uint32_t>(i + 1));
                        }
                        if (!sync_samples.empty() && sync_samples.size() != scan.samples.size()) {
                            const std::size_t stss = writer.BeginFullBox("stss", 0, 0);
                            writer.U32(static_cast<std::uint32_t>(sync_samples.size()));
                            for (std::uint32_t index : sync_samples)
                                writer.U32(index);
                            writer.EndBox(stss);
                        }
                    }
                    writer.EndBox(stbl);
                }
                writer.EndBox(minf);
            }
            writer.EndBox(mdia);
        }
        writer.EndBox(trak);
    }
    writer.EndBox(moov);
    return writer.TakeBytes();
}

std::vector<std::uint8_t> MakeFtyp(std::vector<std::uint8_t> source) {
    if (!source.empty())
        return source;
    RepairBoxWriter writer;
    const std::size_t box = writer.BeginBox("ftyp");
    writer.FourCC("isom");
    writer.U32(512);
    writer.FourCC("isom");
    writer.FourCC("iso2");
    writer.FourCC("mp41");
    writer.EndBox(box);
    return writer.TakeBytes();
}

void WriteMdatHeader(std::ofstream& out, std::uint64_t payload_size) {
    // 旧实现把 'mdat' 写在 [0..3] 又被 size 覆盖，[4..7] 恒为 0 —— 产物里
    // 是一个类型全零的盒，靠样本偏移恰好能读，但结构自检没发现。
    std::uint8_t header[16] = {};
    const std::size_t header_size = MakeMdatHeader(payload_size, header);
    out.write(reinterpret_cast<const char*>(header), static_cast<std::streamsize>(header_size));
}

ScanResult ScanPayload(const std::string& source_path, const std::atomic<bool>* cancel, const RepairRunBudget* budget,
                       MdatInfo* mdat_out, std::uint64_t* file_size_out, std::vector<PlainBox>* boxes_out,
                       std::vector<std::uint8_t>* ftyp_out, std::string* error, std::size_t scan_chunk_bytes) {
    ScanResult result;
    std::vector<PlainBox> boxes;
    MdatInfo mdat;
    std::uint64_t file_size = 0;
    if (!ScanTopLevel(source_path, &boxes, &mdat, &file_size, error)) {
        return result;
    }
    if (!mdat.found) {
        *error = "源文件没有 mdat，无法重建样本表";
        return result;
    }
    if (mdat.count != 1) {
        *error = "存在多个 mdat，无 moov 时无法判断 chunk 交错布局";
        return result;
    }
    for (const auto& box : boxes) {
        if (box.type == "moof") {
            *error = "源文件是分片 MP4（含 moof），不走普通 MP4 重建";
            return result;
        }
        if (box.type == "moov") {
            *error = "源文件已有 moov，不需要无参考重建";
            return result;
        }
        if (box.type == "ftyp") {
            ftyp_out->resize(static_cast<std::size_t>(box.size));
            std::ifstream ftyp_in(ToNativePath(source_path), std::ios::binary);
            if (ftyp_in && !ReadAt(ftyp_in, box.offset, ftyp_out->data(), ftyp_out->size()))
                ftyp_out->clear();
        }
    }

    ScanWindow window;
    window.file_offset = mdat.payload;
    window.size = mdat.payload_size;
    window.track_id = 1;
    window.stream_index = 0;
    window.segment_index = 0;
    window.config_id = "plain_mp4_mdat_scan";

    // 两个 producer 各用独立的分块字节源：第一次扫描因「码流类型不匹配」失败时，
    // 不该把它的读取状态带进第二次扫描。
    ChunkedByteSource audio_source(source_path, window, scan_chunk_bytes, budget, cancel);
    if (!audio_source.Error().empty()) {
        *error = audio_source.Error();
        return result;
    }

    // 先试 ADTS 纯音频，再试 AVCC 纯视频；两者都失败才拒绝。
    ScanResult audio = ScanAdts(audio_source, window, cancel, budget);
    if (audio.ok) {
        *mdat_out = mdat;
        *file_size_out = file_size;
        *boxes_out = std::move(boxes);
        return audio;
    }
    ChunkedByteSource video_source(source_path, window, scan_chunk_bytes, budget, cancel);
    ScanResult video = ScanAvcc(video_source, window, cancel, budget);
    if (video.ok) {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::string dimension_error;
        if (!ParseSpsDimensions(video.sps, &width, &height, &dimension_error)) {
            *error = "SPS 宽高解析失败: " + dimension_error;
            return result;
        }
        video.width = width;
        video.height = height;
        *mdat_out = mdat;
        *file_size_out = file_size;
        *boxes_out = std::move(boxes);
        return video;
    }
    *error = "mdat 载荷既不是纯 ADTS AAC，也不是纯 AVCC H.264（音视频交织或码流损坏）。" + video.error;
    return result;
}

} // namespace

RepairPlainMp4Probe RepairPlainMp4Rebuilder::Probe(const std::string& source_path, const std::atomic<bool>* cancel,
                                                   const RepairRunBudget* budget, std::size_t scan_chunk_bytes) {
    RepairPlainMp4Probe probe;
    MdatInfo mdat;
    std::uint64_t file_size = 0;
    std::vector<PlainBox> boxes;
    std::vector<std::uint8_t> ftyp;
    std::string error;
    const ScanResult scan =
        ScanPayload(source_path, cancel, budget, &mdat, &file_size, &boxes, &ftyp, &error, scan_chunk_bytes);
    if (!scan.ok) {
        probe.reason = error;
        return probe;
    }
    if (scan.width > 0 && scan.height > 0) {
        probe.codec_summary = scan.codec_summary + " / " + std::to_string(scan.width) + "x" +
                              std::to_string(scan.height) + " / " + std::to_string(scan.ledger.Total()) + " 个样本";
    } else {
        probe.codec_summary = scan.codec_summary + " / " + std::to_string(scan.ledger.Total()) + " 个样本";
    }
    probe.attemptable = true;
    probe.kind = scan.kind;
    probe.limitations = scan.limitations;
    return probe;
}

RepairPlainMp4RebuildResult RepairPlainMp4Rebuilder::Rebuild(const std::string& source_path,
                                                             const std::string& output_path,
                                                             const std::atomic<bool>* cancel,
                                                             const RepairRunBudget* budget,
                                                             std::size_t scan_chunk_bytes) {
    RepairPlainMp4RebuildResult result;
    result.attempted = true;
    if (Stopped(cancel, budget, &result.error))
        return result;

    MdatInfo mdat;
    std::uint64_t file_size = 0;
    std::vector<PlainBox> boxes;
    std::vector<std::uint8_t> ftyp_source;
    std::string error;
    const ScanResult scan =
        ScanPayload(source_path, cancel, budget, &mdat, &file_size, &boxes, &ftyp_source, &error, scan_chunk_bytes);
    if (!scan.ok) {
        result.error = error.empty() ? "无法从 mdat 重建样本表" : error;
        return result;
    }
    if (scan.samples.empty()) {
        result.error = "扫描没有产生任何样本";
        return result;
    }

    result.probe.attemptable = true;
    result.probe.kind = scan.kind;
    result.probe.codec_summary = scan.codec_summary;
    result.probe.limitations = scan.limitations;

    std::vector<std::uint8_t> ftyp_bytes = MakeFtyp(std::move(ftyp_source));
    const std::uint32_t track_id = 1;
    const std::uint64_t media_duration = static_cast<std::uint64_t>(scan.sample_delta) * scan.ledger.Total();

    // moov 大小依赖 stco/co64 的选择，后者又依赖载荷起点；迭代到尺寸收敛。
    const std::uint64_t total_payload = scan.total_bytes;
    const std::uint64_t mdat_header_size = (total_payload + 8 > 0xFFFFFFFFull) ? 16 : 8;
    std::vector<std::uint8_t> moov_bytes;
    std::uint64_t payload_start = 0;
    for (int pass = 0; pass < 4; ++pass) {
        moov_bytes = BuildMoov(scan, track_id, media_duration, payload_start);
        const std::uint64_t new_start = ftyp_bytes.size() + moov_bytes.size() + mdat_header_size;
        if (new_start == payload_start && pass > 0)
            break;
        payload_start = new_start;
    }
    moov_bytes = BuildMoov(scan, track_id, media_duration, payload_start);
    if (ftyp_bytes.size() + moov_bytes.size() + mdat_header_size != payload_start) {
        result.error = "重建 moov 尺寸未收敛，拒绝写出可能错位的结果";
        return result;
    }

    std::ofstream out(ToNativePath(output_path), std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "无法创建重建文件: " + output_path;
        return result;
    }
    out.write(reinterpret_cast<const char*>(ftyp_bytes.data()), static_cast<std::streamsize>(ftyp_bytes.size()));
    out.write(reinterpret_cast<const char*>(moov_bytes.data()), static_cast<std::streamsize>(moov_bytes.size()));
    WriteMdatHeader(out, total_payload);
    if (!out) {
        result.error = "写入重建文件头失败";
        return result;
    }

    std::ifstream in(ToNativePath(source_path), std::ios::binary);
    if (!in) {
        result.error = "无法读取源文件";
        return result;
    }
    std::uint64_t written = 0;
    for (const auto& sample : scan.samples) {
        if (Stopped(cancel, budget, &result.error))
            return result;
        std::vector<std::uint8_t> buffer(sample.size);
        if (!ReadAt(in, sample.source_offset, buffer.data(), buffer.size())) {
            result.error = "读取样本失败：offset=" + std::to_string(sample.source_offset);
            return result;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!out) {
            result.error = "写入重建样本失败";
            return result;
        }
        written += sample.size;
    }
    out.flush();
    if (!out) {
        result.error = "重建文件落盘失败";
        return result;
    }
    out.close();
    if (written != total_payload) {
        result.error = "重建写入字节数与样本表不一致";
        return result;
    }

    IsobmffFile check;
    if (!IsobmffParser::Parse(output_path, check) || !check.ok || !check.found_moov || check.tracks.size() != 1) {
        result.error = "重建结果结构自检失败: " + check.error_message;
        return result;
    }
    if (!VerifyRebuiltMdat(check, payload_start, payload_start + total_payload, total_payload, &result.error))
        return result;

    result.ok = true;
    // 上层样本数直接取样本层总数，不再从 scan.samples 另数一遍。
    result.samples_rebuilt = scan.ledger.Total();
    result.sample_ledger = scan.ledger.Summary();
    result.rebuilt_tracks = 1;
    result.output_bytes = ftyp_bytes.size() + moov_bytes.size() + mdat_header_size + total_payload;
    result.note = "无 moov，按 mdat 扫描重建 " + scan.codec_summary + " 单轨，样本 " +
                  std::to_string(scan.ledger.Total()) + " 个；时基 " + std::to_string(scan.timescale) +
                  " ticks，样本时长 " + std::to_string(scan.sample_delta);
    result.probe.limitations.push_back("重建产物仍需重新封装与解码验证");
    if (mdat.payload_size != total_payload)
        result.note += "；mdat 中未被样本引用的字节已丢弃";
    return result;
}

} // namespace repair
} // namespace videoeye
