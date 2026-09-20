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
    
    // Color info
    bool neutral_chroma_indication = false;
    bool field_seq_flag = false;
    bool frame_mbs_only_flag = true;
    
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
    
    // Chroma format
    int chroma_format_idc = 0;        // 0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4
    bool separate_colour_plane_flag = false;
    int bit_depth_luma_minus8 = 0;    // actual = value + 8
    int bit_depth_chroma_minus8 = 0;  // actual = value + 8
    
    // GOP structure
    int qpprime_y_zero_transform_bypass_flag = 0;
    int seq_scaling_matrix_present_flag = 0;
    int log2_max_pic_order_cnt_lsb_minus4 = 0;
    int max_num_ref_frames = 0;       // 最大参考帧数
    int gaps_in_frame_val_allowed_flag = 0;
    
    // Frame properties
    int pic_width_in_mbs_minus1 = 0;  // (value + 1) * 16 = width
    int pic_height_in_mbs_minus1 = 0; // (value + 1) * 16 = height (double for field)
    bool direct_8x8_inference_flag = false;
    
    // Frame cropping
    int frame_cropping_flag = 0;
    int frame_crop_left_offset = 0;
    int frame_crop_right_offset = 0;
    int frame_crop_top_offset = 0;
    int frame_crop_bottom_offset = 0;
    
    // VUI 信息（如果存在）
    H264VuiInfo vui;
    
    // 计算出的宽高
    int width() const { return (pic_width_in_mbs_minus1 + 1) * 16; }
    int height() const { 
        return (2 - (vui.frame_mbs_only_flag ? 1 : 0)) * (pic_height_in_mbs_minus1 + 1) * 16;
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
    int vps_profile_level_tier = 0;  // 高位 2 bits: profile, 低位：tier
    int vps_max_layers = 0;
    int vps_max_sub_layers = 0;
    int vps_temporal_nal_layer_only_flag = 0;
    int vps_ptl_following_flag = 0;
    
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
    
    // PTL (Profile Tier Level)
    int general_profile_idc = 0;
    int general_tier_flag = 0;
    int chroma_format_idc = 0;      // 0=4:0:0, 1=4:2:0, 2=4:2:2, 3=4:4:4
    int separate_colour_plane_flag = 0;
    int pic_width_in_ctu_minus1 = 0; // CTU size default 64, (value+1)*CTU = width
    int pic_height_in_ctu_minus1 = 0;
    
    // Bit depth
    int bit_depth_luma_minus8 = 0;
    int bit_depth_chroma_minus8 = 0;
    
    // Chroma config
    int log2_max_pic_order_cnt_lsb_minus4 = 0;
    int sub_layer_ordering_info_present_flag = 0;
    int log2_min_luma_coding_block_size_minus3 = 0;
    int log2_diff_max_min_luma_coding_block_size = 0;
    int log2_min_luma_transform_block_size_minus3 = 0;
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
    int sar_width = 0;
    int sar_height = 0;
    int video_full_range_flag = 0;
    int colour_primaries = 0;
    int transfer_characteristics = 0;
    int matrix_coefficients = 0;
    int chroma_sample_loc_type_top_field = 0;
    int chroma_sample_loc_type_bottom_field = 0;
    int def_disp_win_left_offset = 0;
    int def_disp_win_right_offset = 0;
    int def_disp_win_top_offset = 0;
    int def_disp_win_bottom_offset = 0;
    int vui_timing_info_present_flag = 0;
    uint32_t vui_num_units_in_tick = 0;
    uint32_t vui_time_scale = 0;
    int bitstream_restriction_flag = 0;
    int max_bytes_per_pic_denom = 0;
    int max_bits_per_min_cu_denom = 0;
    int log2_max_mv_length_horizontal = 0;
    int log2_max_mv_length_vertical = 0;
    
    // Frame properties
    int width() const {
        int ctu_size = 64; // Default CTU size
        return (pic_width_in_ctu_minus1 + 1) * ctu_size;
    }
    
    int height() const {
        int ctu_size = 64; // Default CTU size
        return (pic_height_in_ctu_minus1 + 1) * ctu_size;
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
    int weighted_ppred_flag = 0;
    int weighted_bi_pred_flag = 0;
    int transquant_bypass_enabled_flag = 0;
    int tiles_enabled_flag = 0;
    int entropy_coding_sync_enabled_flag = 0;
    int independent_slice_flag = 0;
    int constrained_intra_pred_flag = 0;
    int transform_skip_rotation_enabled_flag = 0;
    int transform_skip_context_enabled_flag = 0;
    int implicit_residual_differential_coding_enabled_flag = 0;
    int residual_adaptive_color_transform_enabled_flag = 0;
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
    
    bool use_127_input_colorspace = false;
};

struct Av1SequenceHeaderInfo {
    bool present = false;
    
    // Profile and level
    int profile = 0;                // 0=Profile 0, 1=Profile 1, 2=Profile 2
    int level = 0;                  // 0..63 (actual level = value / 2)
    int tier = 0;                   // 0=Main, 1=High
    
    // Frame size
    int frame_width_minus_1 = 0;
    int frame_height_minus_1 = 0;
    
    int FrameWidth() const { return frame_width_minus_1 + 1; }
    int FrameHeight() const { return frame_height_minus_1 + 1; }
    
    // Bit depth
    int bit_depth_minus_8 = 0;
    int input_bit_depth = 0;        // Actual = value + 8
    
    // Initial presentation delay
    int initial_presentation_delay_bits = 0;
    
    // Timing
    bool timing_info_present_flag = false;
    uint32_t timescale = 0;
    uint32_t num_ticks_per_picture = 0;
    uint32_t min_update_interval = 0;
    uint32_t target_picture_duration = 0;
    
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

// --------------------------------------------------------------------------
// VVC 码流信息（简化版）
// --------------------------------------------------------------------------
struct VvcNalUnitInfo {
    bool present = false;
    
    int nal_unit_type = 0;
    int temporal_id = 0;
    int nuh_layer_id = 0;
    
    bool is_vps = false;
    bool is_vps_extension = false;
};

struct VvcVpsInfo {
    bool present = false;
    
    int vps_video_parameter_set_id = 0;
    int vps_max_layers = 0;
    int vps_max_sub_layers = 0;
    int vps_reserved_zero_2bits = 0;
    int vps_num_ptl = 0;
    
    // PTL
    int general_profile_space = 0;
    int general_tier_flag = 0;
    int general_profile_idc = 0;
    uint32_t general_level_idc = 0;
    
    // Video format
    int vps_video_format = 0;
    
    // Color config
    int vps_chroma_format_idc = 0;
    int vps_bit_depth_luma_minus8 = 0;
    int vps_bit_depth_chroma_minus8 = 0;
};

struct VvcSpsInfo {
    bool present = false;
    
    int sps_seq_parameter_set_id = 0;
    int sps_num_dss = 0;
    
    // PTL
    int general_profile_idc = 0;
    int general_tier_flag = 0;
    uint32_t general_level_idc = 0;
    
    // Chroma
    int sps_chroma_format_idc = 0;
    
    // Bit depth
    int sps_bit_depth_luma_minus8 = 0;
    int sps_bit_depth_chroma_minus8 = 0;
    
    // Frame size
    int sps_log2_diff_max_min_coding_block_size = 0;
    int sps_min_coding_block_size = 0;
    int sps_max_tree_size = 0;
    int sps_min_partition_size = 0;
    
    int Width() const {
        int cb_size = 1 << sps_min_coding_block_size;
        // Need more context to calculate exact width
        return 0;
    }
    
    int Height() const {
        int cb_size = 1 << sps_min_coding_block_size;
        // Need more context to calculate exact height
        return 0;
    }
    
    int BitDepthLuma() const { return sps_bit_depth_luma_minus8 + 8; }
    int BitDepthChroma() const { return sps_bit_depth_chroma_minus8 + 8; }
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
    
    // VVC 信息
    VvcVpsInfo vvc_vps;
    VvcSpsInfo vvc_sps;
    
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
