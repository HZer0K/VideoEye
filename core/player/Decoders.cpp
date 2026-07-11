#include "Decoders.h"
#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavcodec/codec.h>
}

namespace videoeye {
namespace player {

// VideoDecoder 实现
VideoDecoder::VideoDecoder() {
    frame_ = av_frame_alloc();
}

VideoDecoder::~VideoDecoder() {
    Close();
    if (frame_) {
        av_frame_free(&frame_);
    }
}

bool VideoDecoder::Initialize(AVCodecParameters* codec_params) {
    if (!codec_params) {
        std::cerr << "Invalid codec parameters" << std::endl;
        return false;
    }
    
    // 查找解码器
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec" << std::endl;
        return false;
    }
    
    // 分配解码器上下文
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "Failed to allocate codec context" << std::endl;
        return false;
    }
    
    // 复制参数
    int ret = avcodec_parameters_to_context(codec_ctx_, codec_params);
    if (ret < 0) {
        std::cerr << "Failed to copy codec parameters" << std::endl;
        return false;
    }
    
    // 导出运动矢量 side data (供宏块分析使用)
    codec_ctx_->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;

    // 打开解码器
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to open codec" << std::endl;
        return false;
    }
    
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    
    return true;
}

bool VideoDecoder::InitializeFromContext(AVCodecContext* codec_ctx) {
    // 接管已打开的codec context的所有权
    codec_ctx_ = codec_ctx;
    
    if (!codec_ctx_) {
        std::cerr << "Invalid codec context" << std::endl;
        return false;
    }
    
    // 分配帧缓冲区（构造函数已分配；这里避免覆盖导致泄漏）
    if (!frame_) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            std::cerr << "Failed to allocate frame" << std::endl;
            return false;
        }
    }
    
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    
    return true;
}

std::vector<AVHWDeviceType> VideoDecoder::GetAvailableHwDeviceTypes() {
    std::vector<AVHWDeviceType> types;
    // 按优先级遍历常用硬件加速方法
    static const AVHWDeviceType kPreferredTypes[] = {
        AV_HWDEVICE_TYPE_VULKAN,       // 最高优先级 (可零拷贝渲染)
        AV_HWDEVICE_TYPE_VAAPI,        // Linux AMD/Intel
        AV_HWDEVICE_TYPE_CUDA,         // NVIDIA
        AV_HWDEVICE_TYPE_VDPAU,        // NVIDIA (legacy)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX, // macOS/iOS
        AV_HWDEVICE_TYPE_D3D11VA,      // Windows
        AV_HWDEVICE_TYPE_DXVA2,        // Windows (legacy)
        AV_HWDEVICE_TYPE_QSV,          // Intel Quick Sync
    };

    // 探测时临时降低 FFmpeg 日志级别，避免 Vulkan 驱动不兼容的错误刷屏
    int old_level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);

    for (auto type : kPreferredTypes) {
        AVBufferRef* test_ctx = nullptr;
        int ret = av_hwdevice_ctx_create(&test_ctx, type, nullptr, nullptr, 0);
        if (ret >= 0) {
            types.push_back(type);
            av_buffer_unref(&test_ctx);
        }
    }

    av_log_set_level(old_level);
    return types;
}

bool VideoDecoder::InitializeWithHw(AVCodecParameters* codec_params, AVHWDeviceType hw_type) {
    if (!codec_params) return false;

    // 查找支持硬件加速的解码器
    const AVCodec* codec = nullptr;
    void* iter = nullptr;
    while ((codec = av_codec_iterate(&iter)) != nullptr) {
        if (!av_codec_is_decoder(codec)) continue;  // 只查解码器
        if (codec->id != codec_params->codec_id) continue;
        // 检查该解码器是否支持指定的 HW 设备类型
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (!config) break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == hw_type) {
                goto found_codec;
            }
        }
        codec = nullptr;
    }
found_codec:
    if (!codec) {
        std::cerr << "No HW decoder for codec " << avcodec_get_name(codec_params->codec_id)
                  << " with device type " << av_hwdevice_get_type_name(hw_type) << std::endl;
        return false;
    }

    // 创建硬件设备上下文
    int ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_type, nullptr, nullptr, 0);
    if (ret < 0) {
        std::cerr << "Failed to create HW device context" << std::endl;
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "Failed to allocate HW codec context" << std::endl;
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    ret = avcodec_parameters_to_context(codec_ctx_, codec_params);
    if (ret < 0) {
        Close();
        return false;
    }

    // 查找解码器支持的 HW 像素格式
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        if (config->device_type == hw_type) {
            hw_pix_fmt_ = config->pix_fmt;
            break;
        }
    }
    if (hw_pix_fmt_ == AV_PIX_FMT_NONE) {
        Close();
        return false;
    }

    // 设置硬件设备上下文和 get_format 回调
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->get_format = [](AVCodecContext*, const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
        for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
            if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                return *p;
            }
        }
        return pix_fmts[0];
    };

    // 导出运动矢量 side data (供宏块分析使用)
    codec_ctx_->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        Close();
        return false;
    }

    hw_device_type_ = hw_type;
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    return true;
}

bool VideoDecoder::InitializeWithVulkanDevice(AVCodecParameters* codec_params,
                                               AVBufferRef* vk_device_ctx) {
    if (!codec_params || !vk_device_ctx) return false;

    // 查找支持 Vulkan 的解码器
    const AVCodec* codec = nullptr;
    void* iter = nullptr;
    while ((codec = av_codec_iterate(&iter)) != nullptr) {
        if (!av_codec_is_decoder(codec)) continue;
        if (codec->id != codec_params->codec_id) continue;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (!config) break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == AV_HWDEVICE_TYPE_VULKAN) {
                goto found_vulkan_codec;
            }
        }
        codec = nullptr;
    }
found_vulkan_codec:
    if (!codec) {
        std::cerr << "No Vulkan HW decoder for codec "
                  << avcodec_get_name(codec_params->codec_id) << std::endl;
        return false;
    }

    // 使用外部设备上下文 (不自己创建)
    hw_device_ctx_ = av_buffer_ref(vk_device_ctx);
    if (!hw_device_ctx_) {
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    int ret = avcodec_parameters_to_context(codec_ctx_, codec_params);
    if (ret < 0) {
        Close();
        return false;
    }

    // 查找解码器支持的 Vulkan HW 像素格式
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        if (config->device_type == AV_HWDEVICE_TYPE_VULKAN) {
            hw_pix_fmt_ = config->pix_fmt;
            break;
        }
    }
    if (hw_pix_fmt_ == AV_PIX_FMT_NONE) {
        Close();
        return false;
    }

    // 设置硬件设备上下文和 get_format 回调
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->get_format = [](AVCodecContext*, const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
        for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
            if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                return *p;
            }
        }
        return pix_fmts[0];
    };

    // 导出运动矢量 side data (供宏块分析使用)
    codec_ctx_->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        Close();
        return false;
    }

    hw_device_type_ = AV_HWDEVICE_TYPE_VULKAN;
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    return true;
}

bool VideoDecoder::DownloadHwFrame(AVFrame* sw_frame) {
    AVFrame* tmp = av_frame_alloc();
    if (!tmp) return false;

    int ret = av_hwframe_transfer_data(tmp, frame_, 0);
    if (ret < 0) {
        av_frame_free(&tmp);
        return false;
    }
    av_frame_copy_props(tmp, frame_);
    av_frame_move_ref(sw_frame, tmp);
    av_frame_free(&tmp);
    return true;
}

bool VideoDecoder::SendPacket(AVPacket* packet) {
    if (!codec_ctx_ || !frame_) {
        return false;
    }

    int ret = avcodec_send_packet(codec_ctx_, packet);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            return false;
        }
        std::cerr << "Error sending packet to decoder" << std::endl;
        return false;
    }

    return true;
}

bool VideoDecoder::ReceiveFrame(model::FrameData& output_frame) {
    if (!codec_ctx_ || !frame_) {
        return false;
    }

    int ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return false;
        }
        std::cerr << "Error receiving frame from decoder" << std::endl;
        return false;
    }

    last_pict_type_ = frame_->pict_type;

    // 硬件帧需要下载到系统内存
    AVFrame* src_frame = frame_;
    AVFrame* sw_frame = nullptr;
    if (frame_->format == AV_PIX_FMT_NONE || av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame_->format)) == nullptr) {
        // 未知格式, 跳过
    } else {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame_->format));
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            // 零拷贝模式: Vulkan 帧不下载, 仅填充元数据
            if (zero_copy_enabled_ && frame_->format == AV_PIX_FMT_VULKAN) {
                output_frame.Clear();
                output_frame.width = frame_->width;
                output_frame.height = frame_->height;
                output_frame.format = frame_->format;  // AV_PIX_FMT_VULKAN
                if (frame_->pts != AV_NOPTS_VALUE) {
                    output_frame.pts = frame_->pts;
                } else if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
                    output_frame.pts = frame_->best_effort_timestamp;
                } else {
                    output_frame.pts = AV_NOPTS_VALUE;
                }
                if (output_frame.pts != AV_NOPTS_VALUE && codec_ctx_->time_base.den != 0) {
                    output_frame.timestamp = output_frame.pts * av_q2d(codec_ctx_->time_base);
                } else {
                    output_frame.timestamp = 0.0;
                }
                // 不复制像素数据 — VulkanRenderer 通过 GetLastRawFrame() 访问
                return true;
            }

            // 这是硬件帧, 需要下载
            sw_frame = av_frame_alloc();
            if (!sw_frame || !DownloadHwFrame(sw_frame)) {
                av_frame_free(&sw_frame);
                return false;
            }
            src_frame = sw_frame;
        }
    }
    
    // 复制帧数据
    output_frame.Clear();

    output_frame.width = src_frame->width;
    output_frame.height = src_frame->height;
    output_frame.format = src_frame->format;
    if (src_frame->pts != AV_NOPTS_VALUE) {
        output_frame.pts = src_frame->pts;
    } else if (src_frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        output_frame.pts = src_frame->best_effort_timestamp;
    } else {
        output_frame.pts = AV_NOPTS_VALUE;
    }

    if (output_frame.pts != AV_NOPTS_VALUE && codec_ctx_->time_base.den != 0) {
        output_frame.timestamp = output_frame.pts * av_q2d(codec_ctx_->time_base);
    } else {
        output_frame.timestamp = 0.0;
    }

    const AVPixFmtDescriptor* desc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src_frame->format));

    for (int i = 0; i < 8; ++i) {
        if (!src_frame->data[i] || src_frame->linesize[i] <= 0) {
            continue;
        }

        int plane_height = src_frame->height;
        if (desc && (i == 1 || i == 2)) {
            plane_height = AV_CEIL_RSHIFT(src_frame->height, desc->log2_chroma_h);
        }

        int size = src_frame->linesize[i] * plane_height;
        if (size <= 0) {
            continue;
        }

        output_frame.linesize[i] = src_frame->linesize[i];
        output_frame.owned[i].resize(size);
        std::memcpy(output_frame.owned[i].data(), src_frame->data[i], size);
        output_frame.data[i] = output_frame.owned[i].data();
    }

    if (sw_frame) {
        av_frame_free(&sw_frame);
    }
    
    return true;
}

bool VideoDecoder::DecodePacket(AVPacket* packet, model::FrameData& output_frame) {
    if (!SendPacket(packet)) {
        return false;
    }
    return ReceiveFrame(output_frame);
}

std::string VideoDecoder::GetCodecName() const {
    if (codec_ctx_ && codec_ctx_->codec) {
        return codec_ctx_->codec->name;
    }
    return "unknown";
}

void VideoDecoder::Flush() {
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
    }
}

void VideoDecoder::Close() {
    if (codec_ctx_) {
        // 清除 hw_device_ctx 引用 (codec_ctx_ 持有自己的 ref)
        if (codec_ctx_->hw_device_ctx) {
            av_buffer_unref(&codec_ctx_->hw_device_ctx);
        }
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
}

// AudioDecoder 实现
AudioDecoder::AudioDecoder() {
    frame_ = av_frame_alloc();
}

AudioDecoder::~AudioDecoder() {
    Close();
    if (frame_) {
        av_frame_free(&frame_);
    }
}

bool AudioDecoder::Initialize(AVCodecParameters* codec_params) {
    if (!codec_params) {
        std::cerr << "Invalid codec parameters" << std::endl;
        return false;
    }
    
    // 查找解码器
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "Unsupported audio codec" << std::endl;
        return false;
    }
    
    // 分配解码器上下文
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "Failed to allocate audio codec context" << std::endl;
        return false;
    }
    
    // 复制参数
    int ret = avcodec_parameters_to_context(codec_ctx_, codec_params);
    if (ret < 0) {
        std::cerr << "Failed to copy audio codec parameters" << std::endl;
        return false;
    }
    
    // 打开解码器
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        std::cerr << "Failed to open audio codec" << std::endl;
        return false;
    }
    
    sample_rate_ = codec_ctx_->sample_rate;
    channels_ = codec_ctx_->ch_layout.nb_channels;
    if (channels_ <= 0) {
        channels_ = codec_params->ch_layout.nb_channels;
    }
    if (channels_ <= 0) {
        channels_ = 2;
    }
    
    // 初始化重采样上下文
    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) {
        std::cerr << "Failed to allocate resample context" << std::endl;
        return false;
    }
    
    AVChannelLayout in_layout{};
    if (codec_ctx_->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&in_layout, &codec_ctx_->ch_layout) < 0) {
            av_channel_layout_default(&in_layout, channels_);
        }
    } else {
        av_channel_layout_default(&in_layout, channels_);
    }

    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, channels_);

    swr_alloc_set_opts2(&swr_ctx_,
                        &out_layout, AV_SAMPLE_FMT_S16, sample_rate_ > 0 ? sample_rate_ : 44100,
                        &in_layout, codec_ctx_->sample_fmt, sample_rate_ > 0 ? sample_rate_ : 44100,
                        0, nullptr);
    
    ret = swr_init(swr_ctx_);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0) {
        std::cerr << "Failed to initialize resample context" << std::endl;
        return false;
    }
    
    return true;
}

bool AudioDecoder::SendPacket(AVPacket* packet) {
    if (!codec_ctx_ || !frame_ || !swr_ctx_) {
        return false;
    }

    int ret = avcodec_send_packet(codec_ctx_, packet);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            return false;
        }
        std::cerr << "Error sending audio packet" << std::endl;
        return false;
    }

    return true;
}

bool AudioDecoder::ReceiveFrame(uint8_t* output_buffer, int buffer_size, int& output_size) {
    if (!codec_ctx_ || !frame_ || !swr_ctx_) {
        return false;
    }

    int ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return false;
        }
        std::cerr << "Error receiving audio frame" << std::endl;
        return false;
    }

    last_frame_pts_ = frame_->pts;
    if (last_frame_pts_ == AV_NOPTS_VALUE) {
        last_frame_pts_ = frame_->best_effort_timestamp;
    }
    last_frame_sample_count_ = frame_->nb_samples;
    last_frame_sample_rate_ = frame_->sample_rate > 0 ? frame_->sample_rate : sample_rate_;
    last_frame_channels_ = frame_->ch_layout.nb_channels > 0 ? frame_->ch_layout.nb_channels : channels_;

    // 重采样
    uint8_t* output_ptrs[8] = {output_buffer, nullptr, nullptr, nullptr, 
                                nullptr, nullptr, nullptr, nullptr};
    if (channels_ <= 0) {
        return false;
    }
    output_size = swr_convert(swr_ctx_, output_ptrs, buffer_size / (channels_ * 2),
                              (const uint8_t**)frame_->data, frame_->nb_samples);
    
    if (output_size < 0) {
        std::cerr << "Error during audio resampling" << std::endl;
        return false;
    }
    
    output_size *= channels_ * 2; // 转换为字节数
    last_output_size_ = output_size;
    
    return true;
}

bool AudioDecoder::DecodePacket(AVPacket* packet, uint8_t* output_buffer, int buffer_size, int& output_size) {
    if (!SendPacket(packet)) {
        return false;
    }
    return ReceiveFrame(output_buffer, buffer_size, output_size);
}

std::string AudioDecoder::GetCodecName() const {
    if (codec_ctx_ && codec_ctx_->codec) {
        return codec_ctx_->codec->name;
    }
    return "unknown";
}

void AudioDecoder::Flush() {
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
    }
}

void AudioDecoder::Close() {
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
}

} // namespace player
} // namespace videoeye
