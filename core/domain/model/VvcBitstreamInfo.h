#pragma once

// VVC (H.266) 码流解析结果：NAL unit / VUI / VPS / SPS / PPS / vvcC 六组纯值类型。
//
// 从 BitstreamInfo.h 按 codec 拆出，使每个编解码器的结构自成一文件；
// BitstreamInfo.h 作为 umbrella 重新 include 本头，调用方无需改动。
// 本文件只依赖标准库，不依赖其它 codec 子头。
//
// 语法顺序以 FFmpeg 的 libavcodec/cbs_h266_syntax_template.c 为准（CBS 层逐位
// 解析，与 H.266 规范 7.3.2.x 一一对应）。字段命名沿用规范里的语法元素名。

#include <cstdint>
#include <string>

namespace videoeye {
namespace model {

// --------------------------------------------------------------------------
// VVC (H.266) 码流信息
// --------------------------------------------------------------------------
struct VvcNalUnitInfo {
    bool present = false;

    int nal_unit_type = 0;
    int temporal_id = 0;
    int nuh_layer_id = 0;

    bool is_vps = false;
    bool is_vps_extension = false;
};

// VVC VUI（H.266 7.3.2.3 的 vui_parameters()）
// 与 H.264 的 VUI 相比少了 timing/HRD —— VVC 把它们挪到了
// general_timing_hrd_parameters()，这里只收录展示需要的字段。
struct VvcVuiInfo {
    bool present = false;

    bool progressive_source_flag = false;
    bool interlaced_source_flag = false;
    bool non_packed_constraint_flag = false;
    bool non_projected_constraint_flag = false;

    bool aspect_ratio_info_present_flag = false;
    bool aspect_ratio_constant_flag = false;
    int aspect_ratio_idc = 0;           // 0xFF = Extended_SAR
    int sar_width = 0;
    int sar_height = 0;

    bool overscan_info_present_flag = false;
    bool overscan_appropriate_flag = false;

    bool colour_description_present_flag = false;
    int colour_primaries = 2;           // 未出现时规范推断为 2（未指定）
    int transfer_characteristics = 2;
    int matrix_coeffs = 2;
    bool full_range_flag = false;

    bool chroma_loc_info_present_flag = false;
    int chroma_sample_loc_type_frame = 6;
};

struct VvcVpsInfo {
    bool present = false;

    int vps_video_parameter_set_id = 0;
    int vps_max_layers_minus1 = 0;
    int vps_max_sublayers_minus1 = 0;
    bool vps_default_ptl_dpb_hrd_max_tid_flag = true;
    bool vps_all_independent_layers_flag = true;
    bool vps_each_layer_is_an_ols_flag = true;
    int vps_num_ptls_minus1 = 0;

    // PTL（vps_profile_tier_level[0]，单层码流就是它）
    int general_profile_idc = 0;
    int general_tier_flag = 0;
    uint32_t general_level_idc = 0;
    int ptl_num_sub_profiles = 0;

    // Color config（vvcC / OLS DPB 里可能带，码流里通常没有）
    int vps_chroma_format_idc = 0;
    int vps_bit_depth_luma_minus8 = 0;
    int vps_bit_depth_chroma_minus8 = 0;

    int MaxLayers() const { return vps_max_layers_minus1 + 1; }
    int MaxSubLayers() const { return vps_max_sublayers_minus1 + 1; }

    std::string ProfileName() const;
    std::string LevelString() const;
};

struct VvcSpsInfo {
    bool present = false;

    int sps_seq_parameter_set_id = 0;
    int sps_video_parameter_set_id = 0;
    int sps_max_sublayers_minus1 = 0;
    int sps_chroma_format_idc = 0;
    int sps_log2_ctu_size_minus5 = 0;

    bool sps_ptl_dpb_hrd_params_present_flag = false;
    bool sps_gdr_enabled_flag = false;
    bool sps_ref_pic_resampling_enabled_flag = false;
    bool sps_res_change_in_clvs_allowed_flag = false;

    // 图像尺寸（编码尺寸；显示尺寸要看 conformance window）
    int sps_pic_width_max_in_luma_samples = 0;
    int sps_pic_height_max_in_luma_samples = 0;
    bool sps_conformance_window_flag = false;
    int sps_conf_win_left_offset = 0;
    int sps_conf_win_right_offset = 0;
    int sps_conf_win_top_offset = 0;
    int sps_conf_win_bottom_offset = 0;

    int sps_num_subpics_minus1 = 0;
    int sps_bitdepth_minus8 = 0;
    bool sps_entropy_coding_sync_enabled_flag = false;
    bool sps_entry_point_offsets_present_flag = false;
    int sps_log2_max_pic_order_cnt_lsb_minus4 = 0;
    bool sps_poc_msb_cycle_flag = false;

    int sps_log2_min_luma_coding_block_size_minus2 = 0;
    bool sps_partition_constraints_override_enabled_flag = false;

    // 编码工具开关（面板只展示常用的几个，其余按规范跳过）
    bool sps_sao_enabled_flag = false;
    bool sps_alf_enabled_flag = false;
    bool sps_ccalf_enabled_flag = false;
    bool sps_lmcs_enabled_flag = false;
    bool sps_joint_cbcr_enabled_flag = false;
    bool sps_transform_skip_enabled_flag = false;
    bool sps_mts_enabled_flag = false;
    bool sps_lfnst_enabled_flag = false;
    bool sps_weighted_pred_flag = false;
    bool sps_weighted_bipred_flag = false;
    bool sps_long_term_ref_pics_flag = false;
    bool sps_inter_layer_prediction_enabled_flag = false;
    bool sps_temporal_mvp_enabled_flag = false;
    bool sps_affine_enabled_flag = false;
    bool sps_palette_enabled_flag = false;
    bool sps_ibc_enabled_flag = false;
    bool sps_dep_quant_enabled_flag = false;
    bool sps_sign_data_hiding_enabled_flag = false;
    int sps_num_ref_pic_lists[2] = {0, 0};

    bool sps_field_seq_flag = false;
    bool sps_vui_parameters_present_flag = false;
    int sps_vui_payload_size_minus1 = 0;

    // PTL（sps_ptl_dpb_hrd_params_present_flag == 1 时才有）
    int general_profile_idc = 0;
    int general_tier_flag = 0;
    uint32_t general_level_idc = 0;
    int ptl_num_sub_profiles = 0;

    VvcVuiInfo vui;

    // 派生值
    int CtuSize() const { return 1 << (sps_log2_ctu_size_minus5 + 5); }
    int MinCbSizeY() const { return 1 << (sps_log2_min_luma_coding_block_size_minus2 + 2); }
    int MaxPicOrderCntLsb() const { return 1 << (sps_log2_max_pic_order_cnt_lsb_minus4 + 4); }
    int SubWidthC() const;
    int SubHeightC() const;

    // 显示尺寸 = 编码尺寸 - conformance window（按色度采样换算裁剪单位）
    int width() const;
    int height() const;

    int BitDepthLuma() const { return sps_bitdepth_minus8 + 8; }
    int BitDepthChroma() const { return sps_bitdepth_minus8 + 8; }

    std::string ProfileName() const;
    std::string LevelString() const;
};

// VVC PPS：只解析头部到 subpic id 映射为止（后面的 tile/slice 划分面板用不到）
struct VvcPpsInfo {
    bool present = false;

    int pps_pic_parameter_set_id = 0;
    int pps_seq_parameter_set_id = 0;
    bool pps_mixed_nalu_types_in_pic_flag = false;
    int pps_pic_width_in_luma_samples = 0;
    int pps_pic_height_in_luma_samples = 0;
    bool pps_conformance_window_flag = false;
    int pps_conf_win_left_offset = 0;
    int pps_conf_win_right_offset = 0;
    int pps_conf_win_top_offset = 0;
    int pps_conf_win_bottom_offset = 0;
    bool pps_output_flag_present_flag = false;
    bool pps_no_pic_partition_flag = false;
    bool pps_subpic_id_mapping_present_flag = false;
    int pps_num_subpics_minus1 = 0;

    // 从 SPS 拷过来，供 width()/height() 换算裁剪单位
    int chroma_format_idc = 1;
    int SubWidthC() const;
    int SubHeightC() const;
    int width() const;
    int height() const;
};

// vvcC（VvcDecoderConfigurationRecord）里解析出的信息。
//
// 为什么单独存一份：VVC 的 SPS 只在 sps_ptl_dpb_hrd_params_present_flag=1
// 时才带 PTL，单层码流通常把这个 flag 置 0（PTL 只放 VPS 里）；而 MP4 的
// extradata 只有 vvcC、没有 VPS。所以 profile/level/位深很多时候只能从
// vvcC 拿到 —— 与 AV1 的 av1_config 同样定位，作兜底而非主数据源。
struct VvcCodecConfigInfo {
    bool present = false;

    int general_profile_idc = 0;
    int general_tier_flag = 0;
    uint32_t general_level_idc = 0;
    int chroma_format_idc = 1;      // 0=4:0:0 1=4:2:0 2=4:2:2 3=4:4:4
    int bit_depth_minus8 = 0;
    int num_sublayers = 0;          // vvcC 里的 3 位字段，实际子层数 = 值+1
    int max_picture_width = 0;
    int max_picture_height = 0;

    int BitDepth() const { return bit_depth_minus8 + 8; }
    int MaxSubLayers() const { return num_sublayers + 1; }

    std::string ProfileName() const;
    std::string LevelString() const;
};

} // namespace model
} // namespace videoeye