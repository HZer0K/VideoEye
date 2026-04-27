#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <memory>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

#include "core/model/FrameData.h"

namespace videoeye {
namespace player {

// 视频解码器类
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    
    // 初始化解码器
    bool Initialize(AVCodecParameters* codec_params);
    
    // 从已有的AVCodecContext初始化（接管所有权）
    bool InitializeFromContext(AVCodecContext* codec_ctx);
    
    // 解码一帧
    bool DecodePacket(AVPacket* packet, model::FrameData& output_frame);
    
    // 获取解码器信息
    std::string GetCodecName() const;
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    AVPictureType GetLastPictureType() const { return last_pict_type_; }
    
    // 关闭解码器
    void Close();
    
private:
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    AVPictureType last_pict_type_ = AV_PICTURE_TYPE_NONE;
};

// 音频解码器类
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    
    // 初始化解码器
    bool Initialize(AVCodecParameters* codec_params);
    
    // 解码一帧
    bool DecodePacket(AVPacket* packet, uint8_t* output_buffer, int buffer_size, int& output_size);
    
    // 获取解码器信息
    std::string GetCodecName() const;
    int GetSampleRate() const { return sample_rate_; }
    int GetChannels() const { return channels_; }
    
    // 关闭解码器
    void Close();
    
private:
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
};

} // namespace player
} // namespace videoeye
