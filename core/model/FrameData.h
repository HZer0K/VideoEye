#pragma once

#include <string>
#include <memory>
#include <array>
#include <vector>
#include <cstdint>

namespace videoeye {
namespace model {

// 播放器状态枚举
enum class PlayerState {
    Idle,
    Loading,
    Playing,
    Paused,
    Stopped,
    Error
};

// 流信息结构体
struct StreamInfo {
    std::string filename;

    struct ExtractorFormat {
        std::string complete_name;
        std::string format;
        std::string format_profile;
        std::string codec_id;
        std::string file_size;
        std::string duration;
        std::string overall_bit_rate_mode;
        std::string overall_bit_rate;
        std::string frame_rate;
        std::string description;
        std::string writing_application;
    } extractor;

    struct AudioCodecFormat {
        std::string id;
        std::string format;
        std::string format_info;
        std::string codec_id;
        std::string duration;
        std::string bit_rate_mode;
        std::string bit_rate;
        std::string channels;
        std::string channel_layout;
        std::string sampling_rate;
        std::string frame_rate;
        std::string compression_mode;
        std::string stream_size;
        std::string is_default;
        std::string alternate_group;
    } audio;
    
    struct VideoCodecFormat {
        std::string id;
        std::string format;
        std::string format_info;
        std::string format_profile;
        std::string format_settings;
        std::string format_settings_cabac;
        std::string format_settings_reference_frames;
        std::string hdr_format;
        std::string hdr_format_compatibility;
        std::string codec_id;
        std::string codec_id_info;
        std::string duration;
        std::string bit_rate;
        std::string width;
        std::string height;
        std::string display_aspect_ratio;
        std::string frame_rate_mode;
        std::string frame_rate;
        std::string color_space;
        std::string chroma_subsampling;
        std::string bit_depth;
        std::string scan_type;
        std::string bits_pixel_frame;
        std::string stream_size;
        std::string color_range;
        std::string color_primaries;
        std::string transfer_characteristics;
        std::string matrix_coefficients;
        std::string mastering_display_color_primaries;
        std::string mastering_display_luminance;
        std::string maximum_content_light_level;
        std::string maximum_frame_average_light_level;
        std::string codec_configuration_box;
    } video;

    std::string ToString() const;
};

// 帧数据结构
struct FrameData {
    uint8_t* data[8] = {nullptr};
    int linesize[8] = {0};
    int width = 0;
    int height = 0;
    int format = -1;
    int64_t pts = 0;
    double timestamp = 0.0;

    std::array<std::vector<uint8_t>, 8> owned;
    
    ~FrameData();
    void Clear();
    void CopyFrom(const FrameData& other);
};

} // namespace model
} // namespace videoeye
