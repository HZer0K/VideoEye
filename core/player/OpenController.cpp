#include "core/player/OpenController.h"

#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

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

OpenController::OpenController(PlaybackSession& playback, AnalysisSession& analysis,
                               ffmpeg_io::AvInterruptState& interrupt, std::atomic<bool>& cancel,
                               QObject* parent)
    : QObject(parent),
      playback_(playback),
      analysis_(analysis),
      interrupt_(interrupt),
      cancel_(cancel) {}

void OpenController::RequestCancel() {
    // 置位后卡在 avformat_open_input / av_read_frame 上的阻塞 IO 会立刻退出 ——
    // deadline 只会"等到超时"，只有它能让阻塞的 read 马上返回 AVERROR_EXIT。
    cancel_.store(true);
    // 打开/探测阶段的截止时间一并不留（deadline 只服务于打开期，播放期的正常长读不该被误杀）。
    interrupt_.deadline_us = 0;
}

bool OpenController::Open(const QString& url, const AVInputFormat* input_format,
                          AVDictionary* input_options, int volume_percent,
                          model::StreamInfo& out_info, QString& out_error) {
    LOG_INFO("Open: " + url.toStdString());

    // 单一失败出口: 任何一步失败都在这里发信号、释放上下文并返回。
    // 以前八个分支各写一遍"emit + Release + return false"，漏一处就是句柄泄漏。
    const auto fail = [this, &out_error](const QString& message) {
        out_error = message;
        emit OpenFailed(message);
        playback_.Release();
        return false;
    };

    if (!CheckRuntimeVersion(out_error)) return fail(out_error);

    // 取消标志在打开流程开头复位（换文件时不串到上一次打开的取消），
    // 之后挂到中断回调上：打开/探测期间它被置位就能让阻塞的 IO 及时返回 AVERROR_EXIT。
    cancel_.store(false);
    interrupt_.cancel = &cancel_;
    interrupt_.deadline_us = 0;

    if (!OpenAndProbe(url, input_format, input_options, out_error)) return fail(out_error);

    AVFormatContext* fmt = playback_.format_ctx();
    if (!fmt) return fail(QStringLiteral("Format context is null"));

    int video_index = -1;
    int audio_index = -1;
    const AVCodec* best_video_codec = nullptr;
    if (!FindStreams(video_index, audio_index, best_video_codec, out_error)) return fail(out_error);

    bool has_video = (video_index >= 0);
    playback_.SetStreamIndices(video_index, audio_index);
    HandleCoverArt(video_index, audio_index);
    // has_video 是"文件里有没有视频轨"—— 封面图也算有（界面据此切布局），
    // 与降级后的 video_index（"有没有可播放的视频流"）是两件事。
    emit MediaModeChanged(has_video);

    if (video_index >= 0 && !InitVideoDecoder(video_index, best_video_codec, out_error)) {
        return fail(out_error);
    }
    if (audio_index >= 0 && !InitAudioDecoder(audio_index, volume_percent, out_error)) {
        return fail(out_error);
    }

    // 提取流信息 (委托给 StreamInfoExtractor)
    const auto extract_result = stream_info_extractor_.Extract(fmt, video_index, audio_index, url);
    out_info = extract_result.info;
    playback_.SetDuration(extract_result.duration_ms);
    playback_.SetPosition(0);
    playback_.SetIdle();

    LOG_INFO("Open success: " + url.toStdString());
    return true;
}

bool OpenController::CheckRuntimeVersion(QString& out_error) const {
    const unsigned header_avcodec_major = LIBAVCODEC_VERSION_MAJOR;
    const unsigned runtime_avcodec_major = static_cast<unsigned>(avcodec_version() >> 16);
    if (header_avcodec_major == runtime_avcodec_major) return true;
    out_error = QString("FFmpeg libavcodec 版本不匹配：编译期头文件=%1，运行期库=%2。")
                    .arg(header_avcodec_major)
                    .arg(runtime_avcodec_major);
    return false;
}

bool OpenController::OpenAndProbe(const QString& url, const AVInputFormat* input_format,
                                  AVDictionary* input_options, QString& out_error) {
    // 上下文必须自己分配: 中断回调要在 avformat_open_input **之前**装好。
    // 不可达的 URL、损坏文件、异常设备都会让打开/探测长时间阻塞在 IO 上，
    // 而这条路径跑在 UI 线程 —— 阻塞多久，界面就冻多久。装上回调后至少能靠绝对超时兜住。
    AVFormatContext* fmt = avformat_alloc_context();
    ffmpeg_io::AttachInterrupt(fmt, interrupt_, ffmpeg_io::kOpenTimeoutUs);

    AVDictionary* open_options = input_options;
    const std::string url_str = url.toStdString();
    int ret = avformat_open_input(&fmt, url_str.c_str(), input_format,
                                  open_options ? &open_options : nullptr);
    // 探测到的上下文立刻交给播放会话: 之后的每一步失败都由会话负责释放。
    playback_.AdoptFormatContext(fmt);
    fmt = playback_.format_ctx();
    if (open_options) av_dict_free(&open_options);

    if (ret < 0) {
        if (ret == AVERROR_EXIT) {
            out_error = QString("打开输入超时 (超过 %1 秒): %2")
                            .arg(ffmpeg_io::kOpenTimeoutUs / 1000000)
                            .arg(url);
        } else {
            out_error = QString("打开输入失败: %1 | FFmpeg: %2").arg(url, AvErrorString(ret));
            // 定向诊断: fMP4 分片缺 init 段等特征, 给出可操作的修复建议
            const std::string extra = utils::DiagnoseUnopenableFile(url_str);
            if (!extra.empty()) out_error += QString::fromStdString("；" + extra);
        }
        return false;
    }
    LOG_INFO("Open: avformat_open_input OK");

    // 探测阶段允许更长时间，但同样受绝对超时约束
    interrupt_.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
    ret = avformat_find_stream_info(fmt, nullptr);
    // 探测一结束就关掉截止时间: 之后进入解复用/播放阶段，不能让打开期的超时
    // 误杀正常的长素材读取。（回调本身留着，但已是一个恒返回 0 的空钩子。）
    interrupt_.deadline_us = 0;
    if (ret < 0) {
        out_error = (ret == AVERROR_EXIT)
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

bool OpenController::FindStreams(int& video_index, int& audio_index,
                                 const AVCodec*& best_video_codec, QString& out_error) {
    AVFormatContext* fmt = playback_.format_ctx();
    const AVCodec* best_audio_codec = nullptr;
    video_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &best_video_codec, 0);
    audio_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &best_audio_codec, 0);

    if (video_index >= 0 || audio_index >= 0) return true;
    out_error = QStringLiteral("文件中未找到可播放的视频/音频流 (可能已损坏或为不支持的编码)");
    return false;
}

bool OpenController::HandleCoverArt(int& video_index, int audio_index) {
    AVFormatContext* fmt = playback_.format_ctx();
    AVStream* vs = fmt->streams[video_index];
    if (video_index < 0 || !vs || !(vs->disposition & AV_DISPOSITION_ATTACHED_PIC)) return false;

    EmitCoverArt(vs);
    // 封面图不是可播放的视频轨: 无论解没解出来都要把视频流降级掉，
    // 否则后面会拿一个 attached_pic 去初始化视频解码器。音频（如果有）照常。
    video_index = -1;
    playback_.SetStreamIndices(video_index, audio_index);
    return true;
}

void OpenController::EmitCoverArt(AVStream* vs) {
    if (!vs->codecpar || !vs->attached_pic.data || vs->attached_pic.size <= 0) return;
    const AVCodec* cover_codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!cover_codec) return;

    AVCodecContext* cover_ctx = avcodec_alloc_context3(cover_codec);
    if (!cover_ctx) return;
    if (avcodec_parameters_to_context(cover_ctx, vs->codecpar) < 0) {
        avcodec_free_context(&cover_ctx);
        return;
    }
    if (vs->time_base.den != 0) {
        cover_ctx->pkt_timebase = vs->time_base;
        cover_ctx->time_base = vs->time_base;
    }
    if (avcodec_open2(cover_ctx, cover_codec, nullptr) < 0) {
        avcodec_free_context(&cover_ctx);
        return;
    }

    VideoDecoder cover_decoder;
    if (!cover_decoder.InitializeFromContext(cover_ctx)) {
        avcodec_free_context(&cover_ctx);
        return;
    }
    model::FrameData cover_frame;
    if (!cover_decoder.DecodePacket(&vs->attached_pic, cover_frame)) return;
    if (cover_frame.width <= 0 || cover_frame.height <= 0 || !cover_frame.data[0]) return;

    SwsContext* cover_sws = sws_getCachedContext(
        nullptr, cover_frame.width, cover_frame.height,
        static_cast<AVPixelFormat>(cover_frame.format), cover_frame.width, cover_frame.height,
        AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!cover_sws) return;
    QImage cover_img(cover_frame.width, cover_frame.height, QImage::Format_ARGB32);
    if (!cover_img.isNull()) {
        uint8_t* dst_slices[4] = {cover_img.bits(), nullptr, nullptr, nullptr};
        int dst_linesize[4] = {static_cast<int>(cover_img.bytesPerLine()), 0, 0, 0};
        sws_scale(cover_sws, cover_frame.data, cover_frame.linesize, 0, cover_frame.height,
                  dst_slices, dst_linesize);
        emit FrameReady(cover_img);
    }
    sws_freeContext(cover_sws);
}

bool OpenController::InitVideoDecoder(int video_index, const AVCodec* best_video_codec,
                                      QString& out_error) {
    AVFormatContext* fmt = playback_.format_ctx();
    playback_.SetVideoDecoder(std::make_unique<VideoDecoder>());
    AVStream* video_stream = fmt->streams[video_index];
    if (!video_stream || !video_stream->codecpar) {
        out_error = QStringLiteral("视频流编解码参数不可用");
        return false;
    }

    bool hw_initialized = false;
    // 尝试硬件解码
    // 宏块分析依赖软件解码导出的运动矢量 side data, 硬件解码器不产出该数据,
    // 因此启用宏块分析时直接走软件解码路径。
    if (analysis_.IsHardwareDecodingEnabled() && !analysis_.IsMacroblockAnalysisEnabled()) {
        // VAAPI / D3D11VA / CUDA / QSV ...
        // (Vulkan HW 解码随 Vulkan 渲染器一并移除: 本项目定位是分析工具,
        //  为一条零拷贝渲染路径背一套 Vulkan 运行时不划算)
        for (auto hw_type : VideoDecoder::GetAvailableHwDeviceTypes()) {
            if (playback_.video_decoder()->InitializeWithHw(video_stream->codecpar, hw_type)) {
                hw_initialized = true;
                LOG_INFO("HW decoding initialized: " +
                         std::string(av_hwdevice_get_type_name(hw_type)));
                break;
            }
        }
        if (!hw_initialized) {
            LOG_WARN("HW decoding not available, falling back to software");
        }
    }

    if (hw_initialized) {
        LOG_INFO("Video decoder initialized: " +
                 std::to_string(playback_.video_decoder()->GetWidth()) + "x" +
                 std::to_string(playback_.video_decoder()->GetHeight()) + " (HW)");
        return true;
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
    // 注意: 软件解码才会产出 AV_FRAME_DATA_MOTION_VECTORS; 硬件解码
    // (Vulkan/D3D11/CUDA) 不导出该 side data, 宏块分析面板将始终为空。
    video_codec_ctx->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
    ret = avcodec_open2(video_codec_ctx, video_codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&video_codec_ctx);
        out_error = QString("打开视频解码器失败 (%1): %2")
                        .arg(avcodec_get_name(video_codec->id), AvErrorString(ret));
        return false;
    }
    if (!playback_.video_decoder()->InitializeFromContext(video_codec_ctx)) {
        avcodec_free_context(&video_codec_ctx);
        out_error = QStringLiteral("初始化视频解码器失败");
        return false;
    }
    LOG_INFO("Video decoder initialized: " +
             std::to_string(playback_.video_decoder()->GetWidth()) + "x" +
             std::to_string(playback_.video_decoder()->GetHeight()) + " (SW)");
    return true;
}

bool OpenController::InitAudioDecoder(int audio_index, int volume_percent, QString& out_error) {
    AVFormatContext* fmt = playback_.format_ctx();
    playback_.SetAudioDecoder(std::make_unique<AudioDecoder>());
    AVStream* audio_stream = fmt->streams[audio_index];
    if (!audio_stream->codecpar) {
        out_error = QStringLiteral("音频流编解码参数不可用");
        return false;
    }
    if (!playback_.audio_decoder()->Initialize(audio_stream->codecpar)) {
        out_error = QStringLiteral("初始化音频解码器失败");
        return false;
    }
    LOG_INFO("Audio decoder initialized");

    // 初始化音频输出设备（平台原生后端: WASAPI / ALSA / AudioQueue）:
    // 解码后的 PCM 由音频线程主动 pull。
    // 走异步打开: Windows(WASAPI) 首次打开音频设备实测 ~1s,
    // 同步调用会把"打开文件"整段卡住。设备就绪前音频帧丢弃, 视频不受影响。
    auto audio_output = std::make_unique<AudioOutput>();
    audio_output->SetVolume(volume_percent / 100.0);
    audio_output->OpenAsync(playback_.audio_decoder()->GetSampleRate(),
                            playback_.audio_decoder()->GetChannels());
    playback_.SetAudioOutput(std::move(audio_output));
    return true;
}

}  // namespace player
}  // namespace videoeye
