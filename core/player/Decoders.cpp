#include "Decoders.h"
#include <iostream>
#include <cstring>

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
    
    // 复制帧数据
    output_frame.width = frame_->width;
    output_frame.height = frame_->height;
    output_frame.format = frame_->format;
    output_frame.pts = frame_->pts;
    output_frame.timestamp = frame_->pts * av_q2d(codec_ctx_->time_base);
    
    for (int i = 0; i < 8; ++i) {
        output_frame.linesize[i] = frame_->linesize[i];
        if (frame_->data[i] && frame_->linesize[i] > 0) {
            int size = frame_->linesize[i] * frame_->height;
            if (!output_frame.data[i]) {
                output_frame.data[i] = new uint8_t[size];
            }
            std::memcpy(output_frame.data[i], frame_->data[i], size);
        }
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
    
    // 初始化重采样上下文
    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) {
        std::cerr << "Failed to allocate resample context" << std::endl;
        return false;
    }
    
    // 设置重采样参数 (转换为 stereo, s16)
    av_channel_layout_default(&codec_ctx_->ch_layout, channels_);
    swr_alloc_set_opts2(&swr_ctx_,
                        &codec_ctx_->ch_layout, AV_SAMPLE_FMT_S16, codec_ctx_->sample_rate,
                        &codec_ctx_->ch_layout, codec_ctx_->sample_fmt, codec_ctx_->sample_rate,
                        0, nullptr);
    
    ret = swr_init(swr_ctx_);
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
