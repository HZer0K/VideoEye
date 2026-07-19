#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <memory>
#include <string>
#include <vector>
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
    
    // 初始化解码器 (软件解码)
    bool Initialize(AVCodecParameters* codec_params);
    
    // 从已有的AVCodecContext初始化（接管所有权）
    bool InitializeFromContext(AVCodecContext* codec_ctx);

    // 硬件解码初始化: 尝试使用指定 HW 设备类型, 失败则回退软件解码
    bool InitializeWithHw(AVCodecParameters* codec_params, AVHWDeviceType hw_type);

    // 获取当前使用的硬件设备类型 (AV_HWDEVICE_TYPE_NONE 表示软件解码)
    AVHWDeviceType GetHwDeviceType() const { return hw_device_type_; }
    bool IsHardwareDecoding() const { return hw_device_type_ != AV_HWDEVICE_TYPE_NONE; }

    // 查询当前平台可用的硬件加速设备类型列表
    static std::vector<AVHWDeviceType> GetAvailableHwDeviceTypes();

    // === Vulkan 帧访问 ===

    // 获取当前解码帧的 AVFrame 指针 (零拷贝模式使用)
    // 返回的 AVFrame 仅在下次 ReceiveFrame 调用前有效
    const AVFrame* GetLastRawFrame() const { return frame_; }

    // 当前帧是否为 Vulkan 格式
    bool IsCurrentFrameVulkan() const {
        return frame_ && frame_->format == AV_PIX_FMT_VULKAN;
    }

    // 设置零拷贝模式 (不下载到 CPU, 由外部直接读取 VkImage)
    void SetZeroCopyEnabled(bool enable) { zero_copy_enabled_ = enable; }
    bool IsZeroCopyEnabled() const { return zero_copy_enabled_; }

    // 使用外部 Vulkan 设备上下文初始化 (与 InitializeWithHw 互斥)
    bool InitializeWithVulkanDevice(AVCodecParameters* codec_params,
                                     AVBufferRef* vk_device_ctx);

    // 解码流程拆分：先送包，再持续取出所有可用帧。
    bool SendPacket(AVPacket* packet);
    bool ReceiveFrame(model::FrameData& output_frame);
    
    // 解码一帧
    bool DecodePacket(AVPacket* packet, model::FrameData& output_frame);
    
    // 获取解码器信息
    std::string GetCodecName() const;
    AVCodecID GetCodecId() const;
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    AVPictureType GetLastPictureType() const { return last_pict_type_; }

    // 清空解码器内部缓冲 (定位后丢弃旧参考帧/B帧, 避免花屏与过期帧)
    void Flush();

    // 关闭解码器
    void Close();
    
private:
    // 将硬件帧下载到系统内存 (输出到 sw_frame)
    bool DownloadHwFrame(AVFrame* sw_frame);

    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;   // 硬件设备上下文
    AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;  // 硬件像素格式
    bool zero_copy_enabled_ = false;
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

    // 解码流程拆分：先送包，再持续取出所有可用帧。
    bool SendPacket(AVPacket* packet);
    bool ReceiveFrame(uint8_t* output_buffer, int buffer_size, int& output_size);
    
    // 解码一帧
    bool DecodePacket(AVPacket* packet, uint8_t* output_buffer, int buffer_size, int& output_size);
    
    // 获取解码器信息
    std::string GetCodecName() const;
    int GetSampleRate() const { return sample_rate_; }
    int GetChannels() const { return channels_; }

    // 清空解码器内部缓冲 (定位后丢弃旧参考帧, 避免过期音频输出)
    void Flush();
    int64_t GetLastFramePts() const { return last_frame_pts_; }
    int GetLastFrameSampleCount() const { return last_frame_sample_count_; }
    int GetLastFrameSampleRate() const { return last_frame_sample_rate_; }
    int GetLastFrameChannels() const { return last_frame_channels_; }
    int GetLastOutputSize() const { return last_output_size_; }
    
    // 关闭解码器
    void Close();
    
private:
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    int64_t last_frame_pts_ = AV_NOPTS_VALUE;
    int last_frame_sample_count_ = 0;
    int last_frame_sample_rate_ = 0;
    int last_frame_channels_ = 0;
    int last_output_size_ = 0;
};

} // namespace player
} // namespace videoeye
