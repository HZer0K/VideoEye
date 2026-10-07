#pragma once

// AV1 码流解析结果：film grain / color config / sequence header / av1C 四组纯值类型。
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

} // namespace model
} // namespace videoeye