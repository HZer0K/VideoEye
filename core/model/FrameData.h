#pragma once

#include <string>
#include <memory>

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
    std::string format_name;
    std::string codec_name_video;
    std::string codec_name_audio;
    
    int duration_ms = 0;
    int video_width = 0;
    int video_height = 0;
    double video_fps = 0.0;
    int video_bitrate = 0;
    
    int audio_sample_rate = 0;
    int audio_channels = 0;
    int audio_bitrate = 0;
    
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
    
    ~FrameData();
    void CopyFrom(const FrameData& other);
};

} // namespace model
} // namespace videoeye
