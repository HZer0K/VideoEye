#pragma once

// H.265 (HEVC) 码流解析结果：SEI / VPS / SPS / PPS 四组纯值类型。
//
// 从 BitstreamInfo.h 按 codec 拆出，使每个编解码器的结构自成一文件；
// BitstreamInfo.h 作为 umbrella 重新 include 本头，调用方无需改动。
// 本文件只依赖标准库，不依赖其它 codec 子头。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

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

} // namespace model
} // namespace videoeye