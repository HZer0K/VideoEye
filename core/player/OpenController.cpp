#include "core/player/OpenController.h"

#include <cstdint>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include "core/analysis/orchestration/MediaInfoAnalyzer.h"  // FormatFromContext（复用本次探测）
#include "core/media/probe/FileProbe.h"
#include "core/player/AudioOutput.h"
#include "core/player/Decoders.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace player {

namespace {

// FFmpeg 错误码 -> 可读描述。不用 av_err2str 宏 (MSVC 不支持其 compound literal 写法)。
QString AvErrorString(int ret) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(ret, errbuf, sizeof(errbuf)) != 0) {
        return QStringLiteral("错误码 %1").arg(ret);
    }
    return QString::fromUtf8(errbuf);
}

}  // namespace

// --- OpenResult 的所有权 ---
//
// 析构与移动必须成对手写: format_ctx 是裸指针, 默认移动只会按值拷贝它 —— 移动之后源
// 对象仍指向同一个 AVFormatContext。源一析构就 avformat_close_input, 而被搬到的新对象
// (UI 线程正在提交、解码线程随后要 av_read_frame 的那一手)手里立刻是悬垂指针: 先是段
// 错误, 源对象再析构一次又变成 double free。
// 规则: 移动后源必须显式置空; 析构只关自己手里那一份。

void OpenResult::Release() noexcept {
    if (format_ctx) {
        avformat_close_input(&format_ctx);
    }
    format_ctx = nullptr;
    video_decoder.reset();
    audio_decoder.reset();
    audio_output.reset();
}

void OpenResult::MoveFrom(OpenResult& other) noexcept {
    ok = other.ok;
    error = std::move(other.error);
    // 关键一行: 源交出所有权, 它的析构不能再碰这个上下文
    format_ctx = other.format_ctx;
    other.format_ctx = nullptr;
    video_decoder = std::move(other.video_decoder);
    audio_decoder = std::move(other.audio_decoder);
    audio_output = std::move(other.audio_output);
    video_stream_index = other.video_stream_index;
    audio_stream_index = other.audio_stream_index;
    has_video = other.has_video;
    stream_info = std::move(other.stream_info);
    media_info_text = std::move(other.media_info_text);
    duration_ms = other.duration_ms;
    cover_art = std::move(other.cover_art);

    other.ok = false;
    other.error.clear();
    other.video_stream_index = -1;
    other.audio_stream_index = -1;
    other.has_video = false;
    other.duration_ms = 0;
}

OpenResult::~OpenResult() {
    Release();
}

OpenController::OpenController(PlaybackSession& playback, QObject* parent)
    : QObject(parent), playback_(playback) {}

// --- 事务的提交侧 (UI 线程) ---

bool OpenController::Commit(OpenResult&& result, model::StreamInfo& out_info, QString& out_error) {
    if (!result.ok) {
        // 失败是整体失败: 发信号 + 释放会话。上下文(若有)由 result 析构关闭。
        out_error = result.error;
        emit OpenFailed(result.error);
        playback_.Release();
        return false;
    }

    AVFormatContext* fmt = result.format_ctx;
    result.format_ctx = nullptr;  // 所有权转移给播放会话
    playback_.AdoptFormatContext(fmt);
    playback_.SetStreamIndices(result.video_stream_index, result.audio_stream_index);
    playback_.SetVideoDecoder(std::move(result.video_decoder));
    playback_.SetAudioDecoder(std::move(result.audio_decoder));
    playback_.SetAudioOutput(std::move(result.audio_output));
    playback_.SetDuration(result.duration_ms);
    playback_.SetPosition(0);
    playback_.SetIdle();

    out_info = result.stream_info;
    // 信号顺序与拆分前一致: 封面图先于媒体模式。
    if (!result.cover_art.isNull()) emit FrameReady(result.cover_art);
    // has_video 是"文件里有没有视频轨"—— 封面图也算有（界面据此切布局），
    // 与降级后的 video_index（"有没有可播放的视频流"）是两件事。
    emit MediaModeChanged(result.has_video);
    // 媒体信息文本: Prepare 阶段已从同一个上下文格式化好, 这里回 UI 线程。
    // 空文本不发（避免把界面上的"正在解析…"占位覆盖成空）。
    if (!result.media_info_text.empty()) {
        emit MediaInfoTextReady(QString::fromStdString(result.media_info_text));
    }
    out_error.clear();
    return true;
}

// --- 事务的准备侧 (可在后台线程执行) ---

OpenResult OpenController::Prepare(const OpenPrepareParams& params) {
    OpenResult result;
    const std::string url_str = params.url.toStdString();

    if (!CheckRuntimeVersion(result.error)) return result;

    if (!params.attempt) {
        result.error = QStringLiteral("内部错误: 缺少打开尝试状态");
        return result;
    }
    // 取消标志在本尝试开头复位（换文件时不串到上一次打开的取消），
    // 之后挂到中断回调上：打开/探测期间它被置位就能让阻塞的 IO 及时返回 AVERROR_EXIT。
    params.attempt->cancel.store(false, std::memory_order_release);
    params.attempt->interrupt.cancel = &params.attempt->cancel;
    params.attempt->interrupt.deadline_us = 0;

    if (!OpenAndProbe(params, result)) return result;

    AVFormatContext* fmt = result.format_ctx;
    if (!fmt) {
        result.error = QStringLiteral("Format context is null");
        return result;
    }

    // 媒体信息: 刚探测好的上下文直接格式化成文本（拿文件大小也只是 stat 一次）——
    // 这条数据以前由 UI 侧再跑一次 avformat_open_input + find_stream_info 才拿到，
    // 同一个文件被打开第四遍。文本随提交经 MediaInfoTextReady 回到 UI。
    result.media_info_text = MediaInfoAnalyzer::FormatFromContext(fmt, url_str);

    int video_index = -1;
    int audio_index = -1;
    const AVCodec* best_video_codec = nullptr;
    if (!FindStreams(fmt, video_index, audio_index, best_video_codec, result.error)) return result;

    result.has_video = (video_index >= 0);

    // 封面图: 解出后作为一帧画面(提交时随 FrameReady 发出)，并把视频流降级为 -1
    // （封面不是可播放的视频轨）。**只要**带 ATTACHED_PIC 就降级 —— 解不解得出来都不该
    // 把它当可播放的视频流去初始化解码器。
    if (IsCoverArtStream(fmt, video_index)) {
        result.cover_art = DecodeCoverArt(fmt->streams[video_index]);
        video_index = -1;
    }
    result.video_stream_index = video_index;
    result.audio_stream_index = audio_index;

    if (video_index >= 0 &&
        !PrepareVideoDecoder(params, fmt, video_index, best_video_codec, result.video_decoder,
                             result.error)) {
        return result;
    }
    if (audio_index >= 0 &&
        !PrepareAudioDecoder(params, fmt, audio_index, result.audio_decoder, result.audio_output,
                             result.error)) {
        return result;
    }

    // 提取流信息
    StreamInfoExtractor extractor;
    const auto extract_result = extractor.Extract(fmt, video_index, audio_index, params.url);
    result.stream_info = extract_result.info;
    result.duration_ms = extract_result.duration_ms;

    LOG_INFO("Open success (Prepare): " + url_str);
    result.ok = true;
    return result;
}

bool OpenController::CheckRuntimeVersion(QString& out_error) {
    const unsigned header_avcodec_major = LIBAVCODEC_VERSION_MAJOR;
    const unsigned runtime_avcodec_major = static_cast<unsigned>(avcodec_version() >> 16);
    if (header_avcodec_major == runtime_avcodec_major) return true;
    out_error = QString("FFmpeg libavcodec 版本不匹配：编译期头文件=%1，运行期库=%2。")
                    .arg(header_avcodec_major)
                    .arg(runtime_avcodec_major);
    return false;
}

bool OpenController::OpenAndProbe(const OpenPrepareParams& params, OpenResult& result) {
    const QString& url = params.url;
    const std::string url_str = url.toStdString();

    // 上下文必须自己分配: 中断回调要在 avformat_open_input **之前**装好。
    // 不可达的 URL、损坏文件、异常设备都会让打开/探测长时间阻塞在 IO 上 ——
    // 现在这一步在后台线程上跑（OpenAsync），装上回调后靠绝对超时 + 取消兜住。
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        result.error = QStringLiteral("无法分配格式上下文");
        return false;
    }
    ffmpeg_io::AttachInterrupt(fmt, params.attempt->interrupt, ffmpeg_io::kOpenTimeoutUs);

    AVDictionary* open_options = params.input_options;
    int ret = avformat_open_input(&fmt, url_str.c_str(), params.input_format,
                                  open_options ? &open_options : nullptr);
    // 探测到的上下文立刻交给 result 拥有: 之后的每一步失败都由 result 负责释放。
    result.format_ctx = fmt;
    if (open_options) av_dict_free(&open_options);

    if (ret < 0) {
        if (ret == AVERROR_EXIT) {
            result.error = QString("打开输入超时 (超过 %1 秒): %2")
                               .arg(ffmpeg_io::kOpenTimeoutUs / 1000000)
                               .arg(url);
        } else {
            result.error = QString("打开输入失败: %1 | FFmpeg: %2").arg(url, AvErrorString(ret));
            // 定向诊断: fMP4 分片缺 init 段等特征, 给出可操作的修复建议
            const std::string extra = videoeye::DiagnoseUnopenableFile(url_str);
            if (!extra.empty()) result.error += QString::fromStdString("；" + extra);
        }
        return false;
    }
    LOG_INFO("Open: avformat_open_input OK");

    // 探测阶段允许更长时间，但同样受绝对超时约束
    params.attempt->interrupt.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
    ret = avformat_find_stream_info(fmt, nullptr);
    // 探测一结束就关掉截止时间: 之后进入解复用/播放阶段，不能让打开期的超时
    // 误杀正常的长素材读取。（回调本身留着，但已是一个只响应取消的空钩子。）
    params.attempt->interrupt.deadline_us = 0;
    if (ret < 0) {
        result.error = (ret == AVERROR_EXIT)
                           ? QString("解析流信息超时 (超过 %1 秒): %2")
                                 .arg(ffmpeg_io::kProbeTimeoutUs / 1000000)
                                 .arg(url)
                           : QString("无法解析流信息 (文件可能损坏、截断或格式不受支持) | FFmpeg: %1")
                                 .arg(AvErrorString(ret));
        return false;
    }
    LOG_INFO("Open: avformat_find_stream_info OK, streams=" + std::to_string(fmt->nb_streams));
    return true;
}

bool OpenController::FindStreams(AVFormatContext* fmt, int& video_index, int& audio_index,
                                 const AVCodec*& best_video_codec, QString& out_error) {
    const AVCodec* best_audio_codec = nullptr;
    video_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &best_video_codec, 0);
    audio_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &best_audio_codec, 0);

    if (video_index >= 0 || audio_index >= 0) return true;
    out_error = QStringLiteral("文件中未找到可播放的视频/音频流 (可能已损坏或为不支持的编码)");
    return false;
}

bool IsCoverArtStream(const AVFormatContext* format, int video_index) {
    // 顺序不能动: 先判索引合法性, 再取 streams[video_index]。音频-only 时
    // video_index == -1, 旧写法 `AVStream* vs = fmt->streams[video_index];` 会先
    // 越界读 streams[-1] —— 未定义行为, 可能直接崩溃。
    if (!format || video_index < 0 ||
        video_index >= static_cast<int>(format->nb_streams)) {
        return false;
    }
    const AVStream* vs = format->streams[video_index];
    return vs != nullptr && (vs->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
}

QImage OpenController::DecodeCoverArt(AVStream* vs) {
    if (!vs->codecpar || !vs->attached_pic.data || vs->attached_pic.size <= 0) return QImage();
    const AVCodec* cover_codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!cover_codec) return QImage();

    AVCodecContext* cover_ctx = avcodec_alloc_context3(cover_codec);
    if (!cover_ctx) return QImage();
    if (avcodec_parameters_to_context(cover_ctx, vs->codecpar) < 0) {
        avcodec_free_context(&cover_ctx);
        return QImage();
    }
    if (vs->time_base.den != 0) {
        cover_ctx->pkt_timebase = vs->time_base;
        cover_ctx->time_base = vs->time_base;
    }
    if (avcodec_open2(cover_ctx, cover_codec, nullptr) < 0) {
        avcodec_free_context(&cover_ctx);
        return QImage();
    }

    VideoDecoder cover_decoder;
    if (!cover_decoder.InitializeFromContext(cover_ctx)) {
        avcodec_free_context(&cover_ctx);
        return QImage();
    }
    model::FrameData cover_frame;
    if (!cover_decoder.DecodePacket(&vs->attached_pic, cover_frame)) return QImage();
    if (cover_frame.width <= 0 || cover_frame.height <= 0 || !cover_frame.data[0]) return QImage();

    SwsContext* cover_sws = sws_getCachedContext(
        nullptr, cover_frame.width, cover_frame.height,
        static_cast<AVPixelFormat>(cover_frame.format), cover_frame.width, cover_frame.height,
        AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!cover_sws) return QImage();
    QImage cover_img(cover_frame.width, cover_frame.height, QImage::Format_ARGB32);
    if (!cover_img.isNull()) {
        uint8_t* dst_slices[4] = {cover_img.bits(), nullptr, nullptr, nullptr};
        int dst_linesize[4] = {static_cast<int>(cover_img.bytesPerLine()), 0, 0, 0};
        sws_scale(cover_sws, cover_frame.data, cover_frame.linesize, 0, cover_frame.height,
                  dst_slices, dst_linesize);
    }
    sws_freeContext(cover_sws);
    return cover_img;
}

bool OpenController::PrepareVideoDecoder(const OpenPrepareParams& params, AVFormatContext* fmt,
                                         int video_index, const AVCodec* best_video_codec,
                                         std::unique_ptr<VideoDecoder>& out_decoder,
                                         QString& out_error) {
    out_decoder = std::make_unique<VideoDecoder>();
    AVStream* video_stream = fmt->streams[video_index];
    if (!video_stream || !video_stream->codecpar) {
        out_error = QStringLiteral("视频流编解码参数不可用");
        return false;
    }

    // 尝试硬件解码
    // 宏块分析依赖软件解码导出的运动矢量 side data, 硬件解码器不产出该数据,
    // 因此启用宏块分析时直接走软件解码路径。
    if (params.prefer_hw_decoding && !params.macroblock_analysis) {
        // VAAPI / D3D11VA / CUDA / QSV ...
        for (auto hw_type : VideoDecoder::GetAvailableHwDeviceTypes()) {
            if (out_decoder->InitializeWithHw(video_stream->codecpar, hw_type)) {
                LOG_INFO("HW decoding initialized: " +
                         std::string(av_hwdevice_get_type_name(hw_type)));
                LOG_INFO("Video decoder initialized: " +
                         std::to_string(out_decoder->GetWidth()) + "x" +
                         std::to_string(out_decoder->GetHeight()) + " (HW)");
                return true;
            }
        }
        LOG_WARN("HW decoding not available, falling back to software");
    }

    // 软件解码回退
    const AVCodec* video_codec = best_video_codec;
    if (!video_codec) video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!video_codec) {
        out_error = QString("找不到视频解码器 (codec_id=%1)")
                        .arg(avcodec_get_name(video_stream->codecpar->codec_id));
        return false;
    }
    AVCodecContext* video_codec_ctx = avcodec_alloc_context3(video_codec);
    if (!video_codec_ctx) {
        out_error = QStringLiteral("无法创建视频解码器上下文");
        return false;
    }
    int ret = avcodec_parameters_to_context(video_codec_ctx, video_stream->codecpar);
    if (ret < 0) {
        avcodec_free_context(&video_codec_ctx);
        out_error = QString("复制视频编解码参数失败: %1").arg(AvErrorString(ret));
        return false;
    }
    if (video_stream->time_base.den != 0) {
        video_codec_ctx->pkt_timebase = video_stream->time_base;
        video_codec_ctx->time_base = video_stream->time_base;
    }
    // 导出运动矢量 side data (供宏块分析使用)。
    // 注意: 软件解码才会产出 AV_FRAME_DATA_MOTION_VECTORS; 硬件解码不导出该 side data。
    video_codec_ctx->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
    ret = avcodec_open2(video_codec_ctx, video_codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&video_codec_ctx);
        out_error = QString("打开视频解码器失败 (%1): %2")
                        .arg(avcodec_get_name(video_codec->id), AvErrorString(ret));
        return false;
    }
    if (!out_decoder->InitializeFromContext(video_codec_ctx)) {
        avcodec_free_context(&video_codec_ctx);
        out_error = QStringLiteral("初始化视频解码器失败");
        return false;
    }
    LOG_INFO("Video decoder initialized: " +
             std::to_string(out_decoder->GetWidth()) + "x" +
             std::to_string(out_decoder->GetHeight()) + " (SW)");
    return true;
}

bool OpenController::PrepareAudioDecoder(const OpenPrepareParams& params, AVFormatContext* fmt,
                                         int audio_index, std::unique_ptr<AudioDecoder>& out_decoder,
                                         std::unique_ptr<AudioOutput>& out_output,
                                         QString& out_error) {
    out_decoder = std::make_unique<AudioDecoder>();
    AVStream* audio_stream = fmt->streams[audio_index];
    if (!audio_stream->codecpar) {
        out_error = QStringLiteral("音频流编解码参数不可用");
        return false;
    }
    if (!out_decoder->Initialize(audio_stream->codecpar)) {
        out_error = QStringLiteral("初始化音频解码器失败");
        return false;
    }
    LOG_INFO("Audio decoder initialized");

    // 初始化音频输出设备（平台原生后端: WASAPI / ALSA / AudioQueue）:
    // 解码后的 PCM 由音频线程主动 pull。
    // 走异步打开: Windows(WASAPI) 首次打开音频设备实测 ~1s，
    // 同步调用会把"打开文件"整段卡住。设备就绪前音频帧丢弃, 视频不受影响。
    auto audio_output = std::make_unique<AudioOutput>();
    audio_output->SetVolume(params.volume_percent / 100.0);
    audio_output->OpenAsync(out_decoder->GetSampleRate(), out_decoder->GetChannels());
    out_output = std::move(audio_output);
    return true;
}

}  // namespace player
}  // namespace videoeye
