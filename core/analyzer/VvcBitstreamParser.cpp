#include "core/analyzer/VvcBitstreamParser.h"

#include <algorithm>

#include "utils/BitReader.h"

namespace videoeye {
namespace analyzer {
namespace {

// H.266 7.3.1.3（Table 5）：VPS / SPS / PPS 的 NAL unit type
constexpr int kVvcNutVps = 14;
constexpr int kVvcNutSps = 15;
constexpr int kVvcNutPps = 16;

int SubWidthC(int chroma_format_idc) {
    return (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
}

int SubHeightC(int chroma_format_idc) { return (chroma_format_idc == 1) ? 2 : 1; }

// ceil(log2(v))，v >= 1
int CeilLog2(uint32_t v) {
    int bits = 0;
    while ((1u << bits) < v) ++bits;
    return bits;
}

// ceil(v / 2^s)
int CeilShift(int v, int s) { return (v + (1 << s) - 1) >> s; }

}  // namespace

// --------------------------------------------------------------------------
// NAL 单元判定 / RBSP
// --------------------------------------------------------------------------
bool VvcBitstreamParser::IsVpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == kVvcNutVps;
}

bool VvcBitstreamParser::IsSpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == kVvcNutSps;
}

bool VvcBitstreamParser::IsPpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == kVvcNutPps;
}

std::vector<uint8_t> VvcBitstreamParser::GetRbsp(const utils::NalUnit& nal_unit) {
    if (nal_unit.data.empty()) return {};
    return utils::UnescapeRbsp(nal_unit.data.data(), nal_unit.data.size());
}

std::string VvcBitstreamParser::GetProfileName(int general_profile_idc) {
    model::VvcSpsInfo tmp;
    tmp.general_profile_idc = general_profile_idc;
    return tmp.ProfileName();
}

std::string VvcBitstreamParser::GetLevelVersion(uint32_t general_level_idc) {
    const int major = static_cast<int>(general_level_idc) / 16;
    const int minor = (static_cast<int>(general_level_idc) % 16) / 3;
    return std::to_string(major) + "." + std::to_string(minor);
}

// --------------------------------------------------------------------------
// 通用语法结构（只跳过，不落地字段）
// --------------------------------------------------------------------------
void VvcBitstreamParser::SkipGeneralConstraintsInfo(utils::BitReader& reader) {
    const bool gci_present = reader.ReadBit();
    if (!gci_present) {
        reader.AlignToByte();
        return;
    }

    reader.SkipBits(3);  // intra_only / all_layers_independent / one_au_only
    reader.SkipBits(4);  // sixteen_minus_max_bitdepth_constraint_idc
    reader.SkipBits(2);  // three_minus_max_chroma_format_constraint_idc
    reader.SkipBits(10);  // NAL unit type related
    reader.SkipBits(6);   // tile / slice / subpicture partitioning
    reader.SkipBits(2);   // three_minus_max_log2_ctu_size_constraint_idc
    reader.SkipBits(3);   // CTU and block partitioning
    reader.SkipBits(6);   // intra
    reader.SkipBits(16);  // inter
    reader.SkipBits(13);  // transform / quantization / residual
    reader.SkipBits(6);   // loop filter

    const int num_additional_bits = static_cast<int>(reader.ReadBits(8));
    int used = 0;
    if (num_additional_bits > 5) {
        reader.SkipBits(6);
        used = 6;
    }
    if (num_additional_bits > used) {
        reader.SkipBits(num_additional_bits - used);
    }
    reader.AlignToByte();
}

void VvcBitstreamParser::ParseProfileTierLevel(utils::BitReader& reader, bool profile_tier_present,
                                               int max_num_sub_layers_minus1, int* profile_idc,
                                               int* tier_flag, uint32_t* level_idc,
                                               int* num_sub_profiles) {
    if (profile_tier_present) {
        const int profile = static_cast<int>(reader.ReadBits(7));
        const int tier = static_cast<int>(reader.ReadBit());
        if (profile_idc) *profile_idc = profile;
        if (tier_flag) *tier_flag = tier;
    }

    const uint32_t level = reader.ReadBits(8);
    if (level_idc) *level_idc = level;

    reader.SkipBits(2);  // ptl_frame_only_constraint_flag / ptl_multilayer_enabled_flag
    if (profile_tier_present) {
        SkipGeneralConstraintsInfo(reader);
    }

    // 子层 level 存在标志：从 max_num_sub_layers_minus1 - 1 到 0 倒序
    std::vector<int> sublayer_level_present(std::max(0, max_num_sub_layers_minus1), 0);
    for (int i = max_num_sub_layers_minus1 - 1; i >= 0; --i) {
        sublayer_level_present[i] = static_cast<int>(reader.ReadBit());
    }
    reader.AlignToByte();
    for (int i = max_num_sub_layers_minus1 - 1; i >= 0; --i) {
        if (sublayer_level_present[i]) reader.SkipBits(8);  // sublayer_level_idc[i]
    }

    if (profile_tier_present) {
        const int num_sub_profiles_value = static_cast<int>(reader.ReadBits(8));
        if (num_sub_profiles) *num_sub_profiles = num_sub_profiles_value;
        for (int i = 0; i < num_sub_profiles_value; ++i) {
            reader.SkipBits(32);  // general_sub_profile_idc[i]
        }
    }
}

void VvcBitstreamParser::SkipProfileTierLevel(utils::BitReader& reader, bool profile_tier_present,
                                              int max_num_sub_layers_minus1) {
    ParseProfileTierLevel(reader, profile_tier_present, max_num_sub_layers_minus1, nullptr,
                          nullptr, nullptr, nullptr);
}

void VvcBitstreamParser::SkipDpbParameters(utils::BitReader& reader, int max_sublayers_minus1,
                                           bool sublayer_info_flag) {
    const int first = sublayer_info_flag ? 0 : max_sublayers_minus1;
    for (int i = first; i <= max_sublayers_minus1; ++i) {
        reader.ReadUE();  // dpb_max_dec_pic_buffering_minus1[i]
        reader.ReadUE();  // dpb_max_num_reorder_pics[i]
        reader.ReadUE();  // dpb_max_latency_increase_plus1[i]
    }
}

void VvcBitstreamParser::SkipSubLayerHrdParameters(utils::BitReader& reader,
                                                   const TimingHrdContext& ctx) {
    for (uint32_t i = 0; i <= ctx.hrd_cpb_cnt_minus1; ++i) {
        reader.ReadUE();  // bit_rate_value_minus1
        reader.ReadUE();  // cpb_size_value_minus1
        if (ctx.du_hrd_present) {
            reader.ReadUE();  // cpb_size_du_value_minus1
            reader.ReadUE();  // bit_rate_du_value_minus1
        }
        reader.ReadBit();  // cbr_flag
    }
}

void VvcBitstreamParser::SkipGeneralTimingHrdParameters(utils::BitReader& reader,
                                                        TimingHrdContext* ctx) {
    reader.SkipBits(32);  // num_units_in_tick
    reader.SkipBits(32);  // time_scale
    const bool nal_hrd = reader.ReadBit();
    const bool vcl_hrd = reader.ReadBit();
    bool du_hrd = false;
    uint32_t cpb_cnt_minus1 = 0;

    if (nal_hrd || vcl_hrd) {
        reader.SkipBits(1);  // general_same_pic_timing_in_all_ols_flag
        du_hrd = reader.ReadBit();
        if (du_hrd) reader.SkipBits(8);  // tick_divisor_minus2
        reader.SkipBits(4);              // bit_rate_scale
        reader.SkipBits(4);              // cpb_size_scale
        if (du_hrd) reader.SkipBits(4);  // cpb_size_du_scale
        cpb_cnt_minus1 = reader.ReadUE();
    }

    if (ctx) {
        ctx->nal_hrd_present = nal_hrd;
        ctx->vcl_hrd_present = vcl_hrd;
        ctx->du_hrd_present = du_hrd;
        ctx->hrd_cpb_cnt_minus1 = cpb_cnt_minus1;
    }
}

void VvcBitstreamParser::SkipOlsTimingHrdParameters(utils::BitReader& reader, int first_sublayer,
                                                    int max_sublayers_minus1,
                                                    const TimingHrdContext& ctx) {
    for (int i = first_sublayer; i <= max_sublayers_minus1; ++i) {
        const bool fixed_general = reader.ReadBit();
        bool fixed_within_cvs = true;
        if (!fixed_general) {
            fixed_within_cvs = reader.ReadBit();
        }
        if (fixed_within_cvs) {
            reader.ReadUE();  // elemental_duration_in_tc_minus1[i]
        } else if ((ctx.nal_hrd_present || ctx.vcl_hrd_present) && ctx.hrd_cpb_cnt_minus1 == 0) {
            reader.ReadBit();  // low_delay_hrd_flag[i]
        }
        if (ctx.nal_hrd_present) SkipSubLayerHrdParameters(reader, ctx);
        if (ctx.vcl_hrd_present) SkipSubLayerHrdParameters(reader, ctx);
    }
}

void VvcBitstreamParser::SkipRefPicListStruct(utils::BitReader& reader, int poc_lsb_bits,
                                              bool long_term_ref_pics,
                                              bool inter_layer_prediction) {
    const uint32_t num_ref_entries = reader.ReadUE();
    bool ltrp_in_header = false;
    if (long_term_ref_pics && num_ref_entries > 0) {
        ltrp_in_header = reader.ReadBit();
    }

    for (uint32_t i = 0; i < num_ref_entries; ++i) {
        bool inter_layer_ref_pic = false;
        if (inter_layer_prediction) {
            inter_layer_ref_pic = reader.ReadBit();
        }
        if (!inter_layer_ref_pic) {
            bool st_ref_pic = true;
            if (long_term_ref_pics) {
                st_ref_pic = reader.ReadBit();
            }
            if (st_ref_pic) {
                const uint32_t abs_delta_poc = reader.ReadUE();
                // 短时参考项是否带符号位（加权预测时首项不带 +1 修正）
                const uint32_t abs_delta_poc_st = abs_delta_poc + 1;
                if (abs_delta_poc_st > 0) reader.ReadBit();  // strp_entry_sign_flag
            } else if (!ltrp_in_header) {
                reader.SkipBits(poc_lsb_bits);  // rpls_poc_lsb_lt
            }
        } else {
            reader.ReadUE();  // ilrp_idx[i]（多层才可能出现）
        }
    }
}

// --------------------------------------------------------------------------
// VUI
// --------------------------------------------------------------------------
void VvcBitstreamParser::ParseVuiParameters(utils::BitReader& reader, model::VvcVuiInfo& vui) {
    vui.present = true;

    vui.progressive_source_flag = reader.ReadBit();
    vui.interlaced_source_flag = reader.ReadBit();
    vui.non_packed_constraint_flag = reader.ReadBit();
    vui.non_projected_constraint_flag = reader.ReadBit();

    vui.aspect_ratio_info_present_flag = reader.ReadBit();
    if (vui.aspect_ratio_info_present_flag) {
        vui.aspect_ratio_constant_flag = reader.ReadBit();
        vui.aspect_ratio_idc = static_cast<int>(reader.ReadBits(8));
        if (vui.aspect_ratio_idc == 255) {
            vui.sar_width = static_cast<int>(reader.ReadBits(16));
            vui.sar_height = static_cast<int>(reader.ReadBits(16));
        }
    }

    vui.overscan_info_present_flag = reader.ReadBit();
    if (vui.overscan_info_present_flag) {
        vui.overscan_appropriate_flag = reader.ReadBit();
    }

    vui.colour_description_present_flag = reader.ReadBit();
    if (vui.colour_description_present_flag) {
        vui.colour_primaries = static_cast<int>(reader.ReadBits(8));
        vui.transfer_characteristics = static_cast<int>(reader.ReadBits(8));
        vui.matrix_coeffs = static_cast<int>(reader.ReadBits(8));
        vui.full_range_flag = reader.ReadBit();
    }

    vui.chroma_loc_info_present_flag = reader.ReadBit();
    if (vui.chroma_loc_info_present_flag) {
        if (vui.progressive_source_flag && !vui.interlaced_source_flag) {
            vui.chroma_sample_loc_type_frame = static_cast<int>(reader.ReadUE());
        } else {
            reader.ReadUE();  // top field
            reader.ReadUE();  // bottom field
        }
    }
}

// --------------------------------------------------------------------------
// VPS
// --------------------------------------------------------------------------
model::VvcVpsInfo VvcBitstreamParser::ParseVpsFromNalUnit(const utils::NalUnit& nal_unit) {
    model::VvcVpsInfo vps;
    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) return vps;

    utils::BitReader reader;
    reader.Reset(rbsp.data(), rbsp.size());

    vps.vps_video_parameter_set_id = static_cast<int>(reader.ReadBits(4));
    vps.vps_max_layers_minus1 = static_cast<int>(reader.ReadBits(6));
    vps.vps_max_sublayers_minus1 = static_cast<int>(reader.ReadBits(3));

    bool default_ptl_dpb_hrd_max_tid = true;
    if (vps.vps_max_layers_minus1 > 0 && vps.vps_max_sublayers_minus1 > 0) {
        default_ptl_dpb_hrd_max_tid = reader.ReadBit();
    }
    vps.vps_default_ptl_dpb_hrd_max_tid_flag = default_ptl_dpb_hrd_max_tid;

    bool all_independent_layers = true;
    if (vps.vps_max_layers_minus1 > 0) {
        all_independent_layers = reader.ReadBit();
    }
    vps.vps_all_independent_layers_flag = all_independent_layers;

    // 层信息：单层时只有 vps_layer_id[0]
    for (int i = 0; i <= vps.vps_max_layers_minus1; ++i) {
        reader.SkipBits(6);  // vps_layer_id[i]
        if (i > 0 && !all_independent_layers) {
            const bool independent = reader.ReadBit();
            if (!independent) {
                const bool max_tid_ref_present = reader.ReadBit();
                for (int j = 0; j < i; ++j) {
                    const bool direct_ref = reader.ReadBit();
                    if (max_tid_ref_present && direct_ref) {
                        reader.SkipBits(3);  // vps_max_tid_il_ref_pics_plus1[i][j]
                    }
                }
            }
        }
    }

    bool each_layer_is_an_ols = true;
    int total_num_olss = 1;
    if (vps.vps_max_layers_minus1 > 0) {
        if (all_independent_layers) {
            each_layer_is_an_ols = reader.ReadBit();
        } else {
            each_layer_is_an_ols = false;
        }
        if (!each_layer_is_an_ols) {
            int ols_mode_idc = 2;
            if (!all_independent_layers) {
                ols_mode_idc = static_cast<int>(reader.ReadBits(2));
            }
            if (ols_mode_idc == 2) {
                const int num_output_layer_sets_minus2 = static_cast<int>(reader.ReadBits(8));
                for (int i = 1; i <= num_output_layer_sets_minus2 + 1; ++i) {
                    for (int j = 0; j <= vps.vps_max_layers_minus1; ++j) {
                        reader.SkipBits(1);  // vps_ols_output_layer_flag[i][j]
                    }
                }
                total_num_olss = num_output_layer_sets_minus2 + 2;
            } else {
                // ols_mode_idc 0/1/4 都是「层数 == OLS 数」
                total_num_olss = vps.vps_max_layers_minus1 + 1;
            }
            vps.vps_num_ptls_minus1 = static_cast<int>(reader.ReadBits(8));
        } else {
            total_num_olss = vps.vps_max_layers_minus1 + 1;
            vps.vps_num_ptls_minus1 = static_cast<int>(reader.ReadBits(8));
        }
    } else {
        each_layer_is_an_ols = true;
        vps.vps_num_ptls_minus1 = 0;
    }
    vps.vps_each_layer_is_an_ols_flag = each_layer_is_an_ols;

    std::vector<int> pt_present(vps.vps_num_ptls_minus1 + 1, 1);
    std::vector<int> ptl_max_tid(vps.vps_num_ptls_minus1 + 1, vps.vps_max_sublayers_minus1);
    for (int i = 0; i <= vps.vps_num_ptls_minus1; ++i) {
        if (i > 0) {
            pt_present[i] = static_cast<int>(reader.ReadBit());
        }
        if (!default_ptl_dpb_hrd_max_tid) {
            ptl_max_tid[i] = static_cast<int>(reader.ReadBits(3));
        }
    }
    reader.AlignToByte();

    for (int i = 0; i <= vps.vps_num_ptls_minus1; ++i) {
        if (i == 0) {
            ParseProfileTierLevel(reader, pt_present[i] != 0, ptl_max_tid[i],
                                  &vps.general_profile_idc, &vps.general_tier_flag,
                                  &vps.general_level_idc, &vps.ptl_num_sub_profiles);
        } else {
            SkipProfileTierLevel(reader, pt_present[i] != 0, ptl_max_tid[i]);
        }
    }
    vps.present = true;

    if (reader.HasError()) return vps;

    // OLS -> PTL 索引
    for (int i = 0; i < total_num_olss; ++i) {
        if (vps.vps_num_ptls_minus1 > 0 && vps.vps_num_ptls_minus1 + 1 != total_num_olss) {
            reader.SkipBits(8);  // vps_ols_ptl_idx[i]
        }
    }

    if (!each_layer_is_an_ols) {
        // 多层 + 非 each-layer-is-an-OLS：后面的 OLS DPB / HRD 需要
        // NumMultiLayerOlss 推导（依赖 vps_direct_ref_layer_flag 的传递闭包），
        // 这里停止解析，已取得的 PTL 仍然有效。
        return vps;
    }

    reader.ReadBit();  // vps_extension_flag
    return vps;
}

// --------------------------------------------------------------------------
// SPS
// --------------------------------------------------------------------------
model::VvcSpsInfo VvcBitstreamParser::ParseSpsFromNalUnit(const utils::NalUnit& nal_unit) {
    model::VvcSpsInfo sps;
    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) return sps;

    utils::BitReader reader;
    reader.Reset(rbsp.data(), rbsp.size());

    sps.sps_seq_parameter_set_id = static_cast<int>(reader.ReadBits(4));
    sps.sps_video_parameter_set_id = static_cast<int>(reader.ReadBits(4));
    sps.sps_max_sublayers_minus1 = static_cast<int>(reader.ReadBits(3));
    sps.sps_chroma_format_idc = static_cast<int>(reader.ReadBits(2));
    sps.sps_log2_ctu_size_minus5 = static_cast<int>(reader.ReadBits(2));

    const int ctb_log2_size_y = sps.sps_log2_ctu_size_minus5 + 5;
    const int ctb_size_y = 1 << ctb_log2_size_y;

    sps.sps_ptl_dpb_hrd_params_present_flag = reader.ReadBit();
    if (sps.sps_ptl_dpb_hrd_params_present_flag) {
        ParseProfileTierLevel(reader, true, sps.sps_max_sublayers_minus1,
                              &sps.general_profile_idc, &sps.general_tier_flag,
                              &sps.general_level_idc, &sps.ptl_num_sub_profiles);
    }

    sps.sps_gdr_enabled_flag = reader.ReadBit();
    sps.sps_ref_pic_resampling_enabled_flag = reader.ReadBit();
    if (sps.sps_ref_pic_resampling_enabled_flag) {
        sps.sps_res_change_in_clvs_allowed_flag = reader.ReadBit();
    }

    sps.sps_pic_width_max_in_luma_samples = static_cast<int>(reader.ReadUE());
    sps.sps_pic_height_max_in_luma_samples = static_cast<int>(reader.ReadUE());

    sps.sps_conformance_window_flag = reader.ReadBit();
    if (sps.sps_conformance_window_flag) {
        sps.sps_conf_win_left_offset = static_cast<int>(reader.ReadUE());
        sps.sps_conf_win_right_offset = static_cast<int>(reader.ReadUE());
        sps.sps_conf_win_top_offset = static_cast<int>(reader.ReadUE());
        sps.sps_conf_win_bottom_offset = static_cast<int>(reader.ReadUE());
    }

    const int tmp_width_val = CeilShift(sps.sps_pic_width_max_in_luma_samples, ctb_log2_size_y);
    const int tmp_height_val = CeilShift(sps.sps_pic_height_max_in_luma_samples, ctb_log2_size_y);

    // ---- subpicture ----
    if (reader.ReadBit()) {  // sps_subpic_info_present_flag
        sps.sps_num_subpics_minus1 = static_cast<int>(reader.ReadUE());
        bool independent_subpics = true;
        bool same_size = false;
        if (sps.sps_num_subpics_minus1 > 0) {
            independent_subpics = reader.ReadBit();
            same_size = reader.ReadBit();
        }
        if (sps.sps_num_subpics_minus1 > 0) {
            const int wlen = CeilLog2(static_cast<uint32_t>(tmp_width_val));
            const int hlen = CeilLog2(static_cast<uint32_t>(tmp_height_val));
            if (sps.sps_pic_width_max_in_luma_samples > ctb_size_y) reader.SkipBits(wlen);
            if (sps.sps_pic_height_max_in_luma_samples > ctb_size_y) reader.SkipBits(hlen);
            if (!independent_subpics) reader.SkipBits(2);
            for (int i = 1; i <= sps.sps_num_subpics_minus1; ++i) {
                if (!same_size) {
                    if (sps.sps_pic_width_max_in_luma_samples > ctb_size_y) reader.SkipBits(wlen);
                    if (sps.sps_pic_height_max_in_luma_samples > ctb_size_y) reader.SkipBits(hlen);
                    if (i < sps.sps_num_subpics_minus1 &&
                        sps.sps_pic_width_max_in_luma_samples > ctb_size_y) {
                        reader.SkipBits(wlen);
                    }
                    if (i < sps.sps_num_subpics_minus1 &&
                        sps.sps_pic_height_max_in_luma_samples > ctb_size_y) {
                        reader.SkipBits(hlen);
                    }
                }
                if (!independent_subpics) reader.SkipBits(2);
            }
        }
        const int subpic_id_len_minus1 = static_cast<int>(reader.ReadUE());
        if (reader.ReadBit()) {  // sps_subpic_id_mapping_explicitly_signalled_flag
            if (reader.ReadBit()) {  // sps_subpic_id_mapping_present_flag
                for (int i = 0; i <= sps.sps_num_subpics_minus1; ++i) {
                    reader.SkipBits(subpic_id_len_minus1 + 1);
                }
            }
        }
    }

    sps.sps_bitdepth_minus8 = static_cast<int>(reader.ReadUE());
    sps.sps_entropy_coding_sync_enabled_flag = reader.ReadBit();
    sps.sps_entry_point_offsets_present_flag = reader.ReadBit();
    sps.sps_log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(reader.ReadBits(4));
    sps.sps_poc_msb_cycle_flag = reader.ReadBit();
    if (sps.sps_poc_msb_cycle_flag) {
        reader.ReadUE();  // sps_poc_msb_cycle_len_minus1
    }

    const int num_extra_ph_bytes = static_cast<int>(reader.ReadBits(2));
    reader.SkipBits(num_extra_ph_bytes * 8);
    const int num_extra_sh_bytes = static_cast<int>(reader.ReadBits(2));
    reader.SkipBits(num_extra_sh_bytes * 8);

    if (sps.sps_ptl_dpb_hrd_params_present_flag) {
        bool sublayer_dpb_params = false;
        if (sps.sps_max_sublayers_minus1 > 0) {
            sublayer_dpb_params = reader.ReadBit();
        }
        SkipDpbParameters(reader, sps.sps_max_sublayers_minus1, sublayer_dpb_params);
    }

    sps.sps_log2_min_luma_coding_block_size_minus2 = static_cast<int>(reader.ReadUE());
    const int min_cb_log2_size_y = sps.sps_log2_min_luma_coding_block_size_minus2 + 2;
    sps.sps_partition_constraints_override_enabled_flag = reader.ReadBit();

    // ---- 块划分约束 ----
    const int min_qt_log2_intra =
        static_cast<int>(reader.ReadUE()) + min_cb_log2_size_y;
    const int max_mtt_depth_intra = static_cast<int>(reader.ReadUE());
    if (max_mtt_depth_intra != 0) {
        reader.ReadUE();
        reader.ReadUE();
    }
    bool qtbtt_dual_tree_intra = false;
    if (sps.sps_chroma_format_idc != 0) {
        qtbtt_dual_tree_intra = reader.ReadBit();
    }
    int max_mtt_depth_chroma = 0;
    if (qtbtt_dual_tree_intra) {
        const int min_qt_log2_chroma = static_cast<int>(reader.ReadUE()) + min_cb_log2_size_y;
        max_mtt_depth_chroma = static_cast<int>(reader.ReadUE());
        if (max_mtt_depth_chroma != 0) {
            reader.ReadUE();
            reader.ReadUE();
        }
        (void)min_qt_log2_chroma;
    }
    const int min_qt_log2_inter = static_cast<int>(reader.ReadUE()) + min_cb_log2_size_y;
    const int max_mtt_depth_inter = static_cast<int>(reader.ReadUE());
    if (max_mtt_depth_inter != 0) {
        reader.ReadUE();
        reader.ReadUE();
    }
    (void)min_qt_log2_intra;
    (void)min_qt_log2_inter;

    bool max_luma_transform_size_64 = false;
    if (ctb_size_y > 32) {
        max_luma_transform_size_64 = reader.ReadBit();
    }

    sps.sps_transform_skip_enabled_flag = reader.ReadBit();
    if (sps.sps_transform_skip_enabled_flag) {
        reader.ReadUE();  // sps_log2_transform_skip_max_size_minus2
        reader.ReadBit();  // sps_bdpcm_enabled_flag
    }
    sps.sps_mts_enabled_flag = reader.ReadBit();
    if (sps.sps_mts_enabled_flag) {
        reader.SkipBits(2);
    }
    sps.sps_lfnst_enabled_flag = reader.ReadBit();

    if (sps.sps_chroma_format_idc != 0) {
        sps.sps_joint_cbcr_enabled_flag = reader.ReadBit();
        const bool same_qp_table = reader.ReadBit();
        const int num_qp_tables = same_qp_table ? 1 : (sps.sps_joint_cbcr_enabled_flag ? 3 : 2);
        for (int i = 0; i < num_qp_tables; ++i) {
            reader.ReadSE();  // sps_qp_table_start_minus26[i]
            const uint32_t num_points = reader.ReadUE();
            for (uint32_t j = 0; j <= num_points; ++j) {
                reader.ReadUE();  // delta_qp_in_val_minus1
                reader.ReadUE();  // delta_qp_diff_val
            }
        }
    }

    sps.sps_sao_enabled_flag = reader.ReadBit();
    sps.sps_alf_enabled_flag = reader.ReadBit();
    if (sps.sps_alf_enabled_flag && sps.sps_chroma_format_idc != 0) {
        sps.sps_ccalf_enabled_flag = reader.ReadBit();
    }
    sps.sps_lmcs_enabled_flag = reader.ReadBit();
    sps.sps_weighted_pred_flag = reader.ReadBit();
    sps.sps_weighted_bipred_flag = reader.ReadBit();
    sps.sps_long_term_ref_pics_flag = reader.ReadBit();
    if (sps.sps_video_parameter_set_id > 0) {
        sps.sps_inter_layer_prediction_enabled_flag = reader.ReadBit();
    }
    reader.SkipBits(1);  // sps_idr_rpl_present_flag
    const bool rpl1_same_as_rpl0 = reader.ReadBit();

    for (int i = 0; i < (rpl1_same_as_rpl0 ? 1 : 2); ++i) {
        sps.sps_num_ref_pic_lists[i] = static_cast<int>(reader.ReadUE());
        for (int j = 0; j < sps.sps_num_ref_pic_lists[i]; ++j) {
            SkipRefPicListStruct(reader, sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4,
                                 sps.sps_long_term_ref_pics_flag,
                                 sps.sps_inter_layer_prediction_enabled_flag);
        }
    }
    if (rpl1_same_as_rpl0) {
        sps.sps_num_ref_pic_lists[1] = sps.sps_num_ref_pic_lists[0];
    }

    reader.SkipBits(1);  // sps_ref_wraparound_enabled_flag
    sps.sps_temporal_mvp_enabled_flag = reader.ReadBit();
    if (sps.sps_temporal_mvp_enabled_flag) {
        reader.SkipBits(1);  // sps_sbtmvp_enabled_flag
    }
    const bool amvr = reader.ReadBit();
    const bool bdof = reader.ReadBit();
    if (bdof) reader.SkipBits(1);
    reader.SkipBits(1);  // sps_smvd_enabled_flag
    const bool dmvr = reader.ReadBit();
    if (dmvr) reader.SkipBits(1);
    const bool mmvd = reader.ReadBit();
    if (mmvd) reader.SkipBits(1);
    const int max_num_merge_cand = 6 - static_cast<int>(reader.ReadUE());
    reader.SkipBits(1);  // sps_sbt_enabled_flag
    sps.sps_affine_enabled_flag = reader.ReadBit();
    if (sps.sps_affine_enabled_flag) {
        reader.ReadUE();  // sps_five_minus_max_num_subblock_merge_cand
        reader.SkipBits(1);  // 6param affine
        if (amvr) reader.SkipBits(1);
        const bool prof = reader.ReadBit();
        if (prof) reader.SkipBits(1);
    }
    reader.SkipBits(2);  // sps_bcw_enabled_flag / sps_ciip_enabled_flag
    if (max_num_merge_cand >= 2) {
        const bool gpm = reader.ReadBit();
        if (gpm && max_num_merge_cand >= 3) {
            reader.ReadUE();  // sps_max_num_merge_cand_minus_max_num_gpm_cand
        }
    }
    reader.ReadUE();  // sps_log2_parallel_merge_level_minus2
    reader.SkipBits(3);  // isp / mrl / mip
    if (sps.sps_chroma_format_idc != 0) {
        reader.SkipBits(1);  // sps_cclm_enabled_flag
    }
    if (sps.sps_chroma_format_idc == 1) {
        reader.SkipBits(2);  // chroma horizontal / vertical collocated
    }
    sps.sps_palette_enabled_flag = reader.ReadBit();
    bool act = false;
    if (sps.sps_chroma_format_idc == 3 && !max_luma_transform_size_64) {
        act = reader.ReadBit();
    }
    if (sps.sps_transform_skip_enabled_flag || sps.sps_palette_enabled_flag) {
        reader.ReadUE();  // sps_min_qp_prime_ts
    }
    sps.sps_ibc_enabled_flag = reader.ReadBit();
    if (sps.sps_ibc_enabled_flag) {
        reader.ReadUE();  // sps_six_minus_max_num_ibc_merge_cand
    }
    if (reader.ReadBit()) {  // sps_ladf_enabled_flag
        const int num_intervals = static_cast<int>(reader.ReadBits(2)) + 1;
        reader.ReadSE();  // sps_ladf_lowest_interval_qp_offset
        for (int i = 0; i < num_intervals; ++i) {
            reader.ReadSE();
            reader.ReadUE();
        }
    }
    const bool explicit_scaling_list = reader.ReadBit();
    if (sps.sps_lfnst_enabled_flag && explicit_scaling_list) {
        reader.SkipBits(1);
    }
    bool scaling_matrix_alt_colour_space = false;
    if (act && explicit_scaling_list) {
        scaling_matrix_alt_colour_space = reader.ReadBit();
    }
    if (scaling_matrix_alt_colour_space) {
        reader.SkipBits(1);
    }
    sps.sps_dep_quant_enabled_flag = reader.ReadBit();
    sps.sps_sign_data_hiding_enabled_flag = reader.ReadBit();

    if (reader.ReadBit()) {  // sps_virtual_boundaries_enabled_flag
        if (reader.ReadBit()) {  // sps_virtual_boundaries_present_flag
            const uint32_t num_ver = reader.ReadUE();
            for (uint32_t i = 0; i < num_ver; ++i) reader.ReadUE();
            const uint32_t num_hor = reader.ReadUE();
            for (uint32_t i = 0; i < num_hor; ++i) reader.ReadUE();
        }
    }

    if (sps.sps_ptl_dpb_hrd_params_present_flag) {
        if (reader.ReadBit()) {  // sps_timing_hrd_params_present_flag
            TimingHrdContext ctx;
            SkipGeneralTimingHrdParameters(reader, &ctx);
            bool sublayer_cpb_params_present = false;
            if (sps.sps_max_sublayers_minus1 > 0) {
                sublayer_cpb_params_present = reader.ReadBit();
            }
            const int first_sublayer =
                sublayer_cpb_params_present ? 0 : sps.sps_max_sublayers_minus1;
            SkipOlsTimingHrdParameters(reader, first_sublayer, sps.sps_max_sublayers_minus1, ctx);
        }
    }

    sps.sps_field_seq_flag = reader.ReadBit();
    sps.sps_vui_parameters_present_flag = reader.ReadBit();
    if (sps.sps_vui_parameters_present_flag) {
        sps.sps_vui_payload_size_minus1 = static_cast<int>(reader.ReadUE());
        reader.AlignToByte();
        ParseVuiParameters(reader, sps.vui);
    }

    sps.present = true;
    return sps;
}

// --------------------------------------------------------------------------
// PPS
// --------------------------------------------------------------------------
model::VvcPpsInfo VvcBitstreamParser::ParsePpsFromNalUnit(const utils::NalUnit& nal_unit) {
    model::VvcPpsInfo pps;
    const std::vector<uint8_t> rbsp = GetRbsp(nal_unit);
    if (rbsp.empty()) return pps;

    utils::BitReader reader;
    reader.Reset(rbsp.data(), rbsp.size());

    pps.pps_pic_parameter_set_id = static_cast<int>(reader.ReadBits(6));
    pps.pps_seq_parameter_set_id = static_cast<int>(reader.ReadBits(4));
    pps.pps_mixed_nalu_types_in_pic_flag = reader.ReadBit();
    pps.pps_pic_width_in_luma_samples = static_cast<int>(reader.ReadUE());
    pps.pps_pic_height_in_luma_samples = static_cast<int>(reader.ReadUE());

    pps.pps_conformance_window_flag = reader.ReadBit();
    if (pps.pps_conformance_window_flag) {
        pps.pps_conf_win_left_offset = static_cast<int>(reader.ReadUE());
        pps.pps_conf_win_right_offset = static_cast<int>(reader.ReadUE());
        pps.pps_conf_win_top_offset = static_cast<int>(reader.ReadUE());
        pps.pps_conf_win_bottom_offset = static_cast<int>(reader.ReadUE());
    }

    if (reader.ReadBit()) {  // pps_scaling_window_explicit_signalling_flag
        reader.ReadSE();
        reader.ReadSE();
        reader.ReadSE();
        reader.ReadSE();
    }

    pps.pps_output_flag_present_flag = reader.ReadBit();
    pps.pps_no_pic_partition_flag = reader.ReadBit();
    pps.pps_subpic_id_mapping_present_flag = reader.ReadBit();
    if (pps.pps_subpic_id_mapping_present_flag) {
        if (!pps.pps_no_pic_partition_flag) {
            pps.pps_num_subpics_minus1 = static_cast<int>(reader.ReadUE());
        }
        const int subpic_id_len_minus1 = static_cast<int>(reader.ReadUE());
        for (int i = 0; i <= pps.pps_num_subpics_minus1; ++i) {
            reader.SkipBits(subpic_id_len_minus1 + 1);
        }
    }

    // 后面是 tile / slice 划分（面板不展示），到此为止
    pps.present = true;
    return pps;
}

}  // namespace analyzer
}  // namespace videoeye
