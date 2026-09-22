#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace model {

// --------------------------------------------------------------------------
// H.264 码流信息
// --------------------------------------------------------------------------
struct H264VuiInfo {
    bool present = false;
    
    // Timing info
    bool timing_info_present = false;
    uint32_t num_units_in_tick = 0;
    uint32_t time_scale = 0;
    bool fixed_frame_rate = false;
    
    // --- 以下是 H.264 附录 E 中真实存在的 VUI 语法元素 ---
    // Aspect ratio (E.1.1)
    bool aspect_ratio_info_present = false;
    int aspect_ratio_idc = 0;           // 0xFF = Extended_SAR
    int sar_width = 0;
    int sar_height = 0;

    // Overscan
    bool overscan_info_present = false;
    bool overscan_appropriate_flag = false;

    // Video signal type（颜色描述的载体）
    bool video_signal_type_present = false;
    int video_format = 0;               // 0=Component,1=PAL,2=NTSC,3=SECAM,4=MAC,5=Unspecified
    bool video_full_range_flag = false;
    bool colour_description_present = false;

    // Chroma sample location
    bool chroma_loc_info_present = false;
    int chroma_sample_loc_type_top_field = 0;
    int chroma_sample_loc_type_bottom_field = 0;

    // HRD（区分 NAL / VCL 两套）
    bool nal_hrd_parameters_present = false;
    bool vcl_hrd_parameters_present = false;
    bool low_delay_hrd_flag = false;

    // Picture struct
    bool pic_struct_present_flag = false;

    // Bitstream restriction
    bool bitstream_restriction_flag = false;
    bool motion_vectors_over_pic_boundaries_flag = false;
    int max_bytes_per_pic_denom = 0;
    int max_bits_per_mb_denom = 0;
    int log2_max_mv_length_horizontal = 0;
    int log2_max_mv_length_vertical = 0;
    int max_num_reorder_frames = 0;
    int max_dec_frame_buffering = 0;

    // --- 以下字段在 H.264 规范中并不存在，保留仅为兼容旧引用 ---
    bool neutral_chroma_indication = false;
    bool field_seq_flag = false;
    bool frame_mbs_only_flag = true;
    
    // 供 SPS::height() 兼容使用的旧字段（真实值在 H264SpsInfo::frame_mbs_only_flag）
    // Color primaries (从 VUI)
    int color_primaries = 0;        // AVCOL_PRI_*
    int transfer_characteristics = 0; // AVCOL_TRC_*
    int matrix_coefficients = 0;    // AVCOL_SPC_*
    int color_range = 0;            // AVCOL_RANGE_*
    
    // HRD (Hypothetical Reference Decoder)
    bool hrd_present = false;
    uint8_t cpb_cnt = 0;
    uint32_t bit_rate_value = 0;
    uint32_t crb_size_value = 0;
    uint32_t dpb_delay_value = 0;
    uint32_t bit_rate_scale = 0;
    uint32_t cpb_size_scale = 0;
    
    // Scalability
    bool scalability_info_present = false;
    bool adaptive_vui_flag = false;
};

struct H264SpsInfo {
    bool present = false;
    
    // 基本参数
    int seq_parameter_set_id = 0;
    int profile_idc = 0;              // 7=High, 8=High 10, 9=High 4:2:2, 10=High 4:4:4
    int level_idc = 0;                // 31=3.1, 32=3.2, ..., 45=4.2, 51=5.1
    int constraint_flags = 0;         // constraint_set0..5，按位 0..5
    
    // Chroma format
    int chroma_format_idc = 0;        // 0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4
    bool separate_colour_plane_flag = false;
    int bit_depth_luma_minus8 = 0;    // actual = value + 8
    int bit_depth_chroma_minus8 = 0;  // actual = value + 8
    
    // GOP structure
    int qpprime_y_zero_transform_bypass_flag = 0;
    int seq_scaling_matrix_present_flag = 0;
    int log2_max_frame_num_minus4 = 0;
    int pic_order_cnt_type = 0;
    int log2_max_pic_order_cnt_lsb_minus4 = 0;
    int max_num_ref_frames = 0;       // 最大参考帧数
    int gaps_in_frame_val_allowed_flag = 0;  // gaps_in_frame_num_value_allowed_flag
    
    // Frame properties
    int pic_width_in_mbs_minus1 = 0;  // (value + 1) * 16 = width
    int pic_height_in_mbs_minus1 = 0; // pic_height_in_map_units_minus1
    bool frame_mbs_only_flag = true;
    bool mb_adaptive_frame_field_flag = false;
    bool direct_8x8_inference_flag = false;
    
    // Frame cropping
    int frame_cropping_flag = 0;
    int frame_crop_left_offset = 0;
    int frame_crop_right_offset = 0;
    int frame_crop_top_offset = 0;
    int frame_crop_bottom_offset = 0;
    
    // VUI 信息（如果存在）
    H264VuiInfo vui;
    
    // 计算出的宽高（已扣除 frame_cropping，单位换算按 7.4.2.1.1）
    int CropUnitX() const {
        const int chroma_array_type = separate_colour_plane_flag ? 0 : chroma_format_idc;
        if (chroma_array_type == 0) return 1;               // 4:0:0
        return (chroma_array_type == 3) ? 1 : 2;            // 4:4:4 → 1，4:2:0/4:2:2 → 2
    }
    int CropUnitY() const {
        const int chroma_array_type = separate_colour_plane_flag ? 0 : chroma_format_idc;
        const int frame_mbs = frame_mbs_only_flag ? 1 : 0;
        if (chroma_array_type == 0) return 2 - frame_mbs;
        return ((chroma_array_type == 1) ? 2 : 1) * (2 - frame_mbs);
    }

    int width() const {
        int w = (pic_width_in_mbs_minus1 + 1) * 16;
        if (frame_cropping_flag) {
            w -= (frame_crop_left_offset + frame_crop_right_offset) * CropUnitX();
        }
        return w;
    }
    int height() const {
        int h = (2 - (frame_mbs_only_flag ? 1 : 0)) * (pic_height_in_mbs_minus1 + 1) * 16;
        if (frame_cropping_flag) {
            h -= (frame_crop_top_offset + frame_crop_bottom_offset) * CropUnitY();
        }
        return h;
    }
    
    // Profile 名称
    std::string ProfileName() const;
    
    // Level 版本
    std::string LevelVersion() const;
    
    // Bit depth 实际值
    int BitDepthLuma() const { return bit_depth_luma_minus8 + 8; }
    int BitDepthChroma() const { return bit_depth_chroma_minus8 + 8; }
};

struct H264PpsInfo {
    bool present = false;
    
    int pic_parameter_set_id = 0;
    int seq_parameter_set_id = 0;
    int num_ref_idx_l0_default_active_minus1 = 0;
    int num_ref_idx_l1_default_active_minus1 = 0;
    bool weighted_pred_flag = false;
    int weighted_bipred_idc = 0;
    int pic_init_qp_minus26 = 0;
    int pic_init_slq_minus26 = 0;
    bool deblocking_filter_control_present_flag = false;
    bool constrained_intra_pred_flag = false;
    int redundant_pic_cnt_present_flag = 0;
    bool transform_8x8_mode_flag = false;
    bool pic_scaling_matrix_present_flag = false;
    int second_chroma_pic_init_qp_minus26 = 0;
    
    int PicWidth() const;
    int PicHeight() const;
};

// --------------------------------------------------------------------------
// H.265 码流信息
// --------------------------------------------------------------------------
struct HevcSeiMessage {
    enum Type {
        MasteringDisplay = 1,
        ContentLight = 4,
        Hdr10Plus = 5,
        HdrVivid = 6,
        BufferingPeriod = 2,
        PictureTiming = 3,
        ScalabilityInfo = 9,
        AlternativeTransferCharacteristic = 10,
        RegionRefreshInfo = 11,
        DynamicHdrParam = 13,
        AlternativeColourSpace = 16,
        ColourTransform = 17,
        SceneInfo = 18,
        DisplayOrientation = 21,
        ToneMappingInfo = 22,
        ActiveOcclusion = 24,
        AdaptationRegions = 25,
        DecodingUnitInfo = 27,
        TemporalRefChange = 28,
        MotionConstrainedTileSets = 29,
        FramerateInfo = 30,
        IsoMedia = 31,
        QualityControlData = 32,
        SampleDependency = 33,
        IsoMediaFileType = 34,
        TimeCode = 35,
        CodedDomainColourSpace = 36,
        SpatialRelationship = 37,
        AdaptiveDpb = 38,
        DpbOutputTime = 39,
        DecodedPictureHash = 128,
        RecoveryPoint = 129,
        FramePackingArrangement = 132,
        Stereo3dVideo = 133,
        AltViewpointInfo = 134,
        AltViewpointRepeat = 135,
        MultiviewViewPosition = 136,
        MultiviewAcquisitionInfo = 137,
        SubSeqLayerInformation = 142,
        SubSeqInformation = 143,
        StereoMetadata = 144,
        DepthInformation = 145,
        SubPicInfo = 146,
        RegionWiseParititionInfo = 147,
        RegionWiseParititionReplication = 148,
        RegionWiseParititionMotionInfo = 149,
        RegionWiseParititionDepthInfo = 150,
        RegionWiseParititionNormalInfo = 151,
        RegionWiseParititionTextureInfo = 152,
        RegionWiseParititionDepthToTexture = 153,
        RegionWiseParititionDepthToNormal = 154,
        RegionWiseParititionDepthToTextureRange = 155,
        RegionWiseParititionDepthToNormalRange = 156,
        RegionWiseParititionDepthToTextureDirection = 157,
        RegionWiseParititionDepthToNormalDirection = 158,
        RegionWiseParititionTextureToDepth = 159,
        RegionWiseParititionNormalToDepth = 160,
        RegionWiseParititionTextureToNormal = 161,
        RegionWiseParititionNormalToTexture = 162,
        RegionWiseParititionDepthToTextureDirectionRange = 163,
        RegionWiseParititionDepthToNormalDirectionRange = 164,
        RegionWiseParititionTextureToDepthDirection = 165,
        RegionWiseParititionNormalToDepthDirection = 166,
        RegionWiseParititionTextureToNormalDirection = 167,
        RegionWiseParititionNormalToTextureDirection = 168,
        RegionWiseParititionDepthToTextureDirectionRangeEnd = 169,
        RegionWiseParititionDepthToNormalDirectionRangeEnd = 170,
        RegionWiseParititionTextureToDepthDirectionEnd = 171,
        RegionWiseParititionNormalToDepthDirectionEnd = 172,
        RegionWiseParititionTextureToNormalDirectionEnd = 173,
        RegionWiseParititionNormalToTextureDirectionEnd = 174,
    };
    
    bool present = false;
    Type type = Type::MasteringDisplay;
    std::vector<uint8_t> data;
    
    // Mastering Display (ST 2086)
    bool has_primaries = false;
    double red_x = 0.0, red_y = 0.0;
    double green_x = 0.0, green_y = 0.0;
    double blue_x = 0.0, blue_y = 0.0;
    double white_x = 0.0, white_y = 0.0;
    double max_luminance = 0.0;   // cd/m^2
    double min_luminance = 0.0;   // cd/m^2
    
    // Content Light Level (CTA-861.3)
    bool has_cll = false;
    unsigned max_cll = 0;         // MaxCLL cd/m^2
    unsigned max_fall = 0;        // MaxFALL cd/m^2
};

struct HevcVpsInfo {
    bool present = false;
    
    int vps_video_parameter_set_id = 0;
    int vps_max_layers = 0;          // = vps_max_layers_minus1 + 1
    int vps_max_sub_layers = 0;      // = vps_max_sub_layers_minus1 + 1
    int vps_temporal_id_nesting_flag = 0;
    bool vps_sub_layer_ordering_info_present_flag = false;
    int vps_max_layer_id = 0;
    // vps_temporal_nal_layer_only_flag 在规范里叫 vps_temporal_id_nesting_flag，保留旧名兼容
    int vps_temporal_nal_layer_only_flag = 0;
    
    // PTL (Profile Tier Level)
    int general_profile_space = 0;
    int general_tier_flag = 0;      // 0=Main, 1=Main 10
    int general_profile_idc = 0;
    uint32_t general_level_idc = 0;
    
    // Sub-layer profile/level
    std::vector<bool> sub_layer_profile_present_flags;
    std::vector<bool> sub_layer_level_present_flags;
    
    // Video format
    int vps_video_format = 0;       // 0=component, 1=pal, 2=ntsc, 3=secam, 4=mac
    
    // Color config
    int vps_chroma_format_idc = 0;  // 0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4
    int vps_bit_depth_luma_minus8 = 0;
    int vps_bit_depth_chroma_minus8 = 0;
    
    // Timing
    bool vps_timing_info_present_flag = false;
    uint32_t vps_num_units_in_tick = 0;
    uint32_t vps_time_scale = 0;
    bool vps_poc_proportional_to_timestamp_flag = false;
    int vps_no_long_term_images_flag = 0;
    
    // NAL layer
    int vps_num_ptl_symbols = 0;
    
    // SEI messages
    std::vector<HevcSeiMessage> sei_messages;
};

struct HevcSpsInfo {
    bool present = false;
    
    int sps_seq_parameter_set_id = 0;
    int sps_video_parameter_set_id = 0;
    int sps_max_sub_layers_minus1 = 0;
    int sps_temporal_id_nesting_flag = 0;
    bool sps_sub_layer_ordering_info_present_flag = false;
    
    // PTL (Profile Tier Level)
    int general_profile_space = 0;
    int general_profile_idc = 0;
    int general_tier_flag = 0;
    uint32_t general_level_idc = 0;
    std::vector<bool> sub_layer_profile_present_flags;
    std::vector<bool> sub_layer_level_present_flags;
    int chroma_format_idc = 0;      // 0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4
    int separate_colour_plane_flag = 0;
    
    // 规范里直接给的是亮度采样数，不是 CTU 数
    int pic_width_in_luma_samples = 0;
    int pic_height_in_luma_samples = 0;
    int pic_width_in_ctu_minus1 = 0;  // 仅作参考，解析时按 CTU 反推
    int pic_height_in_ctu_minus1 = 0;
    
    // conformance window（等价于 H.264 的 frame_cropping）
    int conformance_window_flag = 0;
    int conf_win_left_offset = 0;
    int conf_win_right_offset = 0;
    int conf_win_top_offset = 0;
    int conf_win_bottom_offset = 0;
    
    // Bit depth
    int bit_depth_luma_minus8 = 0;
    int bit_depth_chroma_minus8 = 0;
    
    // Chroma config
    int log2_max_pic_order_cnt_lsb_minus4 = 0;
    int sub_layer_ordering_info_present_flag = 0;
    int log2_min_luma_coding_block_size_minus3 = 0;
    int log2_diff_max_min_luma_coding_block_size = 0;
    int log2_min_luma_transform_block_size_minus2 = 0;
    int log2_diff_max_min_luma_transform_block_size = 0;
    int max_transform_hierarchy_depth_inter = 0;
    int max_transform_hierarchy_depth_intra = 0;
    
    // Scaling
    int scaling_list_enable_flag = 0;
    int amp_enabled_flag = 0;
    int sample_adaptive_offset_enabled_flag = 0;
    int pcm_enabled_flag = 0;
    int log2_min_pcm_luma_coding_block_size = 0;
    int log2_diff_max_min_pcm_luma_coding_block_size = 0;
    int pcm_sample_bit_depth_luma_minus1 = 0;
    int pcm_sample_bit_depth_chroma_minus1 = 0;
    int num_short_term_ref_pic_sets = 0;
    int long_term_ref_pics_present_flag = 0;
    int num_long_term_ref_pics_sps = 0;
    
    // SPS extensions
    int spatial_scalable_idc = 0;
    int num_scalable_layers = 0;
    int num_output_reorder_specs = 0;
    int sync_source_layer = 0;
    int spatial_layer_id = 0;
    int temporal_scalable_idc = 0;
    int ref_layer_chroma_format_idc = 0;
    int inter_layer_slice_type = 0;
    int intra_slice_allowance_flag = 0;
    int intra_slice_header_data_present_flag = 0;
    int intra_slice_header_data_byte_alignment_flag = 0;
    int intra_slice_header_data_payload_size = 0;
    int intra_slice_header_data_payload_byte_alignment_flag = 0;
    int intra_slice_header_data_payload_alignment_byte = 0;
    int intra_slice_header_data_payload_alignment_bits = 0;
    int intra_slice_header_data_payload_alignment_bit = 0;
    
    // Transform skip
    int transform_skip_enabled_flag = 0;
    int log2_transform_skip_max_size_minus2 = 0;
    
    // CABAC
    int cabac_init_present_flag = 0;
    
    // CU partition
    int split_cu_delta_flag = 0;
    int log2_diff_cu_min_qp_idx_minus1 = 0;
    
    // STSA
    int stref_pic_all_flag = 0;
    
    // Temporal motion vectors
    int temporal_mvp_enabled_flag = 0;
    
    // Stronger smoothing
    int strongly_smoothed_img_between_grids_flag = 0;
    
    // Sign data hiding
    int sign_data_hiding_enabled_flag = 0;
    
    // CABAC bypass
    int cabac_bypass_alignment_enabled_flag = 0;
    
    // FP
    int four_twenty_two_log2_conversion_enable_flag = 0;
    
    // VUI
    int vui_parameters_present_flag = 0;
    int aspect_ratio_info_present_flag = 0;
    int aspect_ratio_idc = 0;           // 255 = Extended_SAR
    int sar_width = 0;
    int sar_height = 0;
    int video_full_range_flag = 0;
    int colour_primaries = 0;
    int transfer_characteristics = 0;
    int matrix_coefficients = 0;
    int chroma_sample_loc_type_top_field = 0;
    int chroma_sample_loc_type_bottom_field = 0;
    bool neutral_chroma_indication_flag = false;
    bool field_seq_flag = false;
    bool frame_field_info_present_flag = false;
    int def_disp_win_left_offset = 0;
    int def_disp_win_right_offset = 0;
    int def_disp_win_top_offset = 0;
    int def_disp_win_bottom_offset = 0;
    int vui_timing_info_present_flag = 0;
    uint32_t vui_num_units_in_tick = 0;
    uint32_t vui_time_scale = 0;
    bool vui_poc_proportional_to_timing_flag = false;
    bool vui_hrd_parameters_present_flag = false;
    int bitstream_restriction_flag = 0;
    int min_spatial_segmentation_idc = 0;
    int max_bytes_per_pic_denom = 0;
    int max_bits_per_min_cu_denom = 0;
    int log2_max_mv_length_horizontal = 0;
    int log2_max_mv_length_vertical = 0;
    
    // 显示宽高 = 亮度采样数 - conformance window（按 SubWidthC/SubHeightC 换算）
    int SubWidthC() const {
        const int cat = separate_colour_plane_flag ? 0 : chroma_format_idc;
        return (cat == 1 || cat == 2) ? 2 : 1;
    }
    int SubHeightC() const {
        const int cat = separate_colour_plane_flag ? 0 : chroma_format_idc;
        return (cat == 1) ? 2 : 1;
    }

    int width() const {
        int w = pic_width_in_luma_samples;
        if (conformance_window_flag) {
            w -= (conf_win_left_offset + conf_win_right_offset) * SubWidthC();
        }
        return w;
    }

    int height() const {
        int h = pic_height_in_luma_samples;
        if (conformance_window_flag) {
            h -= (conf_win_top_offset + conf_win_bottom_offset) * SubHeightC();
        }
        return h;
    }
    
    int BitDepthLuma() const { return bit_depth_luma_minus8 + 8; }
    int BitDepthChroma() const { return bit_depth_chroma_minus8 + 8; }
    
    std::string ProfileName() const;
};

struct HevcPpsInfo {
    bool present = false;
    
    int pic_parameter_set_id = 0;
    int seq_parameter_set_id = 0;
    int dependent_slice_segments_enabled_flag = 0;
    int sign_data_hiding_enabled_flag = 0;
    int cabac_init_present_flag = 0;
    int num_ref_idx_l0_default_active_minus1 = 0;
    int num_ref_idx_l1_default_active_minus1 = 0;
    int init_qp_minus26 = 0;
    int diff_cu_chroma_qp_offset_depth = 0;
    int chroma_qp_index_offset = 0;
    int second_chroma_qp_index_offset = 0;
    int num_extra_slice_header_bits = 0;
    int slice_segment_header_extension_present_flag = 0;
    int bits_for_temporal_id = 0;
    int single_tile_in_pic_flag = 0;
    int tile_uniform_spacing_flag = 0;
    int num_tile_columns_minus1 = 0;
    int num_tile_rows_minus1 = 0;
    std::vector<int> column_width_minus1;
    std::vector<int> row_height_minus1;
    int loop_filter_across_tiles_enabled_flag = 0;
    int loop_filter_across_slices_enabled_flag = 0;
    int output_flag_present_flag = 0;
    int num_extra_slice_header_bits_template = 0;
    int weighted_pred_flag = 0;
    int weighted_ppred_flag = 0;     // 旧名，等价 weighted_bipred_flag
    int weighted_bi_pred_flag = 0;   // 旧名，等价 weighted_bipred_flag
    int weighted_bipred_flag = 0;    // 规范名
    int deblocking_filter_control_present_flag = 0;
    int transquant_bypass_enabled_flag = 0;
    int tiles_enabled_flag = 0;
    int entropy_coding_sync_enabled_flag = 0;
    int independent_slice_flag = 0;
    int constrained_intra_pred_flag = 0;
    int transform_skip_rotation_enabled_flag = 0;
    int transform_skip_context_enabled_flag = 0;
    int implicit_residual_differential_coding_enabled_flag = 0;
    int residual_adaptive_color_transform_enabled_flag = 0;
    int pps_slice_chroma_qp_offsets_present_flag = 0;
    int slice_appended_flag = 0;
    int slice_segment_appended_flag = 0;
    int picture_appended_flag = 0;
    
    // Additional fields used by parser
    int redundant_pic_cnt_present_flag = 0;
    int transform_skip_enabled_flag = 0;
    int cu_qp_offset_enabled_flag = 0;
    int pps_cb_offset = 0;
    int pps_cr_offset = 0;
    
    int PicWidth() const { return 0; }
    int PicHeight() const { return 0; }
};

// --------------------------------------------------------------------------
// AV1 码流信息
// --------------------------------------------------------------------------
struct Av1FilmGrainInfo {
    bool present = false;
    
    bool film_grain_params_present_flag = false;
    bool film_grain_remove_flag = false;
    int film_grain_model_id = 0;
    int scaling_shift_minus8 = 0;
    int ar_coeff_lag = 0;
    int ar_coeff_quant = 0;
    int grain_scaling_shift = 0;
    int semi_random_seed_dist_change_mode = 0;
    uint32_t film_grain_seed = 0;
    
    // AR coefficients
    std::vector<int> ar_coeffs_luma;
    std::vector<int> ar_coeffs_chroma;
    
    // Multipliers and offsets
    std::vector<int> multipliers;
    std::vector<int> clipper_lookups;
};

struct Av1ColorConfigInfo {
    bool present = false;
    
    bool high_bitdepth = false;
    bool twelve_bit = false;
    bool monochrome = false;
    bool color_description_present_flag = false;
    
    int color_primaries = 0;        // AOM_COLOR_PRIMARIES_*
    int transfer_characteristics = 0; // AOM_TRANSFER_CHARACTERISTICS_*
    int matrix_coefficients = 0;    // AOM_MATRIX_COEFFICIENTS_*
    int full_range_flag = 0;
    
    int color_bit_depth = 0;        // Actual bit depth
    int luma_bit_depth = 0;
    int chroma_bit_depth = 0;
    
    int subsampling_x = 0;          // 0=4:4:4, 1=4:2:2, 2=4:2:0
    int subsampling_y = 0;
    bool separate_uv_delta = false;
    
    // 仅当 subsampling_x && subsampling_y 时出现在码流里
    // 0=UNKNOWN, 1=VERTICAL(H.264 chroma_loc=2), 2=COLOCATED, 3=RESERVED
    int chroma_sample_position = 0;
    
    bool use_127_input_colorspace = false;
    
    // 位深推导（AV1 规范 color_config 之后的 BitDepth 语义）：
    //   seq_profile == 2 && high_bitdepth  -> twelve_bit ? 12 : 10
    //   其它                                -> high_bitdepth ? 10 : 8
    int BitDepth(int seq_profile) const {
        if (seq_profile == 2 && high_bitdepth) return twelve_bit ? 12 : 10;
        return high_bitdepth ? 10 : 8;
    }
};

struct Av1SequenceHeaderInfo {
    bool present = false;
    
    // Profile and level
    int profile = 0;                // seq_profile: 0..2
    int level = 0;                  // seq_level_idx_0: 0..23（2.0 ~ 7.3，每 4 档一档主版本）
    int tier = 0;                   // 0=Main, 1=High
    
    // 序列头前段
    bool still_picture = false;
    bool reduced_still_picture_header = false;
    
    // 编码工具开关
    bool use_128x128_superblock = false;
    bool enable_filter_intra = false;
    bool enable_intra_edge_filter = false;
    bool enable_interintra_compound = false;
    bool enable_masked_compound = false;
    bool enable_warped_motion = false;
    bool enable_dual_filter = false;
    bool enable_order_hint = false;
    bool enable_jnt_comp = false;
    bool enable_ref_frame_mvs = false;
    bool enable_superres = false;
    bool enable_cdef = false;
    bool enable_restoration = false;
    int seq_force_screen_content_tools = 0;  // 0=SELECT_SCREEN_CONTENT_TOOLS, 1..2=force
    int seq_force_integer_mv = 0;            // 同上语义，仅在 screen content tools==2 时有效
    int order_hint_bits_minus_1 = 0;
    
    bool film_grain_params_present = false;
    
    // Frame size
    int frame_width_minus_1 = 0;
    int frame_height_minus_1 = 0;
    
    int FrameWidth() const { return frame_width_minus_1 + 1; }
    int FrameHeight() const { return frame_height_minus_1 + 1; }
    
    // AV1 的 seq_level_idx 是 0..23，映射规则：主版本 = 2 + idx/4，次版本 = idx%4
    // 例：0 -> "2.0"，4 -> "3.0"，19 -> "6.3"，23 -> "7.3"，24 -> "Unknown"
    std::string LevelString() const {
        if (level < 0 || level > 23) return "Unknown";
        return std::to_string(2 + level / 4) + "." + std::to_string(level % 4);
    }
    
    // 位深：优先用 color_config 推导，退化到 input_bit_depth / bit_depth_minus_8
    int BitDepth() const {
        int d = color_config.BitDepth(profile);
        if (d > 0) return d;
        if (input_bit_depth > 0) return input_bit_depth;
        if (bit_depth_minus_8 > 0) return bit_depth_minus_8 + 8;
        return 8;
    }
    
    std::string ChromaFormatString() const {
        if (color_config.monochrome) return "4:0:0";
        if (color_config.subsampling_x && color_config.subsampling_y) return "4:2:0";
        if (color_config.subsampling_x && !color_config.subsampling_y) return "4:2:2";
        return "4:4:4";
    }
    
    // Bit depth
    int bit_depth_minus_8 = 0;
    int input_bit_depth = 0;        // Actual = value + 8
    
    // Initial presentation delay
    int initial_presentation_delay_bits = 0;
    
    // Timing
    bool timing_info_present_flag = false;
    uint32_t num_units_in_tick = 0;
    uint32_t timescale = 0;
    uint32_t num_ticks_per_picture = 0;
    uint32_t min_update_interval = 0;
    uint32_t target_picture_duration = 0;
    
    // 解析后续语法元素需要的中间状态（不是展示字段）
    bool decoder_model_info_present_flag = false;   // decoder_model_info_present
    int decoder_buffer_delay_length = 0;            // 位宽，operating point 里要用
    bool display_model_info_present_flag = false;   // display_model_info_present
    int operating_points_count = 1;
    
    // Color config
    Av1ColorConfigInfo color_config;
    
    // Film grain
    Av1FilmGrainInfo film_grain;
    
    // HDR metadata
    bool has_mastering_display = false;
    double mastering_red_x = 0.0, mastering_red_y = 0.0;
    double mastering_green_x = 0.0, mastering_green_y = 0.0;
    double mastering_blue_x = 0.0, mastering_blue_y = 0.0;
    double mastering_white_x = 0.0, mastering_white_y = 0.0;
    double mastering_max_luminance = 0.0;
    double mastering_min_luminance = 0.0;
    
    bool has_content_light = false;
    unsigned content_light_max = 0;
    unsigned content_light_average = 0;
};

// av1C 配置记录里能拿到的信息（AV1 ISOBMFF 规范 2.3）
//
// MP4 / WebM 里 AV1 的 extradata 通常**只有 av1C**，不含序列头 OBU。
// av1C 里没有分辨率，也没有 color_primaries / transfer / matrix
// （color_description 只出现在序列头里），所以宽高与色彩只能保持未知。
struct Av1CodecConfigInfo {
    bool present = false;
    
    int profile = 0;        // seq_profile 0..2
    int level = 0;          // seq_level_idx_0 0..23
    int tier = 0;           // 0=Main, 1=High
    int bit_depth = 8;
    bool monochrome = false;
    int subsampling_x = 1;
    int subsampling_y = 1;
    int chroma_sample_position = 0;
    int color_range = 1;    // 1 = full range
    int initial_presentation_delay = 0;
    
    std::string LevelString() const {
        if (level < 0 || level > 23) return "Unknown";
        return std::to_string(2 + level / 4) + "." + std::to_string(level % 4);
    }
    
    std::string ChromaFormatString() const {
        if (monochrome) return "4:0:0";
        if (subsampling_x && subsampling_y) return "4:2:0";
        if (subsampling_x && !subsampling_y) return "4:2:2";
        return "4:4:4";
    }
};

// --------------------------------------------------------------------------
// VVC (H.266) 码流信息
//
// 语法顺序以 FFmpeg 的 libavcodec/cbs_h266_syntax_template.c 为准（CBS 层逐位
// 解析，与 H.266 规范 7.3.2.x 一一对应）。字段命名沿用规范里的语法元素名。
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

// --------------------------------------------------------------------------
// 统一码流信息汇总
// --------------------------------------------------------------------------
struct BitstreamAnalysisResult {
    bool analyzed = false;
    int stream_index = -1;
    std::string codec_name;           // "h264", "hevc", "av1", "vvc"
    std::string codec_id_str;         // "AV_CODEC_ID_H264" 等
    
    // 基本视频属性（从码流解析）
    int width = 0;
    int height = 0;
    int bit_depth = 8;
    int color_primaries = 0;
    int transfer_characteristics = 0;
    int matrix_coefficients = 0;

    // 容器层（AVCodecParameters）的同名字段快照，供 UI 对比表两侧并列展示。
    // 由 BitstreamAnalyzer::SetContainerMetadata() 在 Analyze() 时写入；
    // 调用方没设置容器 metadata 时 has_container 为 false，各字段保持 0。
    bool has_container = false;
    int container_width = 0;
    int container_height = 0;
    int container_bit_depth = 0;
    int container_color_primaries = 0;
    int container_transfer_characteristics = 0;
    int container_matrix_coefficients = 0;
    int container_color_range = 0;
    
    // 各编码类型的解析结果（最多只有一个为 true）
    bool has_h264 = false;
    bool has_hevc = false;
    bool has_av1 = false;
    bool has_vvc = false;
    
    // H.264 信息
    H264SpsInfo h264_sps;
    H264PpsInfo h264_pps;
    
    // H.265 信息
    HevcVpsInfo hevc_vps;
    HevcSpsInfo hevc_sps;
    HevcPpsInfo hevc_pps;
    
    // AV1 信息
    Av1SequenceHeaderInfo av1_seq_header;
    Av1CodecConfigInfo av1_config;      // av1C 里解析出的（序列头缺失时的兜底）
    bool has_av1_config = false;
    
    // VVC 信息
    VvcVpsInfo vvc_vps;
    VvcSpsInfo vvc_sps;
    VvcPpsInfo vvc_pps;
    VvcCodecConfigInfo vvc_config;  // vvcC 里解析出的（SPS/VPS 都缺 PTL 时的兜底）
    bool has_vvc_config = false;
    
    // 提取的 NAL/OBU 列表
    std::vector<utils::NalUnit> nal_units;
    std::vector<utils::ObuUnit> obu_units;
    
    // 不一致警告（与容器 metadata 对比）
    struct Inconsistency {
        std::string field;             // 字段名
        std::string container_value;   // 容器中的值
        std::string bitstream_value;   // 码流中的值
        std::string severity;          // "warning" / "error" / "info"
        std::string description;       // 详细描述
        std::string suggestion;        // 建议
    };
    
    std::vector<Inconsistency> inconsistencies;
    
    // 添加不一致警告
    void AddInconsistency(const std::string& field,
                         const std::string& container_value,
                         const std::string& bitstream_value,
                         const std::string& description,
                         const std::string& suggestion = "");
    
    // 摘要信息
    std::string Summary() const;
    
    // JSON 格式输出
    std::string ToJson() const;
};

} // namespace model
} // namespace videoeye
