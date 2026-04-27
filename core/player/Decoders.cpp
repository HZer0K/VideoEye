#include "Decoders.h"
#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
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

bool VideoDecoder::DecodePacket(AVPacket* packet, model::FrameData& output_frame) {
    if (!codec_ctx_ || !frame_) {
        return false;
    }
    
    // 发送数据包到解码器
    int ret = avcodec_send_packet(codec_ctx_, packet);
    if (ret < 0) {
        std::cerr << "Error sending packet to decoder" << std::endl;
        return false;
    }
    
    // 接收解码后的帧
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // 需要更多数据
            return false;
        }
        std::cerr << "Error receiving frame from decoder" << std::endl;
        return false;
    }

    last_pict_type_ = frame_->pict_type;
    
    // 复制帧数据
    output_frame.Clear();

    output_frame.width = frame_->width;
    output_frame.height = frame_->height;
    output_frame.format = frame_->format;
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

    const AVPixFmtDescriptor* desc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame_->format));

    for (int i = 0; i < 8; ++i) {
        if (!frame_->data[i] || frame_->linesize[i] <= 0) {
            continue;
        }

        int plane_height = frame_->height;
        if (desc && (i == 1 || i == 2)) {
            plane_height = AV_CEIL_RSHIFT(frame_->height, desc->log2_chroma_h);
        }

        int size = frame_->linesize[i] * plane_height;
        if (size <= 0) {
            continue;
        }

        output_frame.linesize[i] = frame_->linesize[i];
        output_frame.owned[i].resize(size);
        std::memcpy(output_frame.owned[i].data(), frame_->data[i], size);
        output_frame.data[i] = output_frame.owned[i].data();
    }
    
    return true;
}

std::string VideoDecoder::GetCodecName() const {
    if (codec_ctx_ && codec_ctx_->codec) {
        return codec_ctx_->codec->name;
    }
    return "unknown";
}

void VideoDecoder::Close() {
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
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

bool AudioDecoder::DecodePacket(AVPacket* packet, uint8_t* output_buffer, int buffer_size, int& output_size) {
    if (!codec_ctx_ || !frame_ || !swr_ctx_) {
        return false;
    }
    
    // 发送数据包
    int ret = avcodec_send_packet(codec_ctx_, packet);
    if (ret < 0) {
        std::cerr << "Error sending audio packet" << std::endl;
        return false;
    }
    
    // 接收帧
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            return false;
        }
        std::cerr << "Error receiving audio frame" << std::endl;
        return false;
    }
    
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
    
    return true;
}

std::string AudioDecoder::GetCodecName() const {
    if (codec_ctx_ && codec_ctx_->codec) {
        return codec_ctx_->codec->name;
    }
    return "unknown";
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
