#include "core/analyzer/Av1BitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

namespace {
// AV1 规范附录 6.4.2 的 CICP 取值，dav1d 里同名常量
constexpr int kColorPrimariesBt709 = 1;
constexpr int kColorPrimariesUnknown = 2;
constexpr int kTransferCharacteristicsSrgb = 13;
constexpr int kTransferCharacteristicsUnknown = 2;
constexpr int kMatrixCoefficientsIdentity = 0;
constexpr int kMatrixCoefficientsUnknown = 2;

// screen content tools / integer mv 的「自适应」常量
constexpr int kSelectAdaptive = 2;
} // namespace

std::string Av1BitstreamParser::GetProfileName(int profile) {
    switch (profile) {
        case 0: return "Profile 0";   // Main: 8/10-bit, 4:2:0
        case 1: return "Profile 1";   // High: 8/10-bit, 4:4:4
        case 2: return "Profile 2";   // Professional: 8/10/12-bit
        default: return "Unknown";
    }
}

std::string Av1BitstreamParser::GetLevelString(int seq_level_idx) {
    if (seq_level_idx < 0 || seq_level_idx > 23) return "Unknown";
    return std::to_string(2 + seq_level_idx / 4) + "." + std::to_string(seq_level_idx % 4);
}

bool Av1BitstreamParser::IsSequenceHeaderObu(const utils::ObuUnit& obu_unit) {
    return obu_unit.type == static_cast<uint8_t>(utils::Av1ObuType::SequenceHeader);
}

// timing_info() + decoder_model_info()（AV1 规范 5.5.1）
bool Av1BitstreamParser::SkipTimingInfo(utils::BitReader& reader,
                                       model::Av1SequenceHeaderInfo& sh) {
    sh.timing_info_present_flag = reader.ReadBit();
    if (!sh.timing_info_present_flag) {
        return true;
    }

    sh.num_units_in_tick = reader.ReadBits(32);
    sh.timescale = reader.ReadBits(32);
    const bool equal_picture_interval = reader.ReadBit();
    if (equal_picture_interval) {
        sh.num_ticks_per_picture = reader.ReadUE() + 1;
    }

    sh.decoder_model_info_present_flag = reader.ReadBit();
    if (sh.decoder_model_info_present_flag) {
        sh.decoder_buffer_delay_length = static_cast<int>(reader.ReadBits(5)) + 1;
        reader.SkipBits(32);                                        // num_units_in_decoding_tick
        reader.SkipBits(5);                                         // buffer_removal_time_length_minus_1
        reader.SkipBits(5);                                         // frame_presentation_time_length_minus_1
    }

    return !reader.HasError();
}

// operating_points 循环（AV1 规范 5.5.1）
bool Av1BitstreamParser::SkipOperatingPoints(utils::BitReader& reader,
                                            model::Av1SequenceHeaderInfo& sh) {
    sh.display_model_info_present_flag = reader.ReadBit();
    sh.operating_points_count = static_cast<int>(reader.ReadBits(5)) + 1;

    for (int i = 0; i < sh.operating_points_count; i++) {
        reader.SkipBits(12);                                        // operating_point_idc

        // 5 位 seq_level_idx 被拆成 major(3) + minor(2) 编码，
        // 其中 major 存的是「实际主版本 - 2」
        const int major = 2 + static_cast<int>(reader.ReadBits(3));
        const int minor = static_cast<int>(reader.ReadBits(2));

        int tier = 0;
        if (major > 3) {
            tier = static_cast<int>(reader.ReadBit());              // seq_tier
        }

        if (i == 0) {
            sh.level = (major - 2) * 4 + minor;                     // seq_level_idx 0..23
            sh.tier = tier;
        }

        if (sh.decoder_model_info_present_flag) {
            if (reader.ReadBit()) {                                 // decoder_model_param_present
                reader.SkipBits(sh.decoder_buffer_delay_length);    // decoder_buffer_delay
                reader.SkipBits(sh.decoder_buffer_delay_length);    // encoder_buffer_delay
                reader.SkipBits(1);                                 // low_delay_mode
            }
        }

        bool display_model_param_present = false;
        if (sh.display_model_info_present_flag) {
            display_model_param_present = reader.ReadBit();
        }
        const int initial_display_delay = display_model_param_present
                                         ? static_cast<int>(reader.ReadBits(4)) + 1
                                         : 10;
        if (i == 0) {
            sh.initial_presentation_delay_bits = initial_display_delay;
        }
    }

    return !reader.HasError();
}

// color_config()（AV1 规范 6.4.2）
bool Av1BitstreamParser::ParseColorConfig(utils::BitReader& reader,
                                         model::Av1SequenceHeaderInfo& sh) {
    model::Av1ColorConfigInfo& cc = sh.color_config;

    cc.high_bitdepth = reader.ReadBit();
    int hbd = cc.high_bitdepth ? 1 : 0;
    if (sh.profile == 2 && cc.high_bitdepth) {
        cc.twelve_bit = reader.ReadBit();
        if (cc.twelve_bit) {
            hbd = 2;
        }
    }

    if (sh.profile != 1) {
        cc.monochrome = reader.ReadBit();
    }

    cc.color_description_present_flag = reader.ReadBit();
    if (cc.color_description_present_flag) {
        cc.color_primaries = static_cast<int>(reader.ReadBits(8));
        cc.transfer_characteristics = static_cast<int>(reader.ReadBits(8));
        cc.matrix_coefficients = static_cast<int>(reader.ReadBits(8));
    } else {
        cc.color_primaries = kColorPrimariesUnknown;
        cc.transfer_characteristics = kTransferCharacteristicsUnknown;
        cc.matrix_coefficients = kMatrixCoefficientsUnknown;
    }

    if (cc.monochrome) {
        cc.full_range_flag = reader.ReadBit();
        cc.subsampling_x = 1;
        cc.subsampling_y = 1;
    } else if (cc.color_primaries == kColorPrimariesBt709 &&
               cc.transfer_characteristics == kTransferCharacteristicsSrgb &&
               cc.matrix_coefficients == kMatrixCoefficientsIdentity) {
        // sRGB 特例：强制 4:4:4 + full range，码流里不再出现 subsampling
        cc.subsampling_x = 0;
        cc.subsampling_y = 0;
        cc.full_range_flag = 1;
    } else {
        cc.full_range_flag = reader.ReadBit();
        switch (sh.profile) {
            case 0:
                cc.subsampling_x = 1;
                cc.subsampling_y = 1;                               // 4:2:0
                break;
            case 1:
                cc.subsampling_x = 0;
                cc.subsampling_y = 0;                               // 4:4:4
                break;
            case 2:
                if (hbd == 2) {
                    cc.subsampling_x = static_cast<int>(reader.ReadBit());
                    cc.subsampling_y = cc.subsampling_x
                                       ? static_cast<int>(reader.ReadBit()) : 0;
                } else {
                    cc.subsampling_x = 1;
                    cc.subsampling_y = 0;                           // 4:2:2
                }
                break;
            default:
                break;
        }
        cc.chroma_sample_position = (cc.subsampling_x && cc.subsampling_y)
                                    ? static_cast<int>(reader.ReadBits(2)) : 0;
    }

    if (!cc.monochrome) {
        cc.separate_uv_delta = reader.ReadBit();
    }

    cc.color_bit_depth = 8 + 2 * hbd;
    cc.luma_bit_depth = cc.color_bit_depth;
    cc.chroma_bit_depth = cc.color_bit_depth;
    cc.present = true;

    sh.input_bit_depth = cc.color_bit_depth;
    sh.bit_depth_minus_8 = cc.color_bit_depth - 8;

    return !reader.HasError();
}

model::Av1SequenceHeaderInfo Av1BitstreamParser::ParseFromObuUnit(const utils::ObuUnit& obu_unit) {
    model::Av1SequenceHeaderInfo sh;
    if (!IsSequenceHeaderObu(obu_unit) || obu_unit.data.empty()) {
        return sh;
    }

    utils::BitReader reader;
    reader.Reset(obu_unit.data.data(), obu_unit.data.size());

    sh.profile = static_cast<int>(reader.ReadBits(3));
    if (sh.profile > 2) {
        return model::Av1SequenceHeaderInfo();
    }
    sh.still_picture = reader.ReadBit();
    sh.reduced_still_picture_header = reader.ReadBit();

    if (sh.reduced_still_picture_header) {
        // still picture 分支：直接是 5 位 seq_level_idx（major 3 + minor 2）
        const int major = 2 + static_cast<int>(reader.ReadBits(3));
        const int minor = static_cast<int>(reader.ReadBits(2));
        sh.level = (major - 2) * 4 + minor;
        sh.tier = 0;
        sh.operating_points_count = 1;
    } else {
        if (!SkipTimingInfo(reader, sh)) return sh;
        if (!SkipOperatingPoints(reader, sh)) return sh;
    }

    const int width_bits = static_cast<int>(reader.ReadBits(4)) + 1;
    const int height_bits = static_cast<int>(reader.ReadBits(4)) + 1;
    sh.frame_width_minus_1 = static_cast<int>(reader.ReadBits(width_bits));
    sh.frame_height_minus_1 = static_cast<int>(reader.ReadBits(height_bits));

    if (!sh.reduced_still_picture_header) {
        const bool frame_id_numbers_present = reader.ReadBit();
        if (frame_id_numbers_present) {
            reader.SkipBits(4);                                     // delta_frame_id_length_minus_2
            reader.SkipBits(3);                                     // additional_frame_id_length_minus_1
        }
    }

    sh.use_128x128_superblock = reader.ReadBit();
    sh.enable_filter_intra = reader.ReadBit();
    sh.enable_intra_edge_filter = reader.ReadBit();

    if (sh.reduced_still_picture_header) {
        sh.seq_force_screen_content_tools = kSelectAdaptive;
        sh.seq_force_integer_mv = kSelectAdaptive;
    } else {
        sh.enable_interintra_compound = reader.ReadBit();
        sh.enable_masked_compound = reader.ReadBit();
        sh.enable_warped_motion = reader.ReadBit();
        sh.enable_dual_filter = reader.ReadBit();
        sh.enable_order_hint = reader.ReadBit();
        if (sh.enable_order_hint) {
            sh.enable_jnt_comp = reader.ReadBit();
            sh.enable_ref_frame_mvs = reader.ReadBit();
        }

        sh.seq_force_screen_content_tools =
            reader.ReadBit() ? kSelectAdaptive : static_cast<int>(reader.ReadBit());
        sh.seq_force_integer_mv = sh.seq_force_screen_content_tools
            ? (reader.ReadBit() ? kSelectAdaptive : static_cast<int>(reader.ReadBit()))
            : kSelectAdaptive;

        if (sh.enable_order_hint) {
            sh.order_hint_bits_minus_1 = static_cast<int>(reader.ReadBits(3));
        }
    }

    sh.enable_superres = reader.ReadBit();
    sh.enable_cdef = reader.ReadBit();
    sh.enable_restoration = reader.ReadBit();

    if (!ParseColorConfig(reader, sh)) {
        return model::Av1SequenceHeaderInfo();
    }

    sh.film_grain_params_present = reader.ReadBit();

    if (reader.HasError()) {
        return model::Av1SequenceHeaderInfo();
    }

    sh.present = true;
    return sh;
}

model::Av1SequenceHeaderInfo Av1BitstreamParser::ParseSequenceHeader(const uint8_t* extradata,
                                                                    size_t size) {
    if (extradata == nullptr || size == 0) {
        return model::Av1SequenceHeaderInfo();
    }

    // av1C 走 ParseAv1C（会顺带解析尾部 configOBUs），裸 OBU 流走 DetectFormat 的兜底。
    // 两条路最终都把 OBU 放进 obu_units。
    utils::ExtradataResult parsed = utils::ExtradataParser::Parse(extradata, size);

    for (const utils::ObuUnit& obu : parsed.obu_units) {
        if (IsSequenceHeaderObu(obu)) {
            return ParseFromObuUnit(obu);
        }
    }

    return model::Av1SequenceHeaderInfo();
}

} // namespace analyzer
} // namespace videoeye
