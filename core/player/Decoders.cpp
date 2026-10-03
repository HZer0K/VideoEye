#include "core/player/Decoders.h"
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
    sw_frame_ = av_frame_alloc();
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
        AV_HWDEVICE_TYPE_VAAPI,        // Linux AMD/Intel
        AV_HWDEVICE_TYPE_CUDA,         // NVIDIA
        AV_HWDEVICE_TYPE_VDPAU,        // NVIDIA (legacy)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX, // macOS/iOS
        AV_HWDEVICE_TYPE_D3D11VA,      // Windows
        AV_HWDEVICE_TYPE_DXVA2,        // Windows (legacy)
        AV_HWDEVICE_TYPE_QSV,          // Intel Quick Sync
    };

    // 探测时临时降低 FFmpeg 日志级别，避免硬件驱动不兼容的错误刷屏
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

bool VideoDecoder::DownloadHwFrame(AVFrame* sw_frame, AVFrame* hw_frame) {
    if (!sw_frame || !hw_frame) {
        return false;
    }

    AVFrame* tmp = av_frame_alloc();
    if (!tmp) return false;

    int ret = av_hwframe_transfer_data(tmp, hw_frame, 0);
    if (ret < 0) {
        av_frame_free(&tmp);
        return false;
    }
    av_frame_copy_props(tmp, hw_frame);
    av_frame_move_ref(sw_frame, tmp);
    av_frame_free(&tmp);
    return true;
}

AVFrame* VideoDecoder::PrepareSwFrame(AVFrame* decoded) {
    if (!decoded || !sw_frame_) {
        return nullptr;
    }

    const AVPixFmtDescriptor* desc =
        (decoded->format == AV_PIX_FMT_NONE) ? nullptr
                                             : av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded->format));

    if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        // 硬件帧: data[0] 是设备指针, 必须先下载到系统内存才能交给下游
        if (!DownloadHwFrame(sw_frame_, decoded)) {
            std::cerr << "Failed to download hardware frame" << std::endl;
            return nullptr;
        }
        return sw_frame_;
    }

    // 软件解码: 解码帧本身就在系统内存, 直接接管(复用同一个 AVFrame, 不逐帧分配)
    av_frame_unref(sw_frame_);
    if (!desc) {
        // 未知格式: 保留 sw_frame_ 里上一次的内容, 下游拿到的仍是最近一个有效帧
        return nullptr;
    }
    av_frame_move_ref(sw_frame_, decoded);
    return sw_frame_;
}

void VideoDecoder::StashBufferedFrames() {
    if (!codec_ctx_ || !frame_) {
        return;
    }

    // EAGAIN 的正确语义是"先把内部已解码还没取走的帧收干", 不是放弃这个包
    while (true) {
        int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret < 0) {
            return;  // EAGAIN / EOF: 已经收干
        }
        // 收下的帧要存住: 它存在 frame_ 里会被下一次 receive 覆盖,
        // 之后由 ReceiveFrame 按先入先出交出去。
        AVFrame* stashed = av_frame_clone(frame_);
        if (!stashed) {
            std::cerr << "Failed to clone frame" << std::endl;
            return;
        }
        pending_frames_.push_back(stashed);
    }
}

bool VideoDecoder::SendPacket(AVPacket* packet) {
    if (!codec_ctx_ || !frame_) {
        return false;
    }

    int ret = avcodec_send_packet(codec_ctx_, packet);
    if (ret == AVERROR(EAGAIN)) {
        // 输出队列满(上一批帧还没被取走), 送包被拒。
        // 这里不能直接 return false: 那样整包被丢弃、已解码的帧也永远取不出来,
        // 表现为卡帧 / 音画不同步 / 长时间播放状态错乱。
        // 先把缓冲里的帧收进 pending_frames_ 腾出位置, 这个包 FFmpeg 已经收下,
        // 帧由 ReceiveFrame 补齐, 于是什么都不丢。
        StashBufferedFrames();
    }
    if (ret < 0) {
        std::cerr << "Error sending packet to decoder" << std::endl;
        return false;
    }

    return true;
}

bool VideoDecoder::ReceiveFrame(model::FrameData& output_frame) {
    if (!codec_ctx_ || !frame_) {
        return false;
    }

    // 上次 SendPacket 撞 EAGAIN 时收下的帧还排着队: 必须先入先出把它们交出去,
    // 否则解码顺序就乱了(画面跳变 / 音画不同步)。
    AVFrame* decoded = frame_;
    if (!pending_frames_.empty()) {
        decoded = pending_frames_.front();
    } else {
        int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return false;
            }
            std::cerr << "Error receiving frame from decoder" << std::endl;
            return false;
        }
    }

    last_pict_type_ = decoded->pict_type;

    // 统一落到 sw_frame_: 硬件帧下载到系统内存, 软件帧直接接管。
    // 下游(渲染 / 宏块 / 画质分析)只读它, 拿到的永远是可 CPU 读的帧。
    AVFrame* src_frame = PrepareSwFrame(decoded);
    if (!src_frame) {
        return false;
    }

    // 队列里那一帧已被接管(软件路径 move 走、硬件路径已下载完), 出队并释放空壳;
    // decoded == frame_ 时不走这里 —— 它的生命周期归解码器。
    if (decoded != frame_) {
        pending_frames_.pop_front();
        av_frame_free(&decoded);
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

AVCodecID VideoDecoder::GetCodecId() const {
    if (codec_ctx_) {
        return codec_ctx_->codec_id;
    }
    return AV_CODEC_ID_NONE;
}

void VideoDecoder::Flush() {
    // 队列里暂存的是定位前就已经收下的帧: 它们和解码器缓冲里的旧帧一样过期,
    // 必须一并丢掉, 否则会在新位置之后被当新画面放出来(画面先跳回旧位置再回来)。
    while (!pending_frames_.empty()) {
        av_frame_free(&pending_frames_.front());
        pending_frames_.pop_front();
    }
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
    }
}

void VideoDecoder::Close() {
    // 排队的帧不能漏: 它们已经从解码器里收出来了, 不释放就是泄漏
    for (AVFrame* f : pending_frames_) {
        av_frame_free(&f);
    }
    pending_frames_.clear();
    if (sw_frame_) {
        av_frame_free(&sw_frame_);
    }
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
