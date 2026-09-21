#include "core/analyzer/HevcBitstreamParser.h"

#include "utils/BitReader.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

// --------------------------------------------------------------------------
// Profile / Tier / Level
// --------------------------------------------------------------------------
std::string HevcBitstreamParser::GetProfileName(int general_profile_idc) {
    switch (general_profile_idc) {
        case 0:  return "Main";
        case 1:  return "Main 10";
        case 2:  return "Main Still Picture";
        case 3:  return "Main Rext";
        case 4:  return "High Throughput";
        case 5:  return "Multiview Main";
        case 6:  return "Scalable Main";
        case 7:  return "3D Main";
        case 8:  return "Screen Content Coding";
        case 9:  return "Scalable Range Extension";
        default: return "Unknown";
    }
}

std::string HevcBitstreamParser::GetTierName(int general_tier_flag) {
    return general_tier_flag ? "High" : "Main";
}

std::string HevcBitstreamParser::GetLevelVersion(uint32_t general_level_idc) {
    // general_level_idc = 30 × 主版本 + 3 × 次版本：153 -> 5.1，123 -> 4.1
    if (general_level_idc == 0) return "Undefined";
    const uint32_t level = general_level_idc / 30;
    const uint32_t sub_level = (general_level_idc % 30) / 3;
    if (level == 0 || level > 13) return "Unknown";
    return std::to_string(level) + "." + std::to_string(sub_level);
}

bool HevcBitstreamParser::IsVpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 32;
}

bool HevcBitstreamParser::IsSpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 33;
}

bool HevcBitstreamParser::IsPpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 34;
}

std::vector<uint8_t> HevcBitstreamParser::GetRbsp(const utils::NalUnit& nal_unit) {
    if (nal_unit.data.empty()) {
        return {};
    }
    return utils::UnescapeRbsp(nal_unit.data.data(), nal_unit.data.size());
}

// --------------------------------------------------------------------------
// profile_tier_level() —— 7.3.3
// --------------------------------------------------------------------------
void HevcBitstreamParser::ParseProfileTierLevel(utils::BitReader& reader,
                                                int max_sub_layers_minus1,
                                                int& profile_space,
                                                int& tier_flag,
                                                int& profile_idc,
                                                uint32_t& level_idc,
                                                std::vector<bool>& sub_layer_profile_present,
                                                std::vector<bool>& sub_layer_level_present) {
    bool compat[32] = {};

    profile_space = static_cast<int>(reader.ReadBits(2));
    tier_flag = static_cast<int>(reader.ReadBit());
    profile_idc = static_cast<int>(reader.ReadBits(5));
    for (int j = 0; j < 32; ++j) {
        compat[j] = reader.ReadBit();
    }

    // 6 个固定约束位（progressive / interlaced / non-packed / frame-only 之后，
    // 是否还有扩展约束位取决于 profile 是否属于 Rext 系）
    reader.SkipBits(1); // general_progressive_source_flag
    reader.SkipBits(1); // general_interlaced_source_flag
    reader.SkipBits(1); // general_non_packed_constraint_flag
    reader.SkipBits(1); // general_frame_only_constraint_flag

    const bool is_rext_family =
        profile_idc == 4 || compat[4] ||
        profile_idc == 5 || compat[5] ||
        profile_idc == 6 || compat[6] ||
        profile_idc == 7 || compat[7] ||
        profile_idc == 8 || compat[8] ||
        profile_idc == 9 || compat[9] ||
        profile_idc == 10 || compat[10];

    if (is_rext_family) {
        reader.SkipBits(1); // general_max_12bit_constraint_flag
        reader.SkipBits(1); // general_max_10bit_constraint_flag
        reader.SkipBits(1); // general_max_8bit_constraint_flag
        reader.SkipBits(1); // general_max_422chroma_constraint_flag
        reader.SkipBits(1); // general_max_420chroma_constraint_flag
        reader.SkipBits(1); // general_max_monochrome_constraint_flag
        reader.SkipBits(1); // general_intra_constraint_flag
        reader.SkipBits(1); // general_one_picture_only_constraint_flag
        reader.SkipBits(1); // general_lower_bit_rate_constraint_flag

        const bool has_14bit =
            profile_idc == 5 || compat[5] ||
            profile_idc == 9 || compat[9] ||
            profile_idc == 10 || compat[10];
        if (has_14bit) {
            reader.SkipBits(1);  // general_max_14bit_constraint_flag
            reader.SkipBits(33); // general_reserved_zero_33bits
        } else {
            reader.SkipBits(34); // general_reserved_zero_34bits
        }
    } else {
        reader.SkipBits(43); // general_reserved_zero_43bits
    }

    const bool inbld_family =
        profile_idc == 1 || compat[1] ||
        profile_idc == 2 || compat[2] ||
        profile_idc == 3 || compat[3] ||
        profile_idc == 4 || compat[4];
    if (inbld_family) {
        reader.SkipBits(1); // general_inbld_flag
    } else {
        reader.SkipBits(1); // general_reserved_zero_bit
    }

    level_idc = reader.ReadBits(8);

    sub_layer_profile_present.assign(max_sub_layers_minus1, false);
    sub_layer_level_present.assign(max_sub_layers_minus1, false);
    for (int j = 0; j < max_sub_layers_minus1; ++j) {
        sub_layer_profile_present[j] = reader.ReadBit();
        sub_layer_level_present[j] = reader.ReadBit();
    }
    if (max_sub_layers_minus1 > 0) {
        for (int j = max_sub_layers_minus1; j < 8; ++j) {
            reader.SkipBits(2); // reserved_zero_2bits
        }
    }

    for (int j = 0; j < max_sub_layers_minus1; ++j) {
        if (sub_layer_profile_present[j]) {
            reader.SkipBits(2);  // sub_layer_profile_space
            reader.SkipBits(1);  // sub_layer_tier_flag
            reader.SkipBits(5);  // sub_layer_profile_idc
            reader.SkipBits(32); // sub_layer_profile_compatibility_flag[32]
            reader.SkipBits(1);  // progressive
            reader.SkipBits(1);  // interlaced
            reader.SkipBits(1);  // non-packed
            reader.SkipBits(1);  // frame-only
            reader.SkipBits(43); // sub_layer 约束位 + reserved（Rext 结构同上，简化处理）
        }
        if (sub_layer_level_present[j]) {
            reader.SkipBits(8); // sub_layer_level_idc[j]
        }
    }
}

// --------------------------------------------------------------------------
// scaling_list_data() —— 7.3.4
// --------------------------------------------------------------------------
void HevcBitstreamParser::SkipScalingListData(utils::BitReader& reader) {
    for (int size_id = 0; size_id < 4; ++size_id) {
        for (int matrix_id = 0; matrix_id < 6; matrix_id += (size_id == 3 ? 3 : 1)) {
            const bool pred_mode = reader.ReadBit(); // scaling_list_pred_mode_flag
            if (!pred_mode) {
                reader.ReadUE(); // scaling_list_pred_matrix_id_delta
            } else {
                const int coef_num = (1 << (4 + (size_id << 1))) > 64
                                         ? 64
                                         : (1 << (4 + (size_id << 1)));
                if (size_id > 1) {
                    reader.ReadSE(); // scaling_list_dc_coef_minus8
                }
                for (int i = 0; i < coef_num; ++i) {
                    reader.ReadSE(); // scaling_list_delta_coef
                }
            }
        }
    }
}

// --------------------------------------------------------------------------
// hrd_parameters() —— 7.3.5（附录 E.2.2）
// --------------------------------------------------------------------------
void HevcBitstreamParser::SkipHrdParameters(utils::BitReader& reader,
                                            int max_sub_layers_minus1) {
    const bool nal_hrd = reader.ReadBit();
    const bool vcl_hrd = reader.ReadBit();
    if (nal_hrd || vcl_hrd) {
        reader.SkipBits(1); // sub_pic_hrd_params_present_flag
        if (reader.HasError()) return;
        reader.SkipBits(4); // tick_divisor_minus2
        reader.SkipBits(5); // du_cpb_removal_delay_increment_length_minus1
        reader.SkipBits(1); // sub_pic_cpb_params_in_pic_timing_sei_flag
        reader.SkipBits(5); // dpb_output_delay_du_length_minus1
        reader.SkipBits(5); // bit_rate_scale
        reader.SkipBits(5); // cpb_size_scale
        if (reader.HasError()) return;

        const bool sub_pic = false; // 上面已跳过，简化为不展开
        const uint32_t cpb_cnt = sub_pic ? 0u : 1u;
        for (int i = 0; i <= max_sub_layers_minus1; ++i) {
            const bool fixed_rate = reader.ReadBit();
            (void)fixed_rate;
            reader.SkipBits(1); // nal_hrd_parameters_present_flag(占位，实际 fixed_pic_rate_within_cvs_flag 后)
            if (reader.HasError()) return;
            if (!fixed_rate) {
                reader.SkipBits(1); // elemental_duration_in_tc_minus1 前的 low_delay_hrd_flag
            } else {
                reader.SkipBits(1);
            }
            for (uint32_t j = 0; j < cpb_cnt; ++j) {
                reader.ReadUE(); // bit_rate_value_minus1
                reader.ReadUE(); // cpb_size_value_minus1
                if (!sub_pic) {
                    reader.ReadUE(); // cpb_size_du_value_minus1
                }
                reader.SkipBits(1); // cbr_flag
            }
        }
    }
}

// --------------------------------------------------------------------------
// short_term_ref_pic_set() —— 7.3.7
// --------------------------------------------------------------------------
void HevcBitstreamParser::SkipShortTermRefPicSets(utils::BitReader& reader, int num_sets) {
    std::vector<int> num_delta_pocs(num_sets > 0 ? num_sets : 1, 0);

    for (int i = 0; i < num_sets; ++i) {
        bool predicted = false;
        if (i != 0) {
            predicted = reader.ReadBit(); // inter_ref_pic_set_prediction_flag
        }

        if (predicted) {
            // SPS 里 delta_idx_minus1 恒为 0（i 永远 != num_short_term_ref_pic_sets）
            reader.SkipBits(1); // delta_rps_sign
            reader.ReadUE();    // abs_delta_rps_minus1
            const int ref_num = num_delta_pocs[static_cast<size_t>(i - 1)];
            for (int j = 0; j <= ref_num; ++j) {
                const bool used = reader.ReadBit();      // used_by_curr_pic_flag[j]
                if (!used) {
                    reader.SkipBits(1);                  // use_delta_flag[j]
                }
            }
            // 近似：NumDeltaPocs[i] = NumDeltaPocs[ref]（全部 used_by_curr_pic_flag=1 时精确）
            num_delta_pocs[static_cast<size_t>(i)] = ref_num;
        } else {
            const uint32_t num_negative = reader.ReadUE();
            const uint32_t num_positive = reader.ReadUE();
            for (uint32_t j = 0; j < num_negative; ++j) {
                reader.ReadUE(); // delta_poc_s0_minus1[j]
                reader.SkipBits(1); // used_by_curr_pic_s0_flag[j]
            }
            for (uint32_t j = 0; j < num_positive; ++j) {
                reader.ReadUE(); // delta_poc_s1_minus1[j]
                reader.SkipBits(1); // used_by_curr_pic_s1_flag[j]
            }
            num_delta_pocs[static_cast<size_t>(i)] =
                static_cast<int>(num_negative + num_positive);
        }
    }
}

// --------------------------------------------------------------------------
// vui_parameters() —— 附录 E.2.1
// --------------------------------------------------------------------------
void HevcBitstreamParser::ParseVuiParameters(utils::BitReader& reader,
                                             model::HevcSpsInfo& sps,
                                             int max_sub_layers_minus1) {
    sps.aspect_ratio_info_present_flag = static_cast<int>(reader.ReadBit());
    if (sps.aspect_ratio_info_present_flag) {
        sps.aspect_ratio_idc = static_cast<int>(reader.ReadBits(8));
        if (sps.aspect_ratio_idc == 255) { // Extended_SAR
            sps.sar_width = static_cast<int>(reader.ReadBits(16));
            sps.sar_height = static_cast<int>(reader.ReadBits(16));
        }
    }

    const bool overscan_info_present = reader.ReadBit();
    if (overscan_info_present) {
        reader.SkipBits(1); // overscan_appropriate_flag
    }

    const bool video_signal_type_present = reader.ReadBit();
    if (video_signal_type_present) {
        reader.SkipBits(3); // video_format
        sps.video_full_range_flag = static_cast<int>(reader.ReadBit());
        const bool colour_description_present = reader.ReadBit();
        if (colour_description_present) {
            sps.colour_primaries = static_cast<int>(reader.ReadBits(8));
            sps.transfer_characteristics = static_cast<int>(reader.ReadBits(8));
            sps.matrix_coefficients = static_cast<int>(reader.ReadBits(8));
        }
    }

    const bool chroma_loc_info_present = reader.ReadBit();
    if (chroma_loc_info_present) {
        sps.chroma_sample_loc_type_top_field = static_cast<int>(reader.ReadUE());
        sps.chroma_sample_loc_type_bottom_field = static_cast<int>(reader.ReadUE());
    }

    sps.neutral_chroma_indication_flag = reader.ReadBit();
    sps.field_seq_flag = reader.ReadBit();
    sps.frame_field_info_present_flag = reader.ReadBit();

    const bool default_display_window_flag = reader.ReadBit();
    if (default_display_window_flag) {
        sps.def_disp_win_left_offset = static_cast<int>(reader.ReadUE());
        sps.def_disp_win_right_offset = static_cast<int>(reader.ReadUE());
        sps.def_disp_win_top_offset = static_cast<int>(reader.ReadUE());
        sps.def_disp_win_bottom_offset = static_cast<int>(reader.ReadUE());
    }

    sps.vui_timing_info_present_flag = static_cast<int>(reader.ReadBit());
    if (sps.vui_timing_info_present_flag) {
        sps.vui_num_units_in_tick = reader.ReadBits(32);
        sps.vui_time_scale = reader.ReadBits(32);
        sps.vui_poc_proportional_to_timing_flag = reader.ReadBit();
        if (sps.vui_poc_proportional_to_timing_flag) {
            reader.ReadUE(); // vui_num_ticks_poc_diff_one_minus1
        }
        sps.vui_hrd_parameters_present_flag = reader.ReadBit();
        if (sps.vui_hrd_parameters_present_flag) {
            SkipHrdParameters(reader, max_sub_layers_minus1);
        }
    }

    sps.bitstream_restriction_flag = static_cast<int>(reader.ReadBit());
    if (sps.bitstream_restriction_flag) {
        reader.SkipBits(1); // tiles_fixed_structure_flag
        reader.SkipBits(1); // motion_vectors_over_pic_boundaries_flag
        reader.SkipBits(1); // restricted_ref_pic_lists_flag
        sps.min_spatial_segmentation_idc = static_cast<int>(reader.ReadUE());
        sps.max_bytes_per_pic_denom = static_cast<int>(reader.ReadUE());
        sps.max_bits_per_min_cu_denom = static_cast<int>(reader.ReadUE());
        sps.log2_max_mv_length_horizontal = static_cast<int>(reader.ReadUE());
        sps.log2_max_mv_length_vertical = static_cast<int>(reader.ReadUE());
    }
}

// --------------------------------------------------------------------------
// VPS —— 7.3.2.2
// --------------------------------------------------------------------------
model::HevcVpsInfo HevcBitstreamParser::ParseVpsFromNalUnit(const utils::NalUnit& nal_unit) {
    model::HevcVpsInfo vps;

    if (!IsVpsNalUnit(nal_unit)) {
        return vps;
    }

    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) {
        return vps;
    }

    utils::BitReader reader;
    reader.ResetFromData(rbsp.data(), rbsp.size());

    vps.vps_video_parameter_set_id = static_cast<int>(reader.ReadBits(4));
    reader.SkipBits(2); // vps_reserved_three_2bits
    const uint32_t max_layers_minus1 = reader.ReadBits(6);
    const uint32_t max_sub_layers_minus1 = reader.ReadBits(3);
    vps.vps_max_layers = static_cast<int>(max_layers_minus1) + 1;
    vps.vps_max_sub_layers = static_cast<int>(max_sub_layers_minus1) + 1;
    vps.vps_temporal_id_nesting_flag = static_cast<int>(reader.ReadBit());
    vps.vps_temporal_nal_layer_only_flag = vps.vps_temporal_id_nesting_flag;
    reader.SkipBits(16); // vps_reserved_0xffff_16bits

    ParseProfileTierLevel(reader, static_cast<int>(max_sub_layers_minus1),
                          vps.general_profile_space, vps.general_tier_flag,
                          vps.general_profile_idc, vps.general_level_idc,
                          vps.sub_layer_profile_present_flags,
                          vps.sub_layer_level_present_flags);

    vps.vps_sub_layer_ordering_info_present_flag = reader.ReadBit();
    const int start = vps.vps_sub_layer_ordering_info_present_flag
                          ? 0
                          : static_cast<int>(max_sub_layers_minus1);
    for (int i = start; i <= static_cast<int>(max_sub_layers_minus1); ++i) {
        reader.ReadUE(); // vps_max_dec_pic_buffering_minus1[i]
        reader.ReadUE(); // vps_max_num_reorder_pics[i]
        reader.ReadUE(); // vps_max_latency_increase_plus1[i]
    }

    vps.vps_max_layer_id = static_cast<int>(reader.ReadBits(6));
    const uint32_t num_layer_sets_minus1 = reader.ReadUE();
    for (uint32_t i = 1; i <= num_layer_sets_minus1; ++i) {
        for (int j = 0; j <= vps.vps_max_layer_id; ++j) {
            reader.SkipBits(1); // layer_id_included_flag[i][j]
        }
    }

    vps.vps_timing_info_present_flag = reader.ReadBit();
    if (vps.vps_timing_info_present_flag) {
        vps.vps_num_units_in_tick = reader.ReadBits(32);
        vps.vps_time_scale = reader.ReadBits(32);
        vps.vps_poc_proportional_to_timestamp_flag = reader.ReadBit();
        if (vps.vps_poc_proportional_to_timestamp_flag) {
            reader.ReadUE(); // vps_num_ticks_poc_diff_one_minus1
        }
        const uint32_t num_hrd = reader.ReadUE();
        // 到这里已经拿到了我们关心的全部字段，HRD 部分结构复杂且与上层展示无关，
        // 直接停止解析（后续字节不读不会被判定为错误）。
        (void)num_hrd;
    }

    if (reader.HasError()) {
        return model::HevcVpsInfo();
    }

    vps.present = true;
    return vps;
}

// --------------------------------------------------------------------------
// SPS —— 7.3.2.2.1
// --------------------------------------------------------------------------
model::HevcSpsInfo HevcBitstreamParser::ParseSpfFromNalUnit(const utils::NalUnit& nal_unit) {
    model::HevcSpsInfo sps;

    if (!IsSpsNalUnit(nal_unit)) {
        return sps;
    }

    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) {
        return sps;
    }

    utils::BitReader reader;
    reader.ResetFromData(rbsp.data(), rbsp.size());

    sps.sps_video_parameter_set_id = static_cast<int>(reader.ReadBits(4));
    sps.sps_max_sub_layers_minus1 = static_cast<int>(reader.ReadBits(3));
    sps.sps_temporal_id_nesting_flag = static_cast<int>(reader.ReadBit());

    ParseProfileTierLevel(reader, sps.sps_max_sub_layers_minus1,
                          sps.general_profile_space, sps.general_tier_flag,
                          sps.general_profile_idc, sps.general_level_idc,
                          sps.sub_layer_profile_present_flags,
                          sps.sub_layer_level_present_flags);

    sps.sps_seq_parameter_set_id = static_cast<int>(reader.ReadUE());
    if (sps.sps_seq_parameter_set_id > 15) {
        return sps;
    }

    sps.chroma_format_idc = static_cast<int>(reader.ReadUE());
    if (sps.chroma_format_idc > 3) {
        return sps;
    }
    if (sps.chroma_format_idc == 3) {
        sps.separate_colour_plane_flag = static_cast<int>(reader.ReadBit());
    }

    sps.pic_width_in_luma_samples = static_cast<int>(reader.ReadUE());
    sps.pic_height_in_luma_samples = static_cast<int>(reader.ReadUE());

    sps.conformance_window_flag = static_cast<int>(reader.ReadBit());
    if (sps.conformance_window_flag) {
        sps.conf_win_left_offset = static_cast<int>(reader.ReadUE());
        sps.conf_win_right_offset = static_cast<int>(reader.ReadUE());
        sps.conf_win_top_offset = static_cast<int>(reader.ReadUE());
        sps.conf_win_bottom_offset = static_cast<int>(reader.ReadUE());
    }

    sps.bit_depth_luma_minus8 = static_cast<int>(reader.ReadUE());
    sps.bit_depth_chroma_minus8 = static_cast<int>(reader.ReadUE());
    if (sps.bit_depth_luma_minus8 > 6 || sps.bit_depth_chroma_minus8 > 6) {
        return sps;
    }

    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(reader.ReadUE());

    sps.sps_sub_layer_ordering_info_present_flag = reader.ReadBit();
    const int start = sps.sps_sub_layer_ordering_info_present_flag
                          ? 0
                          : sps.sps_max_sub_layers_minus1;
    for (int i = start; i <= sps.sps_max_sub_layers_minus1; ++i) {
        reader.ReadUE(); // sps_max_dec_pic_buffering_minus1[i]
        reader.ReadUE(); // sps_max_num_reorder_pics[i]
        reader.ReadUE(); // sps_max_latency_increase_plus1[i]
    }

    sps.log2_min_luma_coding_block_size_minus3 = static_cast<int>(reader.ReadUE());
    sps.log2_diff_max_min_luma_coding_block_size = static_cast<int>(reader.ReadUE());
    sps.log2_min_luma_transform_block_size_minus2 = static_cast<int>(reader.ReadUE());
    sps.log2_diff_max_min_luma_transform_block_size = static_cast<int>(reader.ReadUE());
    sps.max_transform_hierarchy_depth_inter = static_cast<int>(reader.ReadUE());
    sps.max_transform_hierarchy_depth_intra = static_cast<int>(reader.ReadUE());

    sps.scaling_list_enable_flag = static_cast<int>(reader.ReadBit());
    if (sps.scaling_list_enable_flag) {
        if (reader.ReadBit()) { // sps_scaling_list_data_present_flag
            SkipScalingListData(reader);
        }
    }

    sps.amp_enabled_flag = static_cast<int>(reader.ReadBit());
    sps.sample_adaptive_offset_enabled_flag = static_cast<int>(reader.ReadBit());

    sps.pcm_enabled_flag = static_cast<int>(reader.ReadBit());
    if (sps.pcm_enabled_flag) {
        sps.pcm_sample_bit_depth_luma_minus1 = static_cast<int>(reader.ReadBits(4));
        sps.pcm_sample_bit_depth_chroma_minus1 = static_cast<int>(reader.ReadBits(4));
        sps.log2_min_pcm_luma_coding_block_size = static_cast<int>(reader.ReadUE());
        sps.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<int>(reader.ReadUE());
        reader.SkipBits(1); // pcm_loop_filter_disabled_flag
    }

    sps.num_short_term_ref_pic_sets = static_cast<int>(reader.ReadUE());
    SkipShortTermRefPicSets(reader, sps.num_short_term_ref_pic_sets);

    sps.long_term_ref_pics_present_flag = static_cast<int>(reader.ReadBit());
    if (sps.long_term_ref_pics_present_flag) {
        sps.num_long_term_ref_pics_sps = static_cast<int>(reader.ReadUE());
        const int bits = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
        for (int i = 0; i < sps.num_long_term_ref_pics_sps; ++i) {
            reader.ReadBits(bits); // lt_ref_pic_poc_lsb_sps[i]
            reader.SkipBits(1);    // used_by_curr_pic_lt_sps_flag[i]
        }
    }

    sps.temporal_mvp_enabled_flag = static_cast<int>(reader.ReadBit());
    sps.strongly_smoothed_img_between_grids_flag = static_cast<int>(reader.ReadBit());

    sps.vui_parameters_present_flag = static_cast<int>(reader.ReadBit());
    if (sps.vui_parameters_present_flag) {
        ParseVuiParameters(reader, sps, sps.sps_max_sub_layers_minus1);
    }

    if (reader.HasError()) {
        return model::HevcSpsInfo();
    }

    // 顺带按 CTU 反推一份，供仍引用旧字段的地方使用
    const int ctu = 1 << (sps.log2_min_luma_coding_block_size_minus3 + 3 +
                          sps.log2_diff_max_min_luma_coding_block_size);
    if (ctu > 0) {
        sps.pic_width_in_ctu_minus1 = (sps.pic_width_in_luma_samples + ctu - 1) / ctu - 1;
        sps.pic_height_in_ctu_minus1 = (sps.pic_height_in_luma_samples + ctu - 1) / ctu - 1;
    }

    sps.present = true;
    return sps;
}

// --------------------------------------------------------------------------
// PPS —— 7.3.2.3.1
// --------------------------------------------------------------------------
model::HevcPpsInfo HevcBitstreamParser::ParsePpsFromNalUnit(const utils::NalUnit& nal_unit) {
    model::HevcPpsInfo pps;

    if (!IsPpsNalUnit(nal_unit)) {
        return pps;
    }

    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) {
        return pps;
    }

    utils::BitReader reader;
    reader.ResetFromData(rbsp.data(), rbsp.size());

    pps.pic_parameter_set_id = static_cast<int>(reader.ReadUE());
    pps.seq_parameter_set_id = static_cast<int>(reader.ReadUE());
    pps.dependent_slice_segments_enabled_flag = static_cast<int>(reader.ReadBit());
    pps.output_flag_present_flag = static_cast<int>(reader.ReadBit());
    pps.num_extra_slice_header_bits = static_cast<int>(reader.ReadBits(3));
    pps.sign_data_hiding_enabled_flag = static_cast<int>(reader.ReadBit());
    pps.cabac_init_present_flag = static_cast<int>(reader.ReadBit());
    pps.num_ref_idx_l0_default_active_minus1 = static_cast<int>(reader.ReadUE());
    pps.num_ref_idx_l1_default_active_minus1 = static_cast<int>(reader.ReadUE());
    pps.init_qp_minus26 = static_cast<int>(reader.ReadSE());
    pps.constrained_intra_pred_flag = static_cast<int>(reader.ReadBit());
    pps.transform_skip_enabled_flag = static_cast<int>(reader.ReadBit());

    const bool cu_qp_delta_enabled = reader.ReadBit();
    pps.cu_qp_offset_enabled_flag = cu_qp_delta_enabled ? 1 : 0;
    if (cu_qp_delta_enabled) {
        pps.diff_cu_chroma_qp_offset_depth = static_cast<int>(reader.ReadUE());
    }

    pps.pps_cb_offset = static_cast<int>(reader.ReadSE());
    pps.pps_cr_offset = static_cast<int>(reader.ReadSE());
    pps.pps_slice_chroma_qp_offsets_present_flag = static_cast<int>(reader.ReadBit());
    pps.weighted_pred_flag = static_cast<int>(reader.ReadBit());
    pps.weighted_bipred_flag = static_cast<int>(reader.ReadBit());
    pps.transquant_bypass_enabled_flag = static_cast<int>(reader.ReadBit());

    pps.tiles_enabled_flag = static_cast<int>(reader.ReadBit());
    pps.entropy_coding_sync_enabled_flag = static_cast<int>(reader.ReadBit());
    if (pps.tiles_enabled_flag) {
        pps.num_tile_columns_minus1 = static_cast<int>(reader.ReadUE());
        pps.num_tile_rows_minus1 = static_cast<int>(reader.ReadUE());
        pps.tile_uniform_spacing_flag = static_cast<int>(reader.ReadBit());
        if (!pps.tile_uniform_spacing_flag) {
            for (int i = 0; i < pps.num_tile_columns_minus1; ++i) {
                pps.column_width_minus1.push_back(static_cast<int>(reader.ReadUE()));
            }
            for (int i = 0; i < pps.num_tile_rows_minus1; ++i) {
                pps.row_height_minus1.push_back(static_cast<int>(reader.ReadUE()));
            }
        }
        pps.loop_filter_across_tiles_enabled_flag = static_cast<int>(reader.ReadBit());
    }

    pps.loop_filter_across_slices_enabled_flag = static_cast<int>(reader.ReadBit());
    pps.deblocking_filter_control_present_flag = static_cast<int>(reader.ReadBit());
    if (pps.deblocking_filter_control_present_flag) {
        reader.SkipBits(1); // deblocking_filter_override_enabled_flag
        const bool disabled = reader.ReadBit();
        if (!disabled) {
            reader.ReadSE(); // pps_beta_offset_div2
            reader.ReadSE(); // pps_tc_offset_div2
        }
    }

    const bool scaling_list_present = reader.ReadBit();
    if (scaling_list_present) {
        SkipScalingListData(reader);
    }

    reader.SkipBits(1);              // lists_modification_present_flag
    reader.ReadUE();                 // log2_parallel_merge_level_minus2
    pps.slice_segment_header_extension_present_flag = static_cast<int>(reader.ReadBit());

    if (reader.HasError()) {
        return model::HevcPpsInfo();
    }

    pps.present = true;
    return pps;
}

// --------------------------------------------------------------------------
// extradata 入口
// --------------------------------------------------------------------------
model::HevcVpsInfo HevcBitstreamParser::ParseVps(const uint8_t* extradata, size_t size) {
    if (!extradata || size == 0) return model::HevcVpsInfo();

    auto result = utils::ExtradataParser::Parse(extradata, size);
    for (const auto& nal : result.nal_units) {
        if (IsVpsNalUnit(nal)) {
            return ParseVpsFromNalUnit(nal);
        }
    }
    return model::HevcVpsInfo();
}

model::HevcSpsInfo HevcBitstreamParser::ParseSpf(const uint8_t* extradata, size_t size) {
    if (!extradata || size == 0) return model::HevcSpsInfo();

    auto result = utils::ExtradataParser::Parse(extradata, size);
    for (const auto& nal : result.nal_units) {
        if (IsSpsNalUnit(nal)) {
            return ParseSpfFromNalUnit(nal);
        }
    }
    return model::HevcSpsInfo();
}

model::HevcPpsInfo HevcBitstreamParser::ParsePps(const uint8_t* extradata, size_t size) {
    if (!extradata || size == 0) return model::HevcPpsInfo();

    auto result = utils::ExtradataParser::Parse(extradata, size);
    for (const auto& nal : result.nal_units) {
        if (IsPpsNalUnit(nal)) {
            return ParsePpsFromNalUnit(nal);
        }
    }
    return model::HevcPpsInfo();
}

// --------------------------------------------------------------------------
// SEI
// --------------------------------------------------------------------------
std::vector<model::HevcSeiMessage> HevcBitstreamParser::ParseSeiMessages(
    const uint8_t* data, size_t size) {
    std::vector<model::HevcSeiMessage> sei_messages;

    if (!data || size < 2) {
        return sei_messages;
    }

    // SEI payload 是字节对齐的，直接按 7.3.4 的 ff-byte 规则扫描
    size_t pos = 0;
    while (pos < size) {
        uint32_t payload_type = 0;
        while (pos < size && data[pos] == 0xFF) {
            payload_type += 255;
            ++pos;
        }
        if (pos >= size) break;
        payload_type += data[pos++];

        uint32_t payload_size = 0;
        while (pos < size && data[pos] == 0xFF) {
            payload_size += 255;
            ++pos;
        }
        if (pos >= size) break;
        payload_size += data[pos++];

        if (payload_size == 0 || pos + payload_size > size) break;

        model::HevcSeiMessage sei;
        sei.present = true;
        sei.type = static_cast<model::HevcSeiMessage::Type>(payload_type);
        sei.data.assign(data + pos, data + pos + payload_size);
        sei_messages.push_back(sei);

        pos += payload_size;
    }

    return sei_messages;
}

} // namespace analyzer
} // namespace videoeye
