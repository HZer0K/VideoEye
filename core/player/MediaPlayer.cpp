#include "core/player/MediaPlayer.h"
#include "infrastructure/logging/Logger.h"
#include "core/media/probe/FileProbe.h"
#include <QFileInfo>
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <limits>
#include <exception>
#include <utility>
#include <QDir>
#include <QThread>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavcodec/version.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
}

namespace videoeye {
namespace player {

using SteadyClock = std::chrono::steady_clock;

// 析构时等待后台任务退出的总预算(ms)。受管 std::thread 在 CancelAll 之后会因
// 中断回调及时退出, 这里给一个上限而不是无限等(-1), 避免极端情况下进程退出挂死。
// 注意: 这个预算现在约束的是 WaitForAll **整个函数**(含受管线程回收阶段), 不再是只管
// "等终态"。可能卡住的任务已改用 RunBlockingIo 登记 —— 那种任务超预算会被放弃(detach),
// 不会再拖住退出流程; 剩下的协作式任务有中断机制兜底, 实际不会走到上限。
constexpr int kShutdownWaitMs = 8000;

// FFmpeg 错误码 -> 可读描述。不用 av_err2str 宏 (MSVC 不支持其 compound literal 写法)。
static QString AvErrorString(int ret) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(ret, errbuf, sizeof(errbuf)) != 0) {
        return QStringLiteral("错误码 %1").arg(ret);
    }
    return QString::fromUtf8(errbuf);
}

MediaPlayer::MediaPlayer(QObject* parent)
    : QObject(parent),
      // 控制器要拿当前 url 判断"排队请求续跑前有没有换过媒体"，用取值回调而不是拷一份
      // QString —— 拷的那份在换文件后就是过期的，排队请求会拿新文件往旧目录里导。
      export_controller_(task_manager_, [this]() { return current_url_; }, this),
      container_inspection_(task_manager_, this),
      // 控制器借用本类的 analysis_session_ / playback_session_（都声明在它之前），
      // 并在构造时把分析 hook 装进播放会话 —— 所以播放机械照旧对分析一无所知。
      realtime_analysis_(analysis_session_, playback_session_, this) {
    avformat_network_init();
    qRegisterMetaType<model::Mp4BoxAnalysisResult>("model::Mp4BoxAnalysisResult");
    qRegisterMetaType<model::ContainerStructureResult>("model::ContainerStructureResult");
    qRegisterMetaType<model::MacroblockFrameAnalysis>("model::MacroblockFrameAnalysis");
    qRegisterMetaType<model::MacroblockFrameAnalysis>("videoeye::model::MacroblockFrameAnalysis");
    // 画面质量 / 视觉缺陷（解码线程 -> UI 线程，必须注册元类型才能排队投递）
    qRegisterMetaType<model::FrameQualityMetric>("model::FrameQualityMetric");
    qRegisterMetaType<model::FrameQualityMetric>("videoeye::model::FrameQualityMetric");
    qRegisterMetaType<model::VisualDefect>("model::VisualDefect");
    qRegisterMetaType<model::VisualDefect>("videoeye::model::VisualDefect");
    qRegisterMetaType<model::ActivePictureArea>("model::ActivePictureArea");
    qRegisterMetaType<model::ActivePictureArea>("videoeye::model::ActivePictureArea");

    // 播放会话的信号转发到 MediaPlayer 的同名信号: 播放机械住在 PlaybackSession 里,
    // 但对外(UI)的信号契约必须保持原样, 一个字节都不许变。
    // 这些连接都是跨线程的(sender 在解码线程), Qt 会自动判成 QueuedConnection。
    connect(&playback_session_, &PlaybackSession::StateChanged,
            this, &MediaPlayer::StateChanged);
    connect(&playback_session_, &PlaybackSession::FrameReady,
            this, &MediaPlayer::FrameReady);
    connect(&playback_session_, &PlaybackSession::PositionChanged,
            this, &MediaPlayer::PositionChanged);
    connect(&playback_session_, &PlaybackSession::Error,
            this, &MediaPlayer::Error);
    connect(&playback_session_, &PlaybackSession::PlaybackFinished,
            this, &MediaPlayer::PlaybackFinished);

    // 导出的十条信号原样转发: 外部（UI）连的是本对象的信号，拆分前后契约一个字节都不变。
    // 控制器自己也要发这些信号 —— worker 是 QObject，连接需要一个接收者，
    // 而"排队 / 代际 / 取消"的状态全在它那边，只有它能判断该不该发。
    connect(&export_controller_, &ExportController::VideoFrameExportStarted,
            this, &MediaPlayer::VideoFrameExportStarted);
    connect(&export_controller_, &ExportController::VideoFrameExportProgress,
            this, &MediaPlayer::VideoFrameExportProgress);
    connect(&export_controller_, &ExportController::VideoFrameExportFinished,
            this, &MediaPlayer::VideoFrameExportFinished);
    connect(&export_controller_, &ExportController::VideoFrameExportCanceled,
            this, &MediaPlayer::VideoFrameExportCanceled);
    connect(&export_controller_, &ExportController::VideoFrameExportError,
            this, &MediaPlayer::VideoFrameExportError);
    connect(&export_controller_, &ExportController::MediaExportStarted,
            this, &MediaPlayer::MediaExportStarted);
    connect(&export_controller_, &ExportController::MediaExportProgress,
            this, &MediaPlayer::MediaExportProgress);
    connect(&export_controller_, &ExportController::MediaExportFinished,
            this, &MediaPlayer::MediaExportFinished);
    connect(&export_controller_, &ExportController::MediaExportCanceled,
            this, &MediaPlayer::MediaExportCanceled);
    connect(&export_controller_, &ExportController::MediaExportError,
            this, &MediaPlayer::MediaExportError);

    // 容器结构分析的两条信号原样转发: 控制器在后台线程上判定"该不该弹、过没过期"，
    // 对外（UI）连的是本对象的信号，拆分前后契约一个字节都不变。
    connect(&container_inspection_, &ContainerInspectionController::ContainerStructureReady,
            this, &MediaPlayer::ContainerStructureReady);
    connect(&container_inspection_, &ContainerInspectionController::ContainerStructureFailed,
            this, &MediaPlayer::ContainerStructureFailed);

    // 实时分析的 23 条信号原样转发: 计数、采样档位、"该不该发"全在控制器里，
    // 对外（UI）连的是本对象的信号，拆分前后契约一个字节都不变。
    connect(&realtime_analysis_, &RealtimeAnalysisController::VideoFrameListReset,
            this, &MediaPlayer::VideoFrameListReset);
    connect(&realtime_analysis_, &RealtimeAnalysisController::AudioFrameListReset,
            this, &MediaPlayer::AudioFrameListReset);
    connect(&realtime_analysis_, &RealtimeAnalysisController::PacketListReset,
            this, &MediaPlayer::PacketListReset);
    connect(&realtime_analysis_, &RealtimeAnalysisController::AnalysisEventListReset,
            this, &MediaPlayer::AnalysisEventListReset);
    connect(&realtime_analysis_, &RealtimeAnalysisController::SyncSampleListReset,
            this, &MediaPlayer::SyncSampleListReset);
    connect(&realtime_analysis_, &RealtimeAnalysisController::TimelineEventListReset,
            this, &MediaPlayer::TimelineEventListReset);
    connect(&realtime_analysis_, &RealtimeAnalysisController::StreamStatsReady,
            this, &MediaPlayer::StreamStatsReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::VideoFrameInfoReady,
            this, &MediaPlayer::VideoFrameInfoReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::AudioFrameInfoReady,
            this, &MediaPlayer::AudioFrameInfoReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::PacketInfoReady,
            this, &MediaPlayer::PacketInfoReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::AnalysisEventReady,
            this, &MediaPlayer::AnalysisEventReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::SyncSampleReady,
            this, &MediaPlayer::SyncSampleReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::TimelinePacketReady,
            this, &MediaPlayer::TimelinePacketReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::FrameTimingReady,
            this, &MediaPlayer::FrameTimingReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::TimelineEventReady,
            this, &MediaPlayer::TimelineEventReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::AudioVisualizationReady,
            this, &MediaPlayer::AudioVisualizationReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::AudioLevelReady,
            this, &MediaPlayer::AudioLevelReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::MacroblockInfoReady,
            this, &MediaPlayer::MacroblockInfoReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::SceneChangeReady,
            this, &MediaPlayer::SceneChangeReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::VisualDefectReset,
            this, &MediaPlayer::VisualDefectReset);
    connect(&realtime_analysis_, &RealtimeAnalysisController::VisualDefectFrameReady,
            this, &MediaPlayer::VisualDefectFrameReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::VisualDefectReady,
            this, &MediaPlayer::VisualDefectReady);
    connect(&realtime_analysis_, &RealtimeAnalysisController::VisualDefectStatsReady,
            this, &MediaPlayer::VisualDefectStatsReady);

}

MediaPlayer::~MediaPlayer() {
    // 导出先收: 立关机标志（不再续跑排队请求）+ 取消两类导出 + 限时收线程。
    // 必须在 Stop()/Release() 之前: 导出线程自己持有 FFmpeg 上下文,
    // 让它先退干净再去 avformat_network_deinit()。
    export_controller_.Shutdown(5000);
    // 取消全部后台任务并等待退出。必须在 Stop()/Release() 之前:
    // 容器结构分析线程自己持有 AVFormatContext, 让它先退干净再去 avformat_network_deinit()。
    task_manager_.CancelAll();
    task_manager_.WaitForAll(kShutdownWaitMs);
    Stop();
    playback_session_.Release();
    avformat_network_deinit();
}

// --- 播放控制 ---

bool MediaPlayer::Open(const QString& url) {
    LOG_INFO("Open ENTER: " + url.toStdString());
    bool ok = OpenInternal(url, nullptr, nullptr);
    LOG_INFO("Open EXIT: result=" + std::string(ok ? "true" : "false"));
    return ok;
}

bool MediaPlayer::OpenRawPcm(const QString& url, const QString& demuxer_name, int sample_rate, int channels) {
    const AVInputFormat* input_format = av_find_input_format(demuxer_name.toUtf8().constData());
    if (!input_format) {
        last_open_error_ = QString("不支持的 PCM 采样格式: %1").arg(demuxer_name);
        emit OpenFailed(last_open_error_);
        return false;
    }
    AVDictionary* input_options = nullptr;
    av_dict_set(&input_options, "sample_rate", QByteArray::number(sample_rate).constData(), 0);
    av_dict_set(&input_options, "channels", QByteArray::number(channels).constData(), 0);
    return OpenInternal(url, input_format, input_options);
}

bool MediaPlayer::OpenInternal(const QString& url, const AVInputFormat* input_format, AVDictionary* input_options) {
    LOG_INFO("OpenInternal: " + url.toStdString());

    // 换媒体 = 换上下文: 两类导出(抽帧 + 音视频转码)都必须在这里统一终止。
    // 放在本函数而不是 UI 里, 是因为打开媒体的入口不止一个（Open / OpenRawPcm /
    // 播放列表切换），任何一个入口漏掉"取消导出"都会留下旧任务写旧文件、
    // 并把终态信号串回新媒体界面的问题。
    CancelAllExports();

    Stop();
    playback_session_.Release();

    current_url_ = url;
    // 换文件了: 上一次的容器结构分析结果作废(任务体投递前会查 IsCurrent)
    container_inspection_.Cancel();
    // 实时分析的复位（19 个逐帧计数器 / "已报告过"标志 / 逐帧分析器 / 清空 UI 列表）
    // 整体在控制器里 —— 逐个留在调用方等于把它的内部状态摊开在别人家。
    realtime_analysis_.ResetForNewMedia();
    analysis_session_.stream_analyzer().Reset();
    playback_session_.ResetSyncTimestamps();

    // 版本检查
    const unsigned header_avcodec_major = LIBAVCODEC_VERSION_MAJOR;
    const unsigned runtime_avcodec_major = static_cast<unsigned>(avcodec_version() >> 16);
    if (header_avcodec_major != runtime_avcodec_major) {
        last_open_error_ = QString("FFmpeg libavcodec 版本不匹配：编译期头文件=%1，运行期库=%2。")
                       .arg(header_avcodec_major).arg(runtime_avcodec_major);
        emit OpenFailed(last_open_error_);
        return false;
    }

    // 打开输入
    std::string url_str = url.toStdString();
    AVDictionary* open_options = input_options;

    // 中断回调 + 打开/探测超时。
    // 不可达的 URL、损坏文件、异常设备都会让 avformat_open_input /
    // avformat_find_stream_info 长时间阻塞在 IO 上，而这条路径是在 UI 线程同步执行的
    // —— 阻塞多久，界面就冻多久。装上回调后至少能靠绝对超时兜住。
    // 挂上取消标志: 打开/探测期间 Stop() 会置位 open_cancel_，让卡在 IO 上的
    // avformat_open_input / avformat_find_stream_info 及时返回 AVERROR_EXIT。
    // 取消标志在换文件时于打开流程开头复位，因此不会串到下一次打开。
    open_cancel_.store(false);
    open_interrupt_.cancel = &open_cancel_;
    open_interrupt_.deadline_us = 0;

    // 上下文必须自己分配: 回调要在 avformat_open_input **之前**装好。
    AVFormatContext* fmt = avformat_alloc_context();
    ffmpeg_io::AttachInterrupt(fmt, open_interrupt_, ffmpeg_io::kOpenTimeoutUs);

    int ret = avformat_open_input(&fmt, url_str.c_str(), input_format, open_options ? &open_options : nullptr);
    playback_session_.AdoptFormatContext(fmt);
    fmt = playback_session_.format_ctx();
    if (open_options) av_dict_free(&open_options);
    if (ret < 0) {
        if (ret == AVERROR_EXIT) {
            last_open_error_ = QString("打开输入超时 (超过 %1 秒): %2")
                                   .arg(ffmpeg_io::kOpenTimeoutUs / 1000000)
                                   .arg(url);
        } else {
            last_open_error_ = QString("打开输入失败: %1 | FFmpeg: %2").arg(url, AvErrorString(ret));
            // 定向诊断: fMP4 分片缺 init 段等特征, 给出可操作的修复建议
            const std::string extra = utils::DiagnoseUnopenableFile(url_str);
            if (!extra.empty()) last_open_error_ += QString::fromStdString("；" + extra);
        }
        emit OpenFailed(last_open_error_);
        return false;
    }
    LOG_INFO("OpenInternal: avformat_open_input OK");

    // 探测阶段允许更长时间，但同样受绝对超时约束
    open_interrupt_.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;
    ret = avformat_find_stream_info(fmt, nullptr);
    // 探测一结束就关掉截止时间: 之后进入解复用/播放阶段，不能让打开期的超时
    // 误杀正常的长素材读取。（回调本身留着，但已是一个恒返回 0 的空钩子。）
    open_interrupt_.deadline_us = 0;
    if (ret < 0) {
        last_open_error_ = (ret == AVERROR_EXIT)
                               ? QString("解析流信息超时 (超过 %1 秒): %2")
                                     .arg(ffmpeg_io::kProbeTimeoutUs / 1000000)
                                     .arg(url)
                               : QString("无法解析流信息 (文件可能损坏、截断或格式不受支持) | FFmpeg: %1")
                                     .arg(AvErrorString(ret));
        emit OpenFailed(last_open_error_);
        playback_session_.Release();
        return false;
    }
    LOG_INFO("OpenInternal: avformat_find_stream_info OK, streams=" + std::to_string(fmt->nb_streams));

    if (!fmt) {
        last_open_error_ = "Format context is null";
        emit OpenFailed(last_open_error_);
        return false;
    }

    // 查找流 (局部先算, 定下来之后交给播放会话持有)
    int video_index = -1;
    int audio_index = -1;
    const AVCodec* best_video_codec = nullptr;
    video_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &best_video_codec, 0);
    const AVCodec* best_audio_codec = nullptr;
    audio_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &best_audio_codec, 0);

    if (video_index < 0 && audio_index < 0) {
        last_open_error_ = "文件中未找到可播放的视频/音频流 (可能已损坏或为不支持的编码)";
        emit OpenFailed(last_open_error_);
        playback_session_.Release();
        return false;
    }

    // 处理封面图
    bool has_video = (video_index >= 0);
    playback_session_.SetStreamIndices(video_index, audio_index);
    if (video_index >= 0 && fmt->streams[video_index] &&
        (fmt->streams[video_index]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        AVStream* vs = fmt->streams[video_index];
        if (vs->codecpar && vs->attached_pic.data && vs->attached_pic.size > 0) {
            const AVCodec* cover_codec = avcodec_find_decoder(vs->codecpar->codec_id);
            if (cover_codec) {
                AVCodecContext* cover_ctx = avcodec_alloc_context3(cover_codec);
                if (cover_ctx) {
                    if (avcodec_parameters_to_context(cover_ctx, vs->codecpar) >= 0) {
                        if (vs->time_base.den != 0) {
                            cover_ctx->pkt_timebase = vs->time_base;
                            cover_ctx->time_base = vs->time_base;
                        }
                        if (avcodec_open2(cover_ctx, cover_codec, nullptr) >= 0) {
                            VideoDecoder cover_decoder;
                            if (cover_decoder.InitializeFromContext(cover_ctx)) {
                                model::FrameData cover_frame;
                                if (cover_decoder.DecodePacket(&vs->attached_pic, cover_frame)) {
                                    if (cover_frame.width > 0 && cover_frame.height > 0 && cover_frame.data[0]) {
                                        SwsContext* cover_sws = sws_getCachedContext(
                                            nullptr, cover_frame.width, cover_frame.height,
                                            static_cast<AVPixelFormat>(cover_frame.format),
                                            cover_frame.width, cover_frame.height, AV_PIX_FMT_BGRA,
                                            SWS_BILINEAR, nullptr, nullptr, nullptr);
                                        if (cover_sws) {
                                            QImage cover_img(cover_frame.width, cover_frame.height, QImage::Format_ARGB32);
                                            if (!cover_img.isNull()) {
                                                uint8_t* dst_slices[4] = {cover_img.bits(), nullptr, nullptr, nullptr};
                                                int dst_linesize[4] = {static_cast<int>(cover_img.bytesPerLine()), 0, 0, 0};
                                                sws_scale(cover_sws, cover_frame.data, cover_frame.linesize,
                                                          0, cover_frame.height, dst_slices, dst_linesize);
                                                emit FrameReady(cover_img);
                                            }
                                            sws_freeContext(cover_sws);
                                        }
                                    }
                                }
                            } else { avcodec_free_context(&cover_ctx); }
                        } else { avcodec_free_context(&cover_ctx); }
                    } else { avcodec_free_context(&cover_ctx); }
                }
            }
        }
        video_index = -1;
        playback_session_.SetStreamIndices(video_index, audio_index);
    }

    emit MediaModeChanged(has_video);

    // 初始化视频解码器
    if (video_index >= 0) {
        auto video_decoder = std::make_unique<VideoDecoder>();
        playback_session_.SetVideoDecoder(std::move(video_decoder));
        AVStream* video_stream = fmt->streams[video_index];
        if (!video_stream || !video_stream->codecpar) {
            last_open_error_ = "视频流编解码参数不可用";
            emit OpenFailed(last_open_error_);
            playback_session_.Release();
            return false;
        }

        bool hw_initialized = false;
        // 尝试硬件解码
        // 宏块分析依赖软件解码导出的运动矢量 side data, 硬件解码器不产出该数据,
        // 因此启用宏块分析时直接走软件解码路径。
        if (analysis_session_.IsHardwareDecodingEnabled() && !analysis_session_.IsMacroblockAnalysisEnabled()) {
            // VAAPI / D3D11VA / CUDA / QSV ...
            // (Vulkan HW 解码随 Vulkan 渲染器一并移除: 本项目定位是分析工具,
            //  为一条零拷贝渲染路径背一套 Vulkan 运行时不划算)
            auto hw_types = VideoDecoder::GetAvailableHwDeviceTypes();
            for (auto hw_type : hw_types) {
                if (playback_session_.video_decoder()->InitializeWithHw(video_stream->codecpar, hw_type)) {
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

        // 软件解码回退
        if (!hw_initialized) {
            const AVCodec* video_codec = best_video_codec;
            if (!video_codec) video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
            if (!video_codec) {
                last_open_error_ = QString("找不到视频解码器 (codec_id=%1)")
                                       .arg(avcodec_get_name(video_stream->codecpar->codec_id));
                emit OpenFailed(last_open_error_);
                playback_session_.Release();
                return false;
            }
            AVCodecContext* video_codec_ctx = avcodec_alloc_context3(video_codec);
            if (!video_codec_ctx) {
                last_open_error_ = "无法创建视频解码器上下文";
                emit OpenFailed(last_open_error_);
                playback_session_.Release();
                return false;
            }
            ret = avcodec_parameters_to_context(video_codec_ctx, video_stream->codecpar);
            if (ret < 0) {
                avcodec_free_context(&video_codec_ctx);
                last_open_error_ = QString("复制视频编解码参数失败: %1").arg(AvErrorString(ret));
                emit OpenFailed(last_open_error_);
                playback_session_.Release();
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
                last_open_error_ = QString("打开视频解码器失败 (%1): %2")
                                       .arg(avcodec_get_name(video_codec->id), AvErrorString(ret));
                emit OpenFailed(last_open_error_);
                playback_session_.Release();
                return false;
            }
            if (!playback_session_.video_decoder()->InitializeFromContext(video_codec_ctx)) {
                avcodec_free_context(&video_codec_ctx);
                last_open_error_ = "初始化视频解码器失败";
                emit OpenFailed(last_open_error_);
                playback_session_.Release();
                return false;
            }
        }
        LOG_INFO("Video decoder initialized: " + std::to_string(playback_session_.video_decoder()->GetWidth()) + "x" +
                 std::to_string(playback_session_.video_decoder()->GetHeight()) +
                 (playback_session_.video_decoder()->IsHardwareDecoding() ? " (HW)" : " (SW)"));
    }

    // 初始化音频解码器
    if (audio_index >= 0) {
        auto audio_decoder = std::make_unique<AudioDecoder>();
        playback_session_.SetAudioDecoder(std::move(audio_decoder));
        AVStream* audio_stream = fmt->streams[audio_index];
        if (!audio_stream->codecpar) {
            last_open_error_ = "音频流编解码参数不可用";
            emit OpenFailed(last_open_error_);
            playback_session_.Release();
            return false;
        }
        if (!playback_session_.audio_decoder()->Initialize(audio_stream->codecpar)) {
            last_open_error_ = "初始化音频解码器失败";
            emit OpenFailed(last_open_error_);
            playback_session_.Release();
            return false;
        }
        LOG_INFO("Audio decoder initialized");

        // 初始化音频输出设备（平台原生后端: WASAPI / ALSA / AudioQueue）:
        // 解码后的 PCM 由音频线程主动 pull。
        // 走异步打开: Windows(WASAPI) 首次打开音频设备实测 ~1s,
        // 同步调用会把"打开文件"整段卡住。设备就绪前音频帧丢弃, 视频不受影响。
        auto audio_output = std::make_unique<AudioOutput>();
        audio_output->SetVolume(volume_ / 100.0);
        audio_output->OpenAsync(playback_session_.audio_decoder()->GetSampleRate(),
                                playback_session_.audio_decoder()->GetChannels());
        playback_session_.SetAudioOutput(std::move(audio_output));
    }

    // 提取流信息 (委托给 StreamInfoExtractor)
    auto extract_result = stream_info_extractor_.Extract(fmt, video_index, audio_index, url);
    stream_info_ = extract_result.info;
    playback_session_.SetDuration(extract_result.duration_ms);
    playback_session_.SetPosition(0);

    playback_session_.SetIdle();

    // 触发容器结构分析 (统一调度) — 放到后台线程执行。
    // 容器结构分析是纯展示信息 (面板里看 Box 树 / 样本表), 不应阻塞打开 / 播放。
    // 之前同步跑在 UI 线程, 大文件的 sample 表展开会让界面冻结数秒~数十秒。
    // 改为后台线程, 完成后通过信号 (跨线程自动排队) 投递结果。
    if (analysis_session_.IsContainerStructureEnabled()) {
        container_inspection_.Start(url);
    }

    last_open_error_.clear();
    LOG_INFO("OpenInternal success: " + url.toStdString());
    return true;
}

void MediaPlayer::RequestContainerStructureAnalysis(const QString& url) {
    // 供播放器 Open 失败后的"分析模式"使用: 文件打不开/播不了也要尽量给出文件级结构信息。
    // 不需要手工回收旧线程: TaskManager 在 Begin 时自动取代同 slot 的旧任务。
    if (analysis_session_.IsContainerStructureEnabled()) {
        container_inspection_.Start(url);
    }
}

void MediaPlayer::Play() {
    LOG_INFO("Play ENTER: state=" +
             std::to_string(static_cast<int>(playback_session_.state())));
    if (!playback_session_.format_ctx()) { emit Error("No media opened"); return; }
    // 起停解码线程、播放状态机、音频设备都在 PlaybackSession 里
    playback_session_.Play();
}

void MediaPlayer::Pause() {
    LOG_INFO("Pause");
    playback_session_.Pause();
}

void MediaPlayer::Stop() {
    LOG_INFO("Stop");
    // 先置中断、再停会话：网络源此刻可能正卡在 avformat_open_input / av_read_frame 上，
    // 置位后那些阻塞 IO 会立刻退出，playback_session_.Stop() 的 join 才等得到返回。
    // 打开/探测阶段的截止时间一并不留（deadline 只服务于打开期，播放期的正常长读不该被误杀）。
    open_cancel_.store(true);
    open_interrupt_.deadline_us = 0;
    playback_session_.Stop();
    // 停止播放时闭合未结束的缺陷段（UI 立刻能看到最后一条）
    realtime_analysis_.FlushOnStop();
}

void MediaPlayer::Seek(int position_ms, model::SeekMode mode) {
    LOG_INFO("Seek: target_ms=" + std::to_string(position_ms) +
             " mode=" + std::to_string(static_cast<int>(mode)));
    // 只投递 seek 请求: AVFormatContext 由解码线程独占调用 av_seek_frame / av_read_frame,
    // 避免 UI 线程与解码线程并发访问同一个 FFmpeg 上下文。
    playback_session_.Seek(position_ms, mode);
}

void MediaPlayer::SetVolume(int volume) {
    volume_ = std::max(0, std::min(100, volume));
    if (playback_session_.audio_output()) playback_session_.audio_output()->SetVolume(volume_ / 100.0);
}

void MediaPlayer::EnableAnalysis(bool enable) {
    analysis_session_.SetAnalysisEnabled(enable);
    if (enable) {
        analysis_session_.Start();
        LOG_INFO("已启用视频分析");
    } else {
        analysis_session_.Stop();
        LOG_INFO("已禁用视频分析");
    }
}

void MediaPlayer::SetFrameTypeAnalysisEnabled(bool enable) { analysis_session_.SetFrameTypeAnalysisEnabled(enable); }

// 原来这些是头文件的内联转发。收进 .cpp 是因为它们现在只是 SetAnalysisFeature()
// 的分派目标，不打算再被直接调用 —— 放在 .cpp 里既避免头文件暴露实现，也让
// "新增一个维度要改什么"这件事只落在一个文件里。
void MediaPlayer::SetAudioFrameAnalysisEnabled(bool enable) { analysis_session_.SetAudioFrameAnalysisEnabled(enable); }
void MediaPlayer::SetPacketAnalysisEnabled(bool enable) { analysis_session_.SetPacketAnalysisEnabled(enable); }
void MediaPlayer::SetEventAnalysisEnabled(bool enable) { analysis_session_.SetEventAnalysisEnabled(enable); }
void MediaPlayer::SetSyncAnalysisEnabled(bool enable) { analysis_session_.SetSyncAnalysisEnabled(enable); }
void MediaPlayer::SetTimelineAnalysisEnabled(bool enable) { analysis_session_.SetTimelineAnalysisEnabled(enable); }
void MediaPlayer::SetContainerStructureEnabled(bool enable) { analysis_session_.SetContainerStructureEnabled(enable); }
void MediaPlayer::SetSceneChangeAnalysisEnabled(bool enable) { analysis_session_.SetSceneChangeAnalysisEnabled(enable); }

void MediaPlayer::SetAnalysisFeature(model::AnalysisFeature feature, bool enable) {
    switch (feature) {
    case model::AnalysisFeature::Master:
    case model::AnalysisFeature::StreamStats:
        EnableAnalysis(enable);
        break;
    case model::AnalysisFeature::VideoFrame:
        SetFrameTypeAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::AudioFrame:
        SetAudioFrameAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Packet:
        SetPacketAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Event:
        SetEventAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::SyncSample:
        SetSyncAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Timeline:
        SetTimelineAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::ContainerStructure:
        SetContainerStructureEnabled(enable);
        break;
    case model::AnalysisFeature::Macroblock:
        SetMacroblockAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::SceneChange:
        SetSceneChangeAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::VisualDefect:
        SetVisualDefectAnalysisEnabled(enable);
        break;
    case model::AnalysisFeature::Diagnostics:
        // 全文件扫描 + QC 不在播放会话里，由面板侧的 DiagnosticsPage 单独驱动，
        // 这里刻意什么都不做（以前 MainWindow 的 switch 也是落到 default 分支）。
        break;
    }
}

// --- 画面质量 / 视觉缺陷 ---

// 画面质量 / 视觉缺陷: 开关、采样档位、收尾全部在 RealtimeAnalysisController 里
// （见 core/player/RealtimeAnalysisController.h），本类只保留对外入口与信号转发。

void MediaPlayer::SetVisualDefectAnalysisEnabled(bool enable) {
    realtime_analysis_.SetVisualDefectAnalysisEnabled(enable);
}

analyzer::VisualDefectOptions MediaPlayer::GetVisualDefectOptions() const {
    return realtime_analysis_.GetVisualDefectOptions();
}

void MediaPlayer::SetVisualDefectOptions(const analyzer::VisualDefectOptions& options) {
    realtime_analysis_.SetVisualDefectOptions(options);
}

void MediaPlayer::FlushVisualDefectSegments(double end_timestamp_seconds) {
    realtime_analysis_.FlushVisualDefectSegments(end_timestamp_seconds);
}

void MediaPlayer::SetMacroblockAnalysisEnabled(bool enable) {
    bool was_enabled = analysis_session_.IsMacroblockAnalysisEnabled();
    analysis_session_.SetMacroblockAnalysisEnabled(enable);
    if (enable && !was_enabled) {
        LOG_INFO("宏块分析已启用");
        // 运动矢量仅在软件解码下由 FFmpeg 导出; 硬件解码 (Vulkan/D3D11/CUDA) 不产出
        // AV_FRAME_DATA_MOTION_VECTORS side data。若当前正在使用硬件解码, 则重新以
        // 软件解码打开当前文件并恢复播放位置, 否则宏块分析面板将始终为空。
        if (playback_session_.video_decoder() &&
            playback_session_.video_decoder()->IsHardwareDecoding() && !current_url_.isEmpty()) {
            LOG_WARN("当前为硬件解码, 无法导出运动矢量。自动切换为软件解码以启用宏块分析...");
            int resume_ms = playback_session_.current_position_ms();
            bool was_playing = (playback_session_.state() == model::PlayerState::Playing);
            QString url = current_url_;
            if (Open(url)) {
                if (resume_ms > 0) Seek(resume_ms);
                if (was_playing) Play();
            }
        }
    } else if (!enable && was_enabled) {
        LOG_INFO("宏块分析已禁用");
    }
}

analyzer::StreamStats MediaPlayer::GetCurrentStats() const { return analysis_session_.stream_analyzer().GetStats(); }

// --- 导出: 全部委托给 ExportController ---
//
// 抽帧 / 音视频导出的发起、排队、代际过滤、取消与 worker 线程生命周期都在
// core/player/ExportController.cpp，本类只保留对外入口与信号转发
// （连接见构造函数）。拆分的理由与"绝不能改"的几条约定都写在那个文件头。

void MediaPlayer::StartVideoFrameExport(const QString& output_dir, const QString& format,
                                        int jpg_quality, int frame_interval) {
    export_controller_.StartVideoFrameExport(output_dir, format, jpg_quality, frame_interval);
}

void MediaPlayer::CancelVideoFrameExport() {
    export_controller_.CancelVideoFrameExport();
}

void MediaPlayer::StartMediaExport(const exporter::ExportOptions& opt) {
    export_controller_.StartMediaExport(opt);
}

void MediaPlayer::CancelMediaExport() {
    export_controller_.CancelMediaExport();
}

void MediaPlayer::CancelAllExports() {
    export_controller_.CancelAllExports();
}

// --- 解码线程 ---

// --- 播放回调 (由 PlaybackSession 的解码线程调用) ---
//
// 切分后解码循环只做 demux / 解码 / pacing / 画面输出; "要不要发信号、要不要计数"
// 这类分析语义全部留在下面这些 hook 里, 与分析器、计数器放在一起。
// 调用线程与改造前一致(解码线程), 因此信号仍然由 Qt 自动排队到 UI 线程。

bool MediaPlayer::IsHardwareDecoding() const {
    const VideoDecoder* decoder = playback_session_.video_decoder();
    return decoder && decoder->IsHardwareDecoding();
}

std::string MediaPlayer::GetHwDeviceName() const {
    const VideoDecoder* decoder = playback_session_.video_decoder();
    if (!decoder || !decoder->IsHardwareDecoding()) return "none";
    return av_hwdevice_get_type_name(decoder->GetHwDeviceType());
}


} // namespace player
} // namespace videoeye
