#pragma once

// H.264 (AVC) 码流解析结果：VUI / SPS / PPS 三组纯值类型。
//
// 从 BitstreamInfo.h 按 codec 拆出，使每个编解码器的结构自成一文件；
// BitstreamInfo.h 作为 umbrella 重新 include 本头，调用方无需改动。
// 本文件只依赖标准库，不依赖其它 codec 子头。

#include <cstdint>
#include <string>

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

} // namespace model
} // namespace videoeye