#include "core/analyzer/H264BitstreamParser.h"

#include "utils/BitReader.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

namespace {

// 解析中途失败时的兜底：保留已读到的字段，但不置 present
constexpr int kMaxSpsId = 31;

} // namespace

// --------------------------------------------------------------------------
// Profile / Level 名称
// --------------------------------------------------------------------------
std::string H264BitstreamParser::GetProfileName(int profile_idc) {
    switch (profile_idc) {
        case 66:  return "Baseline";
        case 77:  return "Main";
        case 88:  return "Extended";
        case 100: return "High";
        case 110: return "High 10";
        case 122: return "High 4:2:2";
        case 144: return "High 4:4:4";
        case 244: return "High 4:4:4 Predictive";
        case 118: return "Multiview High";
        case 128: return "Stereo High";
        default:  return "Unknown";
    }
}

std::string H264BitstreamParser::GetLevelVersion(int level_idc) {
    // level_idc 是「等级 × 10」：31 -> 3.1。9 是规范里唯一的非十进制特例（1b）。
    if (level_idc == 0) return "Undefined";
    if (level_idc == 9) return "1b";
    const int major = level_idc / 10;
    const int minor = level_idc % 10;
    if (major < 1 || major > 6) return "Unknown";
    return std::to_string(major) + "." + std::to_string(minor);
}

void H264BitstreamParser::ParseProfileLevelInfo(uint32_t profile_idc,
                                                uint32_t level_idc,
                                                int& profile,
                                                std::string& profile_name,
                                                int& level,
                                                std::string& level_version) {
    profile = static_cast<int>(profile_idc);
    level = static_cast<int>(level_idc);
    profile_name = GetProfileName(profile);
    level_version = GetLevelVersion(level);
}

// --------------------------------------------------------------------------
// NAL 类型判断
// --------------------------------------------------------------------------
bool H264BitstreamParser::IsSpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 7; // SPS
}

bool H264BitstreamParser::IsPpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 8; // PPS
}

bool H264BitstreamParser::HasChromaFormatExtension(int profile_idc) {
    // H.264 7.3.2.1.1：只有这些 profile 的 SPS 才带 chroma_format_idc /
    // bit_depth_* / qpprime_y_zero_transform_bypass_flag 等扩展字段。
    switch (profile_idc) {
        case 100: case 110: case 122: case 244:
        case 44:  case 83:  case 86:  case 118:
        case 128: case 138: case 139: case 134:
        case 135:
            return true;
        default:
            return false;
    }
}

std::vector<uint8_t> H264BitstreamParser::GetRbsp(const utils::NalUnit& nal_unit) {
    // NalUnit::data 已不含 NAL header（见 utils/ExtradataParser.h 的不变量说明），
    // 这里只需要做 emulation_prevention_three_byte 的反转义。
    if (nal_unit.data.empty()) {
        return {};
    }
    return utils::UnescapeRbsp(nal_unit.data.data(), nal_unit.data.size());
}

// --------------------------------------------------------------------------
// scaling_list( sizeOfScalingList ) —— 7.3.2.1.1.1
// 我们只需要跳过它，但仍要正确跟踪 nextScale，否则 delta_scale 的取舍会错位。
// --------------------------------------------------------------------------
void H264BitstreamParser::SkipScalingList(utils::BitReader& reader, int size) {
    int last_scale = 8;
    int next_scale = 8;
    for (int j = 0; j < size; ++j) {
        if (next_scale != 0) {
            const int32_t delta_scale = reader.ReadSE();
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

// --------------------------------------------------------------------------
// hrd_parameters() —— 7.3.2.1.2 / 附录 E.1.2
// --------------------------------------------------------------------------
void H264BitstreamParser::SkipHrdParameters(utils::BitReader& reader,
                                            model::H264VuiInfo& vui) {
    const uint32_t cpb_cnt_minus1 = reader.ReadUE();
    if (cpb_cnt_minus1 > 31) {
        return; // 非法，交给调用方按 HasError() 处理
    }

    vui.cpb_cnt = static_cast<uint8_t>(cpb_cnt_minus1 + 1);
    vui.bit_rate_scale = reader.ReadBits(4);
    vui.cpb_size_scale = reader.ReadBits(4);

    for (uint32_t i = 0; i <= cpb_cnt_minus1; ++i) {
        const uint32_t bit_rate_value_minus1 = reader.ReadUE();
        const uint32_t cpb_size_value_minus1 = reader.ReadUE();
        reader.ReadBit(); // cbr_flag[i]

        if (i == 0) {
            vui.bit_rate_value = bit_rate_value_minus1 + 1;
            vui.crb_size_value = cpb_size_value_minus1 + 1;
        }
    }

    reader.SkipBits(5); // initial_cpb_removal_delay_length_minus1
    reader.SkipBits(5); // cpb_removal_delay_length_minus1
    vui.dpb_delay_value = reader.ReadBits(5) + 1; // dpb_output_delay_length_minus1
    reader.SkipBits(5); // time_offset_length
}

// --------------------------------------------------------------------------
// vui_parameters() —— 附录 E.1.1
// --------------------------------------------------------------------------
void H264BitstreamParser::ParseVuiParameters(utils::BitReader& reader,
                                             model::H264VuiInfo& vui) {
    vui.present = true;

    // --- aspect_ratio_info ---
    vui.aspect_ratio_info_present = reader.ReadBit();
    if (vui.aspect_ratio_info_present) {
        vui.aspect_ratio_idc = static_cast<int>(reader.ReadBits(8));
        if (vui.aspect_ratio_idc == 255) { // Extended_SAR
            vui.sar_width = static_cast<int>(reader.ReadBits(16));
            vui.sar_height = static_cast<int>(reader.ReadBits(16));
        }
    }

    // --- overscan_info ---
    vui.overscan_info_present = reader.ReadBit();
    if (vui.overscan_info_present) {
        vui.overscan_appropriate_flag = reader.ReadBit();
    }

    // --- video_signal_type（颜色描述在这里）---
    vui.video_signal_type_present = reader.ReadBit();
    if (vui.video_signal_type_present) {
        vui.video_format = static_cast<int>(reader.ReadBits(3));
        vui.video_full_range_flag = reader.ReadBit();
        vui.colour_description_present = reader.ReadBit();
        if (vui.colour_description_present) {
            vui.color_primaries = static_cast<int>(reader.ReadBits(8));
            vui.transfer_characteristics = static_cast<int>(reader.ReadBits(8));
            vui.matrix_coefficients = static_cast<int>(reader.ReadBits(8));
        }
    }
    vui.color_range = vui.video_full_range_flag ? 1 : 0;

    // --- chroma_loc_info ---
    vui.chroma_loc_info_present = reader.ReadBit();
    if (vui.chroma_loc_info_present) {
        vui.chroma_sample_loc_type_top_field = static_cast<int>(reader.ReadUE());
        vui.chroma_sample_loc_type_bottom_field = static_cast<int>(reader.ReadUE());
    }

    // --- timing_info ---
    vui.timing_info_present = reader.ReadBit();
    if (vui.timing_info_present) {
        vui.num_units_in_tick = reader.ReadBits(32);
        vui.time_scale = reader.ReadBits(32);
        vui.fixed_frame_rate = reader.ReadBit();
    }

    // --- HRD（NAL 与 VCL 两套可各自存在）---
    vui.nal_hrd_parameters_present = reader.ReadBit();
    if (vui.nal_hrd_parameters_present) {
        SkipHrdParameters(reader, vui);
    }
    vui.vcl_hrd_parameters_present = reader.ReadBit();
    if (vui.vcl_hrd_parameters_present) {
        SkipHrdParameters(reader, vui);
    }
    if (vui.nal_hrd_parameters_present || vui.vcl_hrd_parameters_present) {
        vui.low_delay_hrd_flag = reader.ReadBit();
    }
    vui.hrd_present = vui.nal_hrd_parameters_present || vui.vcl_hrd_parameters_present;

    // --- pic_struct ---
    vui.pic_struct_present_flag = reader.ReadBit();

    // --- bitstream_restriction ---
    vui.bitstream_restriction_flag = reader.ReadBit();
    if (vui.bitstream_restriction_flag) {
        vui.motion_vectors_over_pic_boundaries_flag = reader.ReadBit();
        vui.max_bytes_per_pic_denom = static_cast<int>(reader.ReadUE());
        vui.max_bits_per_mb_denom = static_cast<int>(reader.ReadUE());
        vui.log2_max_mv_length_horizontal = static_cast<int>(reader.ReadUE());
        vui.log2_max_mv_length_vertical = static_cast<int>(reader.ReadUE());
        vui.max_num_reorder_frames = static_cast<int>(reader.ReadUE());
        vui.max_dec_frame_buffering = static_cast<int>(reader.ReadUE());
    }
}

// --------------------------------------------------------------------------
// seq_parameter_set_data() —— 7.3.2.1.1
// --------------------------------------------------------------------------
model::H264SpsInfo H264BitstreamParser::ParseFromNalUnit(const utils::NalUnit& nal_unit) {
    model::H264SpsInfo sps;

    if (!IsSpsNalUnit(nal_unit)) {
        return sps;
    }

    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) {
        return sps;
    }

    utils::BitReader reader;
    reader.Reset(rbsp.data(), rbsp.size());

    // profile_idc u(8)
    sps.profile_idc = static_cast<int>(reader.ReadBits(8));

    // constraint_set0..5 u(1) × 6 + reserved_zero_2bits u(2)
    for (int i = 0; i < 6; ++i) {
        if (reader.ReadBit()) {
            sps.constraint_flags |= (1 << i);
        }
    }
    reader.SkipBits(2);

    // level_idc u(8)
    sps.level_idc = static_cast<int>(reader.ReadBits(8));

    // seq_parameter_set_id ue(v)
    sps.seq_parameter_set_id = static_cast<int>(reader.ReadUE());
    if (sps.seq_parameter_set_id > kMaxSpsId) {
        return sps; // 非法 SPS
    }

    if (HasChromaFormatExtension(sps.profile_idc)) {
        sps.chroma_format_idc = static_cast<int>(reader.ReadUE());
        if (sps.chroma_format_idc > 3) {
            return sps;
        }
        if (sps.chroma_format_idc == 3) {
            sps.separate_colour_plane_flag = reader.ReadBit();
        }

        sps.bit_depth_luma_minus8 = static_cast<int>(reader.ReadUE());
        sps.bit_depth_chroma_minus8 = static_cast<int>(reader.ReadUE());
        if (sps.bit_depth_luma_minus8 > 6 || sps.bit_depth_chroma_minus8 > 6) {
            return sps; // 规范上限 14 bit
        }

        sps.qpprime_y_zero_transform_bypass_flag = static_cast<int>(reader.ReadBit());

        sps.seq_scaling_matrix_present_flag = static_cast<int>(reader.ReadBit());
        if (sps.seq_scaling_matrix_present_flag) {
            const int list_count = (sps.chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < list_count; ++i) {
                if (reader.ReadBit()) { // seq_scaling_list_present_flag[i]
                    SkipScalingList(reader, (i < 6) ? 16 : 64);
                }
            }
        }
    } else {
        // 非 High 系 profile：规范隐含 4:2:0 / 8bit
        sps.chroma_format_idc = 1;
        sps.bit_depth_luma_minus8 = 0;
        sps.bit_depth_chroma_minus8 = 0;
    }

    // log2_max_frame_num_minus4 ue(v)
    sps.log2_max_frame_num_minus4 = static_cast<int>(reader.ReadUE());

    // pic_order_cnt_type ue(v)，决定后面有没有额外字段
    sps.pic_order_cnt_type = static_cast<int>(reader.ReadUE());
    if (sps.pic_order_cnt_type == 0) {
        sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(reader.ReadUE());
    } else if (sps.pic_order_cnt_type == 1) {
        reader.SkipBits(1); // delta_pic_order_always_zero_flag
        reader.ReadSE();    // offset_for_non_ref_pic
        reader.ReadSE();    // offset_for_top_to_bottom_field
        const uint32_t cycle = reader.ReadUE();
        for (uint32_t i = 0; i < cycle; ++i) {
            reader.ReadSE(); // offset_for_ref_frame[i]
        }
    }
    if (sps.pic_order_cnt_type > 2) {
        return sps;
    }

    // max_num_ref_frames ue(v)
    sps.max_num_ref_frames = static_cast<int>(reader.ReadUE());

    // gaps_in_frame_num_value_allowed_flag u(1)
    sps.gaps_in_frame_val_allowed_flag = static_cast<int>(reader.ReadBit());

    // pic_width_in_mbs_minus1 ue(v)
    sps.pic_width_in_mbs_minus1 = static_cast<int>(reader.ReadUE());

    // pic_height_in_map_units_minus1 ue(v)
    sps.pic_height_in_mbs_minus1 = static_cast<int>(reader.ReadUE());

    // frame_mbs_only_flag u(1)
    sps.frame_mbs_only_flag = reader.ReadBit();
    if (!sps.frame_mbs_only_flag) {
        sps.mb_adaptive_frame_field_flag = reader.ReadBit();
    }

    // direct_8x8_inference_flag u(1)
    sps.direct_8x8_inference_flag = reader.ReadBit();

    // frame_cropping_flag u(1) + 4 × ue(v)
    sps.frame_cropping_flag = static_cast<int>(reader.ReadBit());
    if (sps.frame_cropping_flag) {
        sps.frame_crop_left_offset = static_cast<int>(reader.ReadUE());
        sps.frame_crop_right_offset = static_cast<int>(reader.ReadUE());
        sps.frame_crop_top_offset = static_cast<int>(reader.ReadUE());
        sps.frame_crop_bottom_offset = static_cast<int>(reader.ReadUE());
    }

    // vui_parameters_present_flag u(1)
    const bool vui_present = reader.ReadBit();
    if (vui_present) {
        ParseVuiParameters(reader, sps.vui);
    }

    // VUI 里的 frame_mbs_only_flag 是历史遗留镜像字段，同步一下避免旧引用读到默认值
    sps.vui.frame_mbs_only_flag = sps.frame_mbs_only_flag;

    if (reader.HasError()) {
        // 码流被截断：已读字段可能不完整，不标记为有效
        return model::H264SpsInfo();
    }

    sps.present = true;
    return sps;
}

// --------------------------------------------------------------------------
// pic_parameter_set_rbsp() —— 7.3.2.2
// --------------------------------------------------------------------------
model::H264PpsInfo H264BitstreamParser::ParsePpsFromNalUnit(const utils::NalUnit& nal_unit) {
    model::H264PpsInfo pps;

    if (!IsPpsNalUnit(nal_unit)) {
        return pps;
    }

    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) {
        return pps;
    }

    utils::BitReader reader;
    reader.Reset(rbsp.data(), rbsp.size());

    pps.pic_parameter_set_id = static_cast<int>(reader.ReadUE());
    pps.seq_parameter_set_id = static_cast<int>(reader.ReadUE());

    reader.SkipBits(1); // entropy_coding_mode_flag
    reader.SkipBits(1); // bottom_field_pic_order_in_frame_present_flag

    const uint32_t num_slice_groups_minus1 = reader.ReadUE();
    if (num_slice_groups_minus1 > 0) {
        const uint32_t map_type = reader.ReadUE();
        if (map_type == 0) {
            for (uint32_t i = 0; i <= num_slice_groups_minus1; ++i) {
                reader.ReadUE(); // run_length_minus1[i]
            }
        } else if (map_type == 2) {
            for (uint32_t i = 0; i < num_slice_groups_minus1; ++i) {
                reader.ReadUE(); // top_left[i]
                reader.ReadUE(); // bottom_right[i]
            }
        } else if (map_type >= 3 && map_type <= 5) {
            reader.SkipBits(1); // slice_group_change_direction_flag
            reader.ReadUE();    // slice_group_change_rate_minus1
        } else if (map_type == 6) {
            const uint32_t pic_size_in_map_units_minus1 = reader.ReadUE();
            int bits = 0;
            uint32_t groups = num_slice_groups_minus1 + 1;
            while ((1u << bits) < groups) {
                ++bits;
            }
            for (uint32_t i = 0; i <= pic_size_in_map_units_minus1; ++i) {
                reader.ReadBits(bits); // slice_group_id[i]
            }
        }
    }

    pps.num_ref_idx_l0_default_active_minus1 = static_cast<int>(reader.ReadUE());
    pps.num_ref_idx_l1_default_active_minus1 = static_cast<int>(reader.ReadUE());
    pps.weighted_pred_flag = reader.ReadBit();
    pps.weighted_bipred_idc = static_cast<int>(reader.ReadBits(2));
    pps.pic_init_qp_minus26 = reader.ReadSE();
    pps.pic_init_slq_minus26 = reader.ReadSE();
    reader.ReadSE(); // chroma_qp_index_offset
    pps.deblocking_filter_control_present_flag = reader.ReadBit();
    pps.constrained_intra_pred_flag = reader.ReadBit();
    pps.redundant_pic_cnt_present_flag = static_cast<int>(reader.ReadBit());

    if (reader.HasError()) {
        return model::H264PpsInfo();
    }

    pps.present = true;
    return pps;
}

// --------------------------------------------------------------------------
// extradata 入口
// --------------------------------------------------------------------------
model::H264SpsInfo H264BitstreamParser::ParseSpf(const uint8_t* extradata, size_t size) {
    if (!extradata || size == 0) {
        return model::H264SpsInfo();
    }

    auto result = utils::ExtradataParser::Parse(extradata, size);

    for (const auto& nal : result.nal_units) {
        if (IsSpsNalUnit(nal)) {
            return ParseFromNalUnit(nal);
        }
    }

    return model::H264SpsInfo();
}

model::H264PpsInfo H264BitstreamParser::ParsePps(const uint8_t* extradata, size_t size) {
    if (!extradata || size == 0) {
        return model::H264PpsInfo();
    }

    auto result = utils::ExtradataParser::Parse(extradata, size);

    for (const auto& nal : result.nal_units) {
        if (IsPpsNalUnit(nal)) {
            return ParsePpsFromNalUnit(nal);
        }
    }

    return model::H264PpsInfo();
}

model::H264VuiInfo H264BitstreamParser::ParseVui(const uint8_t* extradata, size_t size) {
    return ParseSpf(extradata, size).vui;
}

} // namespace analyzer
} // namespace videoeye
