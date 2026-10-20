#include "core/analysis/codec/VideoEsInspector.h"

#include <algorithm>
#include <cmath>
#include <numeric>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataParser.h"

namespace videoeye {
namespace {

// SEI 的 payloadType / payloadSize：0xFF 累加段 + 一个非 FF 尾巴。
// 返回 false = 连一个完整的 type/size 都没读完（流截断了）。
bool ReadSeiValue(const uint8_t* data, size_t size, size_t& pos, uint32_t& out) {
    uint32_t value = 0;
    while (pos < size) {
        const uint8_t b = data[pos++];
        value += b;
        if (b != 0xFF) {
            out = value;
            return true;
        }
    }
    return false;
}

bool IsH264SliceNal(int type) { return type == 1 || type == 2 || type == 5; }
bool IsH264SeiNal(int type) { return type == 6; }

bool IsHevcSliceNal(int type) { return type <= 21; }
bool IsHevcSeiNal(int type) { return type == 39 || type == 40; }  // prefix / suffix SEI

// Dolby Vision RPU：HEVC 里挂在 NAL type 62（unspecified）上，
// RPU payload 首字节恒为 0x19。
constexpr int kHevcNalUnspecified62 = 62;
constexpr uint8_t kDvRpuPrefix = 0x19;

// HDR10+ 的 ITU-T T.35 标识（SMPTE ST 2094-40）
constexpr uint8_t kT35CountryCodeUs = 0xB5;
constexpr uint16_t kT35TerminalProviderSmpte = 0x003B;
constexpr uint16_t kT35ProviderOrientedCodeSt2094 = 0x8000;

// SEI payloadType
constexpr int kSeiPicTiming = 1;
constexpr int kSeiUserDataRegistered = 4;
constexpr int kSeiUserDataUnregistered = 5;
constexpr int kSeiRecoveryPoint = 6;
constexpr int kSeiFilmGrainCharacteristics = 19;
constexpr int kSeiMasteringDisplayColourVolume = 137;
constexpr int kSeiContentLightLevelInfo = 144;

// AV1 metadata OBU 的 metadata_type
constexpr uint32_t kAv1MetadataTypeHdrCll = 1;
constexpr uint32_t kAv1MetadataTypeHdrMdcv = 2;
constexpr uint32_t kAv1MetadataTypeItutT35 = 4;

// H.264/HEVC MDCV 的单位：chromaticity 是 0.00002、luminance 是 0.0001
constexpr double kChromaScale264 = 0.00002;
constexpr double kLuminanceScale264 = 0.0001;
// AV1 HDR MDCV metadata OBU 的单位：chromaticity 是 0.00016
constexpr double kChromaScaleAv1 = 0.00016;

bool IsPlausibleQp(int qp) { return qp >= 0 && qp <= 51; }

// ceil(log2(v))，给 slice_segment_address 的位宽用
int CeilLog2(uint32_t v) {
    int bits = 0;
    while ((1u << bits) < v) ++bits;
    return bits;
}

// leb128：AV1 的 obu_size / metadata_type 都用它。
// 返回 false = 越界或畸形（连续 8 字节还没终止位）。
bool ReadLeb128(const uint8_t* data, size_t size, size_t& pos, uint64_t& out) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        if (pos >= size) return false;
        const uint8_t b = data[pos++];
        value |= static_cast<uint64_t>(b & 0x7F) << (i * 7);
        if ((b & 0x80) == 0) {
            out = value;
            return true;
        }
    }
    return false;
}

}  // namespace

void VideoEsInspector::Reset(int codec_id) { Reset(codec_id, Options{}); }

void VideoEsInspector::Reset(int codec_id, const Options& options) {
    codec_id_ = codec_id;
    options_ = options;
    packets_ = 0;
    done_ = false;
    has_h264_params_ = false;
    h264_sps_ = model::H264SpsInfo();
    h264_pps_ = model::H264PpsInfo();
    hevc_sps_ = model::HevcSpsInfo();
    hevc_pps_ = model::HevcPpsInfo();
    slice_types_.clear();
    slice_qps_.clear();
    result_ = model::SliceSeiSummary();

    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264:
            result_.codec_name = "h264";
            enabled_ = true;
            result_.slices.supported = true;
            break;
        case AV_CODEC_ID_HEVC:
            result_.codec_name = "hevc";
            enabled_ = true;
            // slice QP 不支持要在最开始就写清楚：调用方（面板/报告）据此决定
            // 要不要展示 QP 那一块，而不是看到一个全是 0 的表以为"这条流没 QP"。
            result_.slices.supported = false;
            result_.slices.unsupported_reason =
                "HEVC 的 slice_qp_delta 前面压着完整的 st_ref_pic_set() 与 "
                "NumPocTotalCurr 推导；只实现一半会得到错位的数值，暂不产出 QP";
            break;
        case AV_CODEC_ID_AV1:
            result_.codec_name = "av1";
            enabled_ = true;
            result_.slices.supported = false;
            result_.slices.unsupported_reason =
                "AV1 的逐帧 QP 在 frame header 的 delta_qparams 段里，需要先解完 "
                "uncompressed header（含 tile / segmentation 分支），暂不支持";
            break;
        default:
            enabled_ = false;
            break;
    }
}

void VideoEsInspector::SetH264ParameterSets(const model::H264SpsInfo& sps,
                                            const model::H264PpsInfo& pps) {
    h264_sps_ = sps;
    h264_pps_ = pps;
    has_h264_params_ = sps.present && pps.present;
}

void VideoEsInspector::SetHevcParameterSets(const model::HevcSpsInfo& sps,
                                            const model::HevcPpsInfo& pps) {
    hevc_sps_ = sps;
    hevc_pps_ = pps;
}

bool VideoEsInspector::Feed(const uint8_t* data, size_t size) {
    if (!enabled_ || done_ || data == nullptr || size == 0) return false;
    if (options_.max_packets > 0 && packets_ >= options_.max_packets) {
        result_.truncated = true;
        done_ = true;
        return false;
    }
    ++packets_;
    result_.packets_scanned = packets_;

    switch (static_cast<AVCodecID>(codec_id_)) {
        case AV_CODEC_ID_H264:
            InspectH264(data, size);
            break;
        case AV_CODEC_ID_HEVC:
            InspectHevc(data, size);
            break;
        case AV_CODEC_ID_AV1:
            InspectAv1(data, size);
            break;
        default:
            break;
    }
    return true;
}

void VideoEsInspector::InspectH264(const uint8_t* data, size_t size) {
    const std::vector<NalUnit> nals =
        ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::H264);
    for (const NalUnit& nal : nals) {
        if (IsH264SliceNal(nal.type)) {
            int slice_type = 0;
            int slice_qp = 0;
            if (ParseH264SliceHeader(nal.data.data(), nal.data.size(),
                                     nal.is_idr, slice_type, slice_qp)) {
                slice_types_.push_back(slice_type);
                slice_qps_.push_back(slice_qp);
            }
        } else if (IsH264SeiNal(nal.type)) {
            ParseSeiRbsp(nal.data.data(), nal.data.size(), /*is_hevc=*/false);
        }
    }
}

void VideoEsInspector::InspectHevc(const uint8_t* data, size_t size) {
    const std::vector<NalUnit> nals =
        ExtradataParser::ExtractAnnBNalUnits(data, size, NalSyntax::Hevc);
    for (const NalUnit& nal : nals) {
        if (IsHevcSeiNal(nal.type)) {
            ParseSeiRbsp(nal.data.data(), nal.data.size(), /*is_hevc=*/true);
        } else if (nal.type == kHevcNalUnspecified62 && !nal.data.empty() &&
                   nal.data[0] == kDvRpuPrefix) {
            result_.sei.has_dolby_vision_rpu = true;
            ++result_.sei.dolby_vision_rpu_count;
        } else if (IsHevcSliceNal(nal.type)) {
            // slice QP 不支持（见 Reset() 里写的理由）。这里只统计 slice_type 分布
            // 的前置条件也不具备（slice_type 在 dependent_slice 判定之后），所以
            // 干脆不碰 —— 宁可缺一块，也不给可能对不上号的分布表。
        }
    }
}

void VideoEsInspector::InspectAv1(const uint8_t* data, size_t size) {
    const std::vector<ObuUnit> obus = ExtradataParser::ExtractObuUnits(data, size);
    for (const ObuUnit& obu : obus) {
        if (obu.type != static_cast<uint8_t>(Av1ObuType::Metadata)) continue;
        size_t pos = 0;
        uint64_t metadata_type = 0;
        if (!ReadLeb128(obu.data.data(), obu.data.size(), pos, metadata_type)) continue;

        if (metadata_type == kAv1MetadataTypeHdrCll) {
            if (pos + 4 <= obu.data.size()) {
                model::SeiSummary& sei = result_.sei;
                sei.has_content_light_level = true;
                sei.max_content_light_level = (static_cast<uint32_t>(obu.data[pos]) << 8) |
                                              obu.data[pos + 1];
                sei.max_pic_average_light_level =
                    (static_cast<uint32_t>(obu.data[pos + 2]) << 8) | obu.data[pos + 3];
            }
        } else if (metadata_type == kAv1MetadataTypeHdrMdcv) {
            // primary_chromaticity_x/y[3] u(16) + white_point x/y u(16)
            // + luminance_max/min u(32)
            if (pos + 16 + 8 <= obu.data.size()) {
                model::SeiSummary& sei = result_.sei;
                const auto u16 = [&](size_t i) {
                    return (static_cast<uint32_t>(obu.data[pos + i]) << 8) |
                           obu.data[pos + i + 1];
                };
                const auto u32 = [&](size_t i) {
                    return (static_cast<uint32_t>(obu.data[pos + i]) << 24) |
                           (static_cast<uint32_t>(obu.data[pos + i + 1]) << 16) |
                           (static_cast<uint32_t>(obu.data[pos + i + 2]) << 8) |
                           static_cast<uint32_t>(obu.data[pos + i + 3]);
                };
                for (int i = 0; i < 3; ++i) {
                    sei.display_primaries_x[i] = static_cast<double>(u16(i * 4)) * kChromaScaleAv1;
                    sei.display_primaries_y[i] =
                        static_cast<double>(u16(i * 4 + 2)) * kChromaScaleAv1;
                }
                sei.white_point_x = static_cast<double>(u16(12)) * kChromaScaleAv1;
                sei.white_point_y = static_cast<double>(u16(14)) * kChromaScaleAv1;
                sei.max_display_mastering_luminance = static_cast<double>(u32(16));
                sei.min_display_mastering_luminance = static_cast<double>(u32(20));
                sei.has_mastering_display = true;
            }
        } else if (metadata_type == kAv1MetadataTypeItutT35) {
            if (pos + 4 <= obu.data.size() && obu.data[pos] == kT35CountryCodeUs) {
                const uint16_t provider_code =
                    static_cast<uint16_t>((static_cast<uint16_t>(obu.data[pos + 1]) << 8) |
                                          obu.data[pos + 2]);
                if (provider_code == kT35TerminalProviderSmpte) {
                    result_.sei.has_hdr10plus = true;
                    ++result_.sei.hdr10plus_count;
                }
            }
        }
    }
}

void VideoEsInspector::ParseSeiRbsp(const uint8_t* data, size_t size, bool is_hevc) {
    (void)is_hevc;  // payloadType / payloadSize 的读法两家一致，无需区分
    std::vector<uint8_t> rbsp = UnescapeRbsp(data, size);
    size_t pos = 0;
    while (pos < rbsp.size()) {
        uint32_t payload_type = 0;
        uint32_t payload_size = 0;
        if (!ReadSeiValue(rbsp.data(), rbsp.size(), pos, payload_type)) break;
        if (!ReadSeiValue(rbsp.data(), rbsp.size(), pos, payload_size)) break;
        if (payload_size > rbsp.size() - pos) {
            // size 超出剩余字节：再往下读就跑到下一个 NAL 上了，直接收手
            break;
        }
        HandleSeiPayload(static_cast<int>(payload_type), rbsp.data() + pos, payload_size);
        pos += payload_size;
    }
}

void VideoEsInspector::HandleSeiPayload(int payload_type, const uint8_t* p, size_t n) {
    model::SeiSummary& sei = result_.sei;
    ++sei.total_messages;

    switch (payload_type) {
        case kSeiPicTiming: {
            sei.has_pic_timing = true;
            ++sei.pic_timing_count;
            BitReader reader;
            reader.Reset(p, n);
            // pic_timing 的第一项是可选的 pic_struct（只有 SPS VUI 的
            // pic_struct_present_flag=1 时才存在）。这里拿不到 VUI，
            // 所以只记录"有这条 SEI"，不去猜它到底有没有。
            if (n >= 1) {
                sei.pic_struct = static_cast<int>(reader.ReadBits(4));
            }
            break;
        }
        case kSeiRecoveryPoint:
            sei.has_recovery_point = true;
            break;
        case kSeiUserDataRegistered: {
            // HDR10+：ITU-T T.35 里 country_code=0xB5 (US)、
            // terminal_provider_code=0x003B (SMPTE)、provider_oriented_code=0x8000
            if (n >= 6 && p[0] == kT35CountryCodeUs) {
                const uint16_t provider_code =
                    static_cast<uint16_t>((static_cast<uint16_t>(p[1]) << 8) | p[2]);
                const uint16_t oriented_code =
                    static_cast<uint16_t>((static_cast<uint16_t>(p[3]) << 8) | p[4]);
                if (provider_code == kT35TerminalProviderSmpte &&
                    oriented_code == kT35ProviderOrientedCodeSt2094) {
                    sei.has_hdr10plus = true;
                    ++sei.hdr10plus_count;
                }
            }
            break;
        }
        case kSeiUserDataUnregistered: {
            // Dolby Vision 的另一种携带方式：包在 user_data_unregistered
            // 的 16 字节 UUID 之后。这里不逐个比对厂商 UUID 表 ——
            // 真正的 DV 判定走 HEVC NAL 62 那条更硬的判据（见 InspectHevc）。
            break;
        }
        case kSeiFilmGrainCharacteristics: {
            sei.has_film_grain = true;
            ++sei.film_grain_count;
            BitReader reader;
            reader.Reset(p, n);
            if (n >= 1) {
                sei.film_grain_cancel_flag = reader.ReadBit();
            }
            break;
        }
        case kSeiMasteringDisplayColourVolume: {
            if (n < 8 + 8 + 4 + 4) break;
            BitReader reader;
            reader.Reset(p, n);
            // display_primaries_x/y 的顺序是 G(0) B(1) R(2)，单位是 0.00002
            for (int i = 0; i < 3; ++i) {
                sei.display_primaries_x[i] =
                    static_cast<double>(reader.ReadBits(16)) * kChromaScale264;
                sei.display_primaries_y[i] =
                    static_cast<double>(reader.ReadBits(16)) * kChromaScale264;
            }
            sei.white_point_x = static_cast<double>(reader.ReadBits(16)) * kChromaScale264;
            sei.white_point_y = static_cast<double>(reader.ReadBits(16)) * kChromaScale264;
            sei.max_display_mastering_luminance =
                static_cast<double>(reader.ReadBits(32)) * kLuminanceScale264;
            sei.min_display_mastering_luminance =
                static_cast<double>(reader.ReadBits(32)) * kLuminanceScale264;
            sei.has_mastering_display = true;
            break;
        }
        case kSeiContentLightLevelInfo: {
            if (n < 4) break;
            BitReader reader;
            reader.Reset(p, n);
            sei.max_content_light_level = reader.ReadBits(16);
            sei.max_pic_average_light_level = reader.ReadBits(16);
            sei.has_content_light_level = true;
            break;
        }
        default:
            break;
    }

    // 见到过但还没展开的 payloadType 登记一次供排查（不要每个包都 push）
    const bool handled = payload_type == kSeiPicTiming || payload_type == kSeiRecoveryPoint ||
                         payload_type == kSeiUserDataRegistered ||
                         payload_type == kSeiFilmGrainCharacteristics ||
                         payload_type == kSeiMasteringDisplayColourVolume ||
                         payload_type == kSeiContentLightLevelInfo ||
                         payload_type == kSeiUserDataUnregistered;
    if (!handled &&
        std::find(sei.unhandled_payload_types.begin(), sei.unhandled_payload_types.end(),
                  payload_type) == sei.unhandled_payload_types.end()) {
        sei.unhandled_payload_types.push_back(payload_type);
    }
}

bool VideoEsInspector::ParseH264SliceHeader(const uint8_t* data, size_t size, bool is_idr,
                                            int& slice_type, int& slice_qp) {
    if (data == nullptr || size == 0) return false;
    // 没有 SPS/PPS 就解不出有意义的字段位宽（frame_num、pic_order_cnt_lsb 都依赖它们）
    if (!has_h264_params_) return false;

    BitReader reader;
    reader.Reset(data, size);

    reader.ReadUE();                    // first_mb_in_slice
    slice_type = static_cast<int>(reader.ReadUE());
    if (slice_type > 9) return false;   // 规范只到 9（0..9）
    reader.ReadUE();                    // pic_parameter_set_id
    if (h264_sps_.separate_colour_plane_flag) {
        reader.SkipBits(2);             // colour_plane_id
    }
    reader.SkipBits(h264_sps_.log2_max_frame_num_minus4 + 4);  // frame_num
    bool field_pic = false;
    if (!h264_sps_.frame_mbs_only_flag) {
        field_pic = reader.ReadBit();
        if (field_pic) {
            reader.SkipBits(1);         // bottom_field_flag
        }
    }
    if (is_idr) {
        reader.ReadUE();                // idr_pic_id
    }
    if (h264_sps_.pic_order_cnt_type == 0) {
        reader.SkipBits(h264_sps_.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (h264_pps_.bottom_field_pic_order_in_frame_present_flag && !field_pic) {
            reader.ReadSE();            // delta_pic_order_cnt_bottom
        }
    } else if (h264_sps_.pic_order_cnt_type == 1 &&
               !h264_sps_.delta_pic_order_always_zero_flag) {
        reader.ReadSE();                // delta_pic_order_cnt[0]
        if (h264_pps_.bottom_field_pic_order_in_frame_present_flag && !field_pic) {
            reader.ReadSE();            // delta_pic_order_cnt[1]
        }
    }
    if (h264_pps_.redundant_pic_cnt_present_flag) {
        reader.ReadUE();                // redundant_pic_cnt
    }
    if (slice_type == 1) {              // B slice
        reader.SkipBits(1);             // direct_spatial_mv_pred_flag
    }
    if (slice_type == 0 || slice_type == 1 || slice_type == 3) {  // P / B / SP
        if (reader.ReadBit()) {         // num_ref_idx_active_override_flag
            reader.ReadUE();            // num_ref_idx_l0_active_minus1
            if (slice_type == 1) {
                reader.ReadUE();        // num_ref_idx_l1_active_minus1
            }
        }
    }
    if (reader.HasError()) return false;

    const int delta_qp = reader.ReadSE();
    if (reader.HasError()) return false;
    slice_qp = 26 + h264_pps_.pic_init_qp_minus26 + delta_qp;
    // 错位不会报错，只会给出一个"看似合理"的数 —— 用取值范围做最后一道筛。
    if (!IsPlausibleQp(slice_qp)) return false;
    return true;
}

void VideoEsInspector::Finish() {
    result_.analyzed = true;
    model::SliceSummary& out = result_.slices;
    if (!slice_types_.empty()) {
        out.analyzed = true;
        out.slice_count = static_cast<int>(slice_types_.size());
        out.frames = static_cast<int>(slice_types_.size());
        for (int t : slice_types_) {
            switch (t) {
                case 0: ++out.type_p; break;
                case 1: ++out.type_b; break;
                case 2: ++out.type_i; break;
                default: ++out.type_other; break;
            }
        }
    }
    if (!slice_qps_.empty()) {
        out.analyzed = true;
        out.qp_valid = has_h264_params_;
        out.qp_min = *std::min_element(slice_qps_.begin(), slice_qps_.end());
        out.qp_max = *std::max_element(slice_qps_.begin(), slice_qps_.end());
        out.qp_avg = std::accumulate(slice_qps_.begin(), slice_qps_.end(), 0.0) /
                     static_cast<double>(slice_qps_.size());
        const int n = static_cast<int>(slice_qps_.size());
        const int budget = options_.max_curve_points > 0 ? options_.max_curve_points : 600;
        if (n <= budget) {
            out.qp_curve = slice_qps_;
        } else {
            // 抽稀要留住首尾：GOP 的 QP 形状主要就在这两处体现
            const double step = static_cast<double>(n - 1) / static_cast<double>(budget - 1);
            out.qp_curve.reserve(budget);
            for (int i = 0; i < budget; ++i) {
                out.qp_curve.push_back(slice_qps_[static_cast<size_t>(std::lround(i * step))]);
            }
        }
    }
}

}  // namespace videoeye
