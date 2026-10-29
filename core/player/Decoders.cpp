#include "core/player/Decoders.h"
#include <cstring>
#include <iostream>

extern "C" {
#include <libavcodec/codec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
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
    if (!codec_params)
        return false;

    // 查找支持硬件加速的解码器
    const AVCodec* codec = nullptr;
    void* iter = nullptr;
    while ((codec = av_codec_iterate(&iter)) != nullptr) {
        if (!av_codec_is_decoder(codec))
            continue; // 只查解码器
        if (codec->id != codec_params->codec_id)
            continue;
        // 检查该解码器是否支持指定的 HW 设备类型
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (!config)
                break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == hw_type) {
                goto found_codec;
            }
        }
        codec = nullptr;
    }
found_codec:
    if (!codec) {
        std::cerr << "No HW decoder for codec " << avcodec_get_name(codec_params->codec_id) << " with device type "
                  << av_hwdevice_get_type_name(hw_type) << std::endl;
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
        if (!config)
            break;
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
        for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE;
             ++p){const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
                  if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)){return *p;}
}
return pix_fmts[0];
}; // namespace player

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
} // namespace videoeye

bool VideoDecoder::DownloadHwFrame(AVFrame* sw_frame, AVFrame* hw_frame) {
    if (!sw_frame || !hw_frame) {
        return false;
    }

    AVFrame* tmp = av_frame_alloc();
    if (!tmp)
        return false;

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

    const AVPixFmtDescriptor* desc = (decoded->format == AV_PIX_FMT_NONE)
                                         ? nullptr
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
            return; // EAGAIN / EOF: 已经收干
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
        //
        // ❗ 但"把缓冲收进 pending_frames_ 就完事"同样不对 —— 这时包**还没有**
        // 被解码器接受，ret 仍是 EAGAIN，旧代码照原样往下走 return false，上层
        // 于是跳过这个包（丢帧，且 pending 里的帧补齐的是更早的包）。
        // FFmpeg 的约定是：把输出读干净后**重送同一个包**，这次不会再 EAGAIN。
        StashBufferedFrames();
        ret = avcodec_send_packet(codec_ctx_, packet);
        if (ret == AVERROR(EAGAIN)) {
            // 理论上到不了这里（收干之后必须能收包）。真到了说明解码器状态异常，
            // 宁可报失败也不要静默丢包 —— 上层跳过一包会让时间轴整体错位。
            std::cerr << "Decoder still not accepting packets after draining output" << std::endl;
            return false;
        }
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

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src_frame->format));

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

    // 初始化重采样上下文（走 RebuildSwr：它同时记下输入布局快照，
    // 之后 ReceiveFrame 才能判断"流中途改布局"要不要重建）
    AVChannelLayout in_layout{};
    if (codec_ctx_->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&in_layout, &codec_ctx_->ch_layout) < 0) {
            av_channel_layout_default(&in_layout, channels_);
        }
    } else {
        av_channel_layout_default(&in_layout, channels_);
    }

    const bool swr_ok = RebuildSwr(in_layout, codec_ctx_->sample_fmt, sample_rate_ > 0 ? sample_rate_ : 44100);
    av_channel_layout_uninit(&in_layout);
    if (!swr_ok) {
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
    if (ret == AVERROR(EAGAIN)) {
        // 与视频侧同一条约定：输出队列满 → 先把已解码帧收进 pending_frames_，
        // 然后**重送同一个包**。旧实现直接 return false，上层于是跳过这一包音频
        // （听感上是几十毫秒的断续，时间轴也会整体前移）。
        StashBufferedFrames();
        ret = avcodec_send_packet(codec_ctx_, packet);
    }
    if (ret < 0) {
        std::cerr << "Error sending audio packet" << std::endl;
        return false;
    }

    return true;
}

void AudioDecoder::StashBufferedFrames() {
    if (!codec_ctx_ || !frame_) {
        return;
    }
    // 收干解码器内部已解码、调用方还没取走的帧（先入先出交给 ReceiveFrame）
    while (true) {
        const int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret < 0) {
            return; // EAGAIN / EOF: 已经收干
        }
        AVFrame* stashed = av_frame_clone(frame_);
        if (!stashed) {
            std::cerr << "Failed to clone audio frame" << std::endl;
            return;
        }
        pending_frames_.push_back(stashed);
    }
}

bool AudioDecoder::SwrMatches(const AVChannelLayout& in_layout, AVSampleFormat in_fmt, int in_rate) const {
    if (swr_ctx_ == nullptr || !swr_in_layout_valid_)
        return false;
    if (static_cast<int>(in_fmt) != swr_in_fmt_)
        return false;
    if (in_rate != swr_in_sample_rate_)
        return false;
    if (av_channel_layout_compare(&swr_in_layout_, &in_layout) != 0)
        return false;
    return swr_in_channels_ == in_layout.nb_channels;
}

bool AudioDecoder::RebuildSwr(const AVChannelLayout& in_layout, AVSampleFormat in_fmt, int in_rate) {
    if (swr_ctx_ != nullptr) {
        swr_free(&swr_ctx_);
    }
    swr_in_layout_valid_ = false;

    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) {
        std::cerr << "Failed to allocate resample context" << std::endl;
        return false;
    }

    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, channels_);
    const int out_rate = sample_rate_ > 0 ? sample_rate_ : (in_rate > 0 ? in_rate : 44100);
    const int ret = swr_alloc_set_opts2(&swr_ctx_, &out_layout, AV_SAMPLE_FMT_S16, out_rate, &in_layout, in_fmt,
                                        in_rate > 0 ? in_rate : out_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || swr_init(swr_ctx_) < 0) {
        std::cerr << "Failed to initialize resample context" << std::endl;
        swr_free(&swr_ctx_);
        return false;
    }

    if (av_channel_layout_copy(&swr_in_layout_, &in_layout) < 0) {
        av_channel_layout_uninit(&swr_in_layout_);
        swr_in_layout_valid_ = false;
        return false;
    }
    swr_in_layout_valid_ = true;
    swr_in_channels_ = in_layout.nb_channels;
    swr_in_sample_rate_ = in_rate;
    swr_in_fmt_ = static_cast<int>(in_fmt);
    return true;
}

bool AudioDecoder::ReceiveFrame(uint8_t* output_buffer, int buffer_size, int& output_size) {
    if (!codec_ctx_ || !frame_ || !swr_ctx_) {
        return false;
    }

    // 上次 SendPacket 撞 EAGAIN 时收下的帧还排着队：必须先入先出交出去，
    // 否则输出顺序就乱了（听感上是"抖"，分析侧的时间轴也对不上）。
    if (!pending_frames_.empty()) {
        AVFrame* queued = pending_frames_.front();
        pending_frames_.pop_front();
        const bool ok = ConvertFrameToPcm(queued, output_buffer, buffer_size, output_size);
        av_frame_free(&queued);
        return ok;
    }

    const int ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return false;
        }
        std::cerr << "Error receiving audio frame" << std::endl;
        return false;
    }
    // frame_ 的生命周期归解码器（下一次 receive 会自己 unref），这里不释放
    return ConvertFrameToPcm(frame_, output_buffer, buffer_size, output_size);
}

bool AudioDecoder::ConvertFrameToPcm(AVFrame* src, uint8_t* output_buffer, int buffer_size, int& output_size) {
    output_size = 0;
    if (!src || !swr_ctx_ || !output_buffer || buffer_size <= 0) {
        return false;
    }

    last_frame_pts_ = src->pts;
    if (last_frame_pts_ == AV_NOPTS_VALUE) {
        last_frame_pts_ = src->best_effort_timestamp;
    }
    last_frame_sample_count_ = src->nb_samples;
    last_frame_sample_rate_ = src->sample_rate > 0 ? src->sample_rate : sample_rate_;
    last_frame_channels_ = src->ch_layout.nb_channels > 0 ? src->ch_layout.nb_channels : channels_;

    if (channels_ <= 0) {
        return false;
    }

    // 流中途改布局：用帧自己的 ch_layout/format/sample_rate 重建 swr。
    // 不重建的话 swr 会按 Init 时的旧布局解释新帧。
    AVChannelLayout in_layout{};
    if (src->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&in_layout, &src->ch_layout) < 0) {
            av_channel_layout_default(&in_layout, channels_);
        }
    } else {
        av_channel_layout_default(&in_layout, channels_);
    }
    const AVSampleFormat in_fmt = static_cast<AVSampleFormat>(src->format);
    const int in_rate = src->sample_rate > 0 ? src->sample_rate : sample_rate_;
    if (!SwrMatches(in_layout, in_fmt, in_rate)) {
        const bool rebuilt = RebuildSwr(in_layout, in_fmt, in_rate);
        av_channel_layout_uninit(&in_layout);
        if (!rebuilt) {
            return false;
        }
    } else {
        av_channel_layout_uninit(&in_layout);
    }

    // 重采样
    uint8_t* output_ptrs[8] = {output_buffer, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    // ❗ 输入一侧必须走 extended_data：src->data 只有 8 个指针（AV_NUM_DATA_POINTERS），
    // 超过 8 声道的 planar 帧其余声道在 extended_data 里。传 data 会让 swr 按
    // nb_channels 去读 data[8..N-1] —— 那几格其实是 linesize 的内容，会被当指针解引用。
    // 离线分析路径（AnalysisPipeline::FeedAudioFrame）早就改成 extended_data 了，
    // 只有播放这条漏了。
    // swr_convert() 的 in 形参在 FFmpeg 7.0 起收紧成 const uint8_t* const*；这里只落到
    // const uint8_t**：新版头文件会自己补上顶层 const，老版（Ubuntu 24.04 的 FFmpeg 6.1）
    // 也才接得住 —— 写死 const uint8_t* const* 在 6.1 上是 invalid conversion。
    output_size = swr_convert(swr_ctx_, output_ptrs, buffer_size / (channels_ * 2),
                              const_cast<const uint8_t**>(src->extended_data), src->nb_samples);

    if (output_size < 0) {
        std::cerr << "Error during audio resampling" << std::endl;
        output_size = 0;
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
    // 排队的帧是定位前就收下的，同样过期，必须一起丢掉
    for (AVFrame* f : pending_frames_) {
        av_frame_free(&f);
    }
    pending_frames_.clear();
    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
    }
}

int AudioDecoder::DrainResampler(uint8_t* output_buffer, int buffer_size) {
    if (!swr_ctx_ || !output_buffer || buffer_size <= 0 || channels_ <= 0) {
        return 0;
    }
    // 输入给 nullptr / 0 = 把 swr 内部残留吐出来。
    // 与 ReceiveFrame 保持同一套输出格式：交错 S16、channels_ 声道。
    uint8_t* output_ptrs[8] = {output_buffer, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    const int samples = swr_convert(swr_ctx_, output_ptrs, buffer_size / (channels_ * 2), nullptr, 0);
    if (samples <= 0) {
        return 0;
    }
    return samples * channels_ * 2;
}

void AudioDecoder::Close() {
    for (AVFrame* f : pending_frames_) {
        av_frame_free(&f);
    }
    pending_frames_.clear();
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    if (swr_in_layout_valid_) {
        av_channel_layout_uninit(&swr_in_layout_);
        swr_in_layout_valid_ = false;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
}

} // namespace player
} // namespace videoeye
