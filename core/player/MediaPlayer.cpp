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

// FFmpeg 错误码 -> 可读描述。不用 av_err2str 宏 (MSVC 不支持其 compound literal 写法)。
static QString AvErrorString(int ret) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(ret, errbuf, sizeof(errbuf)) != 0) {
        return QStringLiteral("错误码 %1").arg(ret);
    }
    return QString::fromUtf8(errbuf);
}

// 计算单帧 256-bin 灰度直方图 (供场景切换检测使用)。
// 通过 sws_scale 将任意像素格式转为 GRAY8, 再逐字节计数; 不依赖 OpenCV。
// 返回空 vector 表示转换失败 (调用方应跳过该帧)。
static std::vector<float> ComputeGrayHistogram(const model::FrameData& frame) {
    std::vector<float> hist;
    if (frame.width <= 0 || frame.height <= 0 || frame.format < 0 ||
        !frame.data[0] || frame.linesize[0] <= 0) {
        return hist;
    }

    const auto pix_fmt = static_cast<AVPixelFormat>(frame.format);
    SwsContext* sws = sws_getContext(
        frame.width, frame.height, pix_fmt,
        frame.width, frame.height, AV_PIX_FMT_GRAY8,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        LOG_WARN("无法创建 sws_scale 上下文 (灰度直方图)");
        return hist;
    }

    std::vector<uint8_t> gray(static_cast<size_t>(frame.width) * frame.height);
    uint8_t* dst_data[1] = { gray.data() };
    const int dst_linesize[1] = { frame.width };
    const int ret = sws_scale(sws, frame.data, frame.linesize, 0, frame.height,
                              dst_data, dst_linesize);
    sws_freeContext(sws);
    if (ret <= 0) {
        LOG_WARN("sws_scale 灰度转换失败");
        return hist;
    }

    hist.assign(256, 0.0f);
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* line = gray.data() + static_cast<size_t>(y) * frame.width;
        for (int x = 0; x < frame.width; ++x) {
            ++hist[line[x]];
        }
    }
    return hist;  // SceneChangeAnalyzer::Feed 内部会归一化, 这里返回原始计数即可
}


MediaPlayer::MediaPlayer(QObject* parent)
    : QObject(parent) {
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

    // 分析语义留在 MediaPlayer: 由它把 hook 装进播放会话
    InstallPlaybackHooks();
}

MediaPlayer::~MediaPlayer() {
    CancelVideoFrameExport();
    CancelMediaExport();
    if (media_export_thread_) {
        media_export_thread_->quit();
        media_export_thread_->wait(5000);
    }
    // 取消全部后台任务并等待退出。必须在 Stop()/Release() 之前:
    // 容器结构分析线程自己持有 AVFormatContext, 让它先退干净再去 avformat_network_deinit()。
    task_manager_.CancelAll();
    task_manager_.WaitForAll(-1);
    Stop();
    playback_session_.Release();
    avformat_network_deinit();
}

// --- 分析事件发射 ---

void MediaPlayer::EmitAnalysisEvent(const QString& severity, const QString& type, int stream_index,
                                    qint64 pts, double timestamp_seconds,
                                    const QString& summary, const QString& detail) {
    if (!analysis_session_.IsEventAnalysisEnabled()) return;
    model::AnalysisEvent event_info;
    event_info.index = analysis_event_index_++;
    event_info.severity = severity;
    event_info.type = type;
    event_info.stream_index = stream_index;
    event_info.pts = pts;
    event_info.timestamp_seconds = timestamp_seconds;
    event_info.summary = summary;
    event_info.detail = detail;
    emit AnalysisEventReady(event_info);
    EmitTimelineEvent(QStringLiteral("事件"), timestamp_seconds, summary, detail);
}

void MediaPlayer::EmitSyncSample(double audio_ts, double video_ts, bool audio_anchor) {
    if (!analysis_session_.IsSyncAnalysisEnabled()) return;
    if (!std::isfinite(audio_ts) || !std::isfinite(video_ts)) return;
    model::SyncSample sample;
    sample.index = sync_sample_index_++;
    sample.audio_timestamp_seconds = audio_ts;
    sample.video_timestamp_seconds = video_ts;
    sample.diff_ms = (audio_ts - video_ts) * 1000.0;
    sample.audio_anchor = audio_anchor;
    emit SyncSampleReady(sample);
}

void MediaPlayer::EmitTimelineEvent(const QString& category, double timestamp_seconds,
                                    const QString& label, const QString& detail) {
    if (!analysis_session_.IsTimelineAnalysisEnabled()) return;
    if (!std::isfinite(timestamp_seconds)) return;
    model::TimelineEvent event;
    event.index = timeline_event_index_++;
    event.category = category;
    event.timestamp_seconds = timestamp_seconds;
    event.label = label;
    event.detail = detail;
    emit TimelineEventReady(event);
}

void MediaPlayer::EmitAudioVisualization(const AudioVisualizationResult& vis,
                                          int sample_rate, int channels,
                                          double timestamp_seconds, double level) {
    model::AudioVisualizationFrame frame;
    frame.index = audio_visualization_index_++;
    frame.timestamp_seconds = timestamp_seconds;
    frame.level = level;
    frame.sample_rate = sample_rate;
    frame.channels = channels;
    frame.waveform_points = QVector<double>(vis.waveform_points.begin(), vis.waveform_points.end());
    frame.spectrum_bins = QVector<double>(vis.spectrum_bins.begin(), vis.spectrum_bins.end());
    frame.peak_dbfs = vis.peak_dbfs;
    frame.loudness_momentary_lufs = vis.loudness_momentary_lufs;
    frame.true_peak_dbtp = vis.true_peak_dbtp;
    emit AudioVisualizationReady(frame);
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
    Stop();
    playback_session_.Release();

    current_url_ = url;
    // 换文件了: 上一次的容器结构分析结果作废(任务体投递前会查 IsCurrent)
    task_manager_.Cancel(kSlotContainerStructure);
    video_frame_index_ = 0;
    macroblock_frame_index_ = 0;
    scene_change_frame_index_ = 0;
    scene_change_analyzer_.Reset();
    visual_defect_frame_index_ = 0;
    visual_defect_last_sample_ts_ = -1.0;
    last_audio_level_ = 0.0;
    visual_defect_analyzer_.Reset(GetVisualDefectOptions());
    if (analysis_session_.IsVisualDefectAnalysisEnabled() &&
        GetVisualDefectOptions().preset != analyzer::VisualSamplingPreset::OfflineFull) {
        visual_defect_analyzer_.StartWorker(8);
    }
    analysis_session_.stream_analyzer().Reset();
    audio_frame_index_ = 0;
    packet_index_ = 0;
    timeline_packet_index_ = 0;
    analysis_event_index_ = 0;
    sync_sample_index_ = 0;
    timeline_event_index_ = 0;
    audio_visualization_index_ = 0;
    audio_timeline_sample_counter_ = 0;
    last_packet_ts_by_stream_.clear();
    missing_packet_ts_reported_.clear();
    missing_audio_pts_reported_.clear();
    playback_session_.ResetSyncTimestamps();
    emit VideoFrameListReset();
    if (analysis_session_.IsAudioFrameAnalysisEnabled()) emit AudioFrameListReset();
    if (analysis_session_.IsPacketAnalysisEnabled()) emit PacketListReset();
    if (analysis_session_.IsEventAnalysisEnabled()) emit AnalysisEventListReset();
    if (analysis_session_.IsSyncAnalysisEnabled()) emit SyncSampleListReset();
    if (analysis_session_.IsTimelineAnalysisEnabled()) emit TimelineEventListReset();
    if (analysis_session_.IsVisualDefectAnalysisEnabled()) emit VisualDefectReset();

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
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, url_str.c_str(), input_format, open_options ? &open_options : nullptr);
    playback_session_.AdoptFormatContext(fmt);
    fmt = playback_session_.format_ctx();
    if (open_options) av_dict_free(&open_options);
    if (ret < 0) {
        last_open_error_ = QString("打开输入失败: %1 | FFmpeg: %2").arg(url, AvErrorString(ret));
        // 定向诊断: fMP4 分片缺 init 段等特征, 给出可操作的修复建议
        const std::string extra = utils::DiagnoseUnopenableFile(url_str);
        if (!extra.empty()) last_open_error_ += QString::fromStdString("；" + extra);
        emit OpenFailed(last_open_error_);
        return false;
    }
    LOG_INFO("OpenInternal: avformat_open_input OK");

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        last_open_error_ = QString("无法解析流信息 (文件可能损坏、截断或格式不受支持) | FFmpeg: %1").arg(AvErrorString(ret));
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
        StartContainerStructureAnalysis(url);
    }

    last_open_error_.clear();
    LOG_INFO("OpenInternal success: " + url.toStdString());
    return true;
}

void MediaPlayer::RequestContainerStructureAnalysis(const QString& url) {
    // 供播放器 Open 失败后的"分析模式"使用: 文件打不开/播不了也要尽量给出文件级结构信息。
    // 不需要手工回收旧线程: TaskManager 在 Begin 时自动取代同 slot 的旧任务。
    if (analysis_session_.IsContainerStructureEnabled()) {
        StartContainerStructureAnalysis(url);
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
    playback_session_.Stop();
    // 停止播放时闭合未结束的缺陷段（UI 立刻能看到最后一条）
    if (analysis_session_.IsVisualDefectAnalysisEnabled()) {
        FlushVisualDefectSegments(visual_defect_last_sample_ts_ > 0.0 ? visual_defect_last_sample_ts_
                                                                      : 0.0);
    }
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

// --- 画面质量 / 视觉缺陷 ---

void MediaPlayer::SetVisualDefectAnalysisEnabled(bool enable) {
    if (analysis_session_.IsVisualDefectAnalysisEnabled() == enable) return;
    analysis_session_.SetVisualDefectAnalysisEnabled(enable);
    if (enable) {
        const analyzer::VisualDefectOptions vd_opts = GetVisualDefectOptions();
        visual_defect_analyzer_.Reset(vd_opts);
        visual_defect_frame_index_ = 0;
        visual_defect_last_sample_ts_ = -1.0;
        // 实时档位走工作线程 + 有上限队列: 播放压力大时丢分析帧，绝不反压解码线程。
        // 离线全帧档位不需要队列（Feed 是同步的），开线程反而多一次拷贝。
        if (vd_opts.preset != analyzer::VisualSamplingPreset::OfflineFull) {
            visual_defect_analyzer_.StartWorker(8);
        }
        LOG_INFO("画面质量检测已启用");
        emit VisualDefectReset();
    } else {
        FlushVisualDefectSegments(visual_defect_last_sample_ts_ > 0.0 ? visual_defect_last_sample_ts_ : 0.0);
        visual_defect_analyzer_.StopWorker();
        LOG_INFO("画面质量检测已禁用");
    }
}

analyzer::VisualDefectOptions MediaPlayer::GetVisualDefectOptions() const {
    return analysis_session_.GetVisualDefectOptions();
}

void MediaPlayer::SetVisualDefectOptions(const analyzer::VisualDefectOptions& options) {
    const bool was_enabled = analysis_session_.IsVisualDefectAnalysisEnabled();
    analysis_session_.SetVisualDefectOptions(options);
    if (!was_enabled) return;
    // 档位变了（尤其"离线全帧"开关）要重建线程与状态
    visual_defect_analyzer_.Reset(options);
    visual_defect_frame_index_ = 0;
    visual_defect_last_sample_ts_ = -1.0;
    if (options.preset != analyzer::VisualSamplingPreset::OfflineFull) {
        visual_defect_analyzer_.StartWorker(8);
    }
    emit VisualDefectReset();
}

void MediaPlayer::FlushVisualDefectSegments(double end_timestamp_seconds) {
    visual_defect_analyzer_.Flush(end_timestamp_seconds);
    DrainVisualDefectResults();
    EmitVisualDefectStats(true);
}

void MediaPlayer::DrainVisualDefectResults() {
    const auto metrics = visual_defect_analyzer_.TakePendingMetrics();
    for (const auto& m : metrics) {
        emit VisualDefectFrameReady(m);
    }
    const auto defects = visual_defect_analyzer_.TakePendingDefects();
    for (const auto& d : defects) {
        emit VisualDefectReady(d);
    }
}

void MediaPlayer::FeedVisualDefectFrame(const AVFrame* frame, double timestamp_seconds,
                                        bool audio_silent) {
    if (!frame) return;

    // 解码线程逐帧读取视觉缺陷选项: 取一次加锁副本, 避免反复加锁且与 UI 线程写入互斥。
    const analyzer::VisualDefectOptions vd_opts = GetVisualDefectOptions();

    const double fps = vd_opts.EffectiveSampleFps();
    if (fps > 0.0 && visual_defect_last_sample_ts_ >= 0.0 &&
        (timestamp_seconds - visual_defect_last_sample_ts_) + 1e-6 < 1.0 / fps) {
        return;   // 还没到下一次采样点
    }

    // 时间跳变（seek 后退 / 大跨度前进）: 先闭合上一段。
    // 否则 seek 前后两段不连续的画面会被连成一条超长缺陷（比如"从 3s 跳到 60s"里的冻结）。
    // 2 秒这个量取得比最慢采样档位（1 fps = 1 秒间隔）还宽，正常播放不会误触发。
    if (visual_defect_last_sample_ts_ >= 0.0 &&
        (timestamp_seconds + 0.5 < visual_defect_last_sample_ts_ ||
         timestamp_seconds - visual_defect_last_sample_ts_ > 2.0)) {
        visual_defect_analyzer_.Flush(visual_defect_last_sample_ts_);
        DrainVisualDefectResults();
    }

    model::FrameSample sample;
    std::string error;
    if (!analyzer::QualityAnalyzer::BuildSample(frame,
                                                vd_opts.EffectiveAnalysisWidth(),
                                                vd_opts.EffectiveEvidenceWidth(),
                                                vd_opts.capture_rgb,
                                                sample, error)) {
        LOG_WARN("画面质量分析: 降采样失败 - " + error);
        return;
    }
    sample.frame_index = visual_defect_frame_index_++;
    sample.timestamp_seconds = timestamp_seconds;
    sample.audio_silent = audio_silent;
    visual_defect_last_sample_ts_ = timestamp_seconds;

    if (vd_opts.preset == analyzer::VisualSamplingPreset::OfflineFull) {
        // 离线档位: 同步分析每一帧，一帧都不丢（代价是播放会变慢）
        visual_defect_analyzer_.Feed(sample);
    } else if (!visual_defect_analyzer_.Submit(sample)) {
        DrainVisualDefectResults();
        EmitVisualDefectStats(false);
        return;   // 队列满，本帧丢弃（计数在分析器里，UI 会显示）
    }
    DrainVisualDefectResults();
    EmitVisualDefectStats(false);
}

void MediaPlayer::EmitVisualDefectStats(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - visual_defect_last_stats_emit_).count();
        if (visual_defect_last_stats_emit_.time_since_epoch().count() != 0 && elapsed < 1000) {
            return;
        }
    }
    visual_defect_last_stats_emit_ = now;
    emit VisualDefectStatsReady(visual_defect_analyzer_.analyzed_samples(),
                                visual_defect_analyzer_.dropped_samples(),
                                visual_defect_analyzer_.EffectiveArea());
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

// --- 视频帧导出 (委托给 VideoFrameExporter) ---

void MediaPlayer::StartVideoFrameExport(const QString& output_dir, const QString& format, int jpg_quality, int frame_interval) {
    CancelVideoFrameExport();
    if (current_url_.isEmpty()) { emit VideoFrameExportError("No media opened"); return; }
    if (output_dir.isEmpty()) { emit VideoFrameExportError("Output directory is empty"); return; }
    QDir dir(output_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        emit VideoFrameExportError(QString("Failed to create output directory: %1").arg(output_dir));
        return;
    }

    // 登记到统一任务调度 (任务 ID / 取消标志 / 终态)
    const task::TaskId task_id = task_manager_.Begin(kSlotFrameExport);
    if (task_id == 0) {
        emit VideoFrameExportError("已有后台任务在运行, 请稍后再试");
        return;
    }

    const QString url = current_url_;
    const QString normalized_format = format.toLower();
    const int normalized_interval = std::max(1, frame_interval);

    auto* thread = new QThread(this);
    auto* exporter = new VideoFrameExporter();
    exporter->moveToThread(thread);

    frame_export_thread_ = thread;
    frame_exporter_ = exporter;

    // 转发信号
    connect(exporter, &VideoFrameExporter::ExportStarted, this, &MediaPlayer::VideoFrameExportStarted);
    connect(exporter, &VideoFrameExporter::ExportProgress, this, &MediaPlayer::VideoFrameExportProgress);
    connect(exporter, &VideoFrameExporter::ExportFinished, this, &MediaPlayer::VideoFrameExportFinished);
    connect(exporter, &VideoFrameExporter::ExportCanceled, this, &MediaPlayer::VideoFrameExportCanceled);
    connect(exporter, &VideoFrameExporter::ExportError, this, &MediaPlayer::VideoFrameExportError);

    // 终态上报 (登记在 quit 之前, 保证 WaitForIdle 看到的是终态而不是"还在跑")
    connect(exporter, &VideoFrameExporter::ExportFinished, this, [this, task_id](const QString&) {
        task_manager_.End(kSlotFrameExport, task_id, task::TaskState::Succeeded);
    });
    connect(exporter, &VideoFrameExporter::ExportCanceled, this,
            [this, task_id](int, const QString&) {
                task_manager_.End(kSlotFrameExport, task_id, task::TaskState::Canceled);
            });
    connect(exporter, &VideoFrameExporter::ExportError, this, [this, task_id](const QString&) {
        task_manager_.End(kSlotFrameExport, task_id, task::TaskState::Failed);
    });

    connect(exporter, &VideoFrameExporter::ExportFinished, thread, &QThread::quit);
    connect(exporter, &VideoFrameExporter::ExportCanceled, thread, &QThread::quit);
    connect(exporter, &VideoFrameExporter::ExportError, thread, &QThread::quit);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, exporter, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread, exporter]() {
        if (frame_export_thread_ == thread) frame_export_thread_ = nullptr;
        if (frame_exporter_ == exporter) frame_exporter_ = nullptr;
    });
    connect(thread, &QThread::started, exporter,
            [exporter, url, output_dir, normalized_format, jpg_quality, normalized_interval]() {
                exporter->Export(url, output_dir, normalized_format, jpg_quality, normalized_interval);
            });
    thread->start();
}

void MediaPlayer::CancelVideoFrameExport() {
    // 统一置取消标志(供外部查询 / 后续 Begin 取代旧任务)
    task_manager_.Cancel(kSlotFrameExport);

    VideoFrameExporter* exporter = frame_exporter_;
    QThread* thread = frame_export_thread_;
    if (!exporter && !thread) return;

    if (exporter) exporter->Cancel();
    if (thread) {
        thread->quit();
        // 从 worker 线程自己调进来时不能 wait(), 否则等自己退出 = 直接死锁
        if (thread != QThread::currentThread()) thread->wait(5000);
    }
    // thread->finished 的清理回调是排队投递的, wait() 返回时不一定已经跑过, 这里兜底清指针。
    // (原实现在 thread 为空时仍会 thread->quit(), 直接空指针崩溃)
    if (frame_exporter_ == exporter) frame_exporter_ = nullptr;
    if (frame_export_thread_ == thread) frame_export_thread_ = nullptr;
}

// --- 音视频导出 (后台线程运行 MediaExporter) ---

void MediaPlayer::StartMediaExport(const exporter::ExportOptions& opt) {
    // 先彻底结束可能正在进行的旧导出, 避免同一输出路径上的新旧任务并发冲突与状态错乱。
    // 旧任务取消后写入的是独立临时文件, 但等待其退出可保证不会与本次导出重叠写入同一目标。
    // 先取消同 slot 上的旧导出并等其退出, 再登记新任务
    task_manager_.Cancel(kSlotMediaExport);
    if (media_export_thread_) {
        if (media_exporter_) media_exporter_->Cancel();
        media_export_thread_->quit();
        media_export_thread_->wait(30000);
        media_export_thread_ = nullptr;
        media_exporter_ = nullptr;
    }

    const task::TaskId task_id = task_manager_.Begin(kSlotMediaExport);
    if (task_id == 0) { emit MediaExportError("已有后台任务在运行, 请稍后再试"); return; }
    auto fail = [this, task_id](const QString& msg) {
        task_manager_.End(kSlotMediaExport, task_id, task::TaskState::Failed);
        emit MediaExportError(msg);
    };
    if (opt.input_path.isEmpty()) { fail("未打开媒体文件"); return; }
    if (opt.output_path.isEmpty()) { fail("未指定输出路径"); return; }
    // 注意: 不再预先删除目标文件。MediaExporter 先写同目录临时文件, 成功后才原子替换目标,
    // 因此导出失败/取消时原文件完好保留, 不会出现"旧文件被删、新导出又失败"的数据丢失。

    auto* thread = new QThread(this);
    auto* exporter = new exporter::MediaExporter();
    exporter->moveToThread(thread);

    media_export_thread_ = thread;
    media_exporter_ = exporter;

    connect(exporter, &exporter::MediaExporter::ExportStarted, this, &MediaPlayer::MediaExportStarted);
    connect(exporter, &exporter::MediaExporter::ExportProgress, this, &MediaPlayer::MediaExportProgress);
    connect(exporter, &exporter::MediaExporter::ExportFinished, this, &MediaPlayer::MediaExportFinished);
    connect(exporter, &exporter::MediaExporter::ExportCanceled, this, &MediaPlayer::MediaExportCanceled);
    connect(exporter, &exporter::MediaExporter::ExportError, this, &MediaPlayer::MediaExportError);

    // 终态上报 (登记在 quit 之前, 保证 WaitForIdle 看到的是终态而不是"还在跑")
    connect(exporter, &exporter::MediaExporter::ExportFinished, this,
            [this, task_id](const QString&) {
                task_manager_.End(kSlotMediaExport, task_id, task::TaskState::Succeeded);
            });
    connect(exporter, &exporter::MediaExporter::ExportCanceled, this,
            [this, task_id](const QString&) {
                task_manager_.End(kSlotMediaExport, task_id, task::TaskState::Canceled);
            });
    connect(exporter, &exporter::MediaExporter::ExportError, this, [this, task_id](const QString&) {
        task_manager_.End(kSlotMediaExport, task_id, task::TaskState::Failed);
    });

    // 导出结束 -> 退出线程, 随后自清理 (不触碰 this 的成员指针, 避免覆盖新导出)
    connect(exporter, &exporter::MediaExporter::ExportFinished, thread, &QThread::quit);
    connect(exporter, &exporter::MediaExporter::ExportCanceled, thread, &QThread::quit);
    connect(exporter, &exporter::MediaExporter::ExportError, thread, &QThread::quit);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, exporter, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread, exporter]() {
        if (media_export_thread_ == thread) media_export_thread_ = nullptr;
        if (media_exporter_ == exporter) media_exporter_ = nullptr;
    });

    // 线程启动后执行导出 (在 worker 线程中同步运行)
    connect(thread, &QThread::started, exporter, [exporter, opt]() {
        exporter->Export(opt);
    });

    thread->start();
}

void MediaPlayer::CancelMediaExport() {
    // 统一置取消标志
    task_manager_.Cancel(kSlotMediaExport);
    if (media_exporter_) media_exporter_->Cancel();
}

void MediaPlayer::StartContainerStructureAnalysis(const QString& url) {
    const QString url_copy = url;
    QPointer<MediaPlayer> self = this;

    // 交给统一的任务调度: 同 slot 上只允许一个任务, 换文件时旧任务被取消且结果作废,
    // 线程由 TaskManager 持有并在下次启动 / 析构时回收, 不再每次分析都新建并堆积线程。
    task_manager_.Run(kSlotContainerStructure,
                      [self, url_copy](task::TaskId id, task::CancelToken token) {
        model::ContainerStructureResult cs_result;
        bool ok = false;
        try {
            analyzer::ContainerStructureAnalyzer analyzer;
            ok = analyzer.Analyze(url_copy, cs_result);
        } catch (const std::exception& e) {
            LOG_ERROR("后台容器结构分析异常: " + std::string(e.what()));
        } catch (...) {
            LOG_ERROR("后台容器结构分析发生未知异常");
        }

        if (!ok || !self) return;
        // 过期结果丢弃: 期间换了文件或关了播放器, 这次扫描的结果不能再覆盖新结果。
        // (以前靠 generation 比对, 现在统一由 TaskManager 的 IsCurrent 判定)
        if (token.IsCanceled() ||
            !self->task_manager_.IsCurrent(kSlotContainerStructure, id)) {
            LOG_INFO("容器结构分析结果已过期, 丢弃");
            return;
        }
        QMetaObject::invokeMethod(self, [self, result = std::move(cs_result)]() mutable {
            if (self) emit self->ContainerStructureReady(result);
        }, Qt::QueuedConnection);
        LOG_INFO("OpenInternal: 容器结构分析完成");
    });
    LOG_INFO("OpenInternal: 容器结构分析已派发到后台线程");
}

// --- 解码线程 ---

// --- 播放回调 (由 PlaybackSession 的解码线程调用) ---
//
// 切分后解码循环只做 demux / 解码 / pacing / 画面输出; "要不要发信号、要不要计数"
// 这类分析语义全部留在下面这些 hook 里, 与分析器、计数器放在一起。
// 调用线程与改造前一致(解码线程), 因此信号仍然由 Qt 自动排队到 UI 线程。

void MediaPlayer::InstallPlaybackHooks() {
    PlaybackSession::Hooks hooks;
    hooks.on_packet = [this](const PacketContext& ctx) { OnPlaybackPacket(ctx); };
    hooks.on_video_frame = [this](const VideoFrameContext& ctx) { OnPlaybackVideoFrame(ctx); };
    hooks.on_audio_frame = [this](const AudioFrameContext& ctx) { OnPlaybackAudioFrame(ctx); };
    hooks.on_seek_done = [this](double target_ms, model::SeekMode mode) {
        OnPlaybackSeekDone(target_ms, mode);
    };
    hooks.on_end_of_stream = [this]() { OnPlaybackEndOfStream(); };
    hooks.on_sync_sample = [this](double audio_ts, double video_ts, bool audio_anchor) {
        EmitSyncSample(audio_ts, video_ts, audio_anchor);
    };
    playback_session_.SetHooks(std::move(hooks));
}

void MediaPlayer::OnPlaybackPacket(const PacketContext& ctx) {
    const AVPacket* packet = ctx.packet;
    if (!packet) return;
    AVFormatContext* format_ctx = ctx.format_ctx;
    const double packet_ts_sec = ctx.timestamp_seconds;

    // 包分析
    if (analysis_session_.IsPacketAnalysisEnabled()) {
        model::PacketInfo packet_info;
        packet_info.index = packet_index_++;
        packet_info.stream_index = packet->stream_index;
        if (format_ctx && packet->stream_index >= 0 &&
            packet->stream_index < static_cast<int>(format_ctx->nb_streams) &&
            format_ctx->streams[packet->stream_index] &&
            format_ctx->streams[packet->stream_index]->codecpar) {
            packet_info.stream_type = format_ctx->streams[packet->stream_index]->codecpar->codec_type;
        }
        packet_info.pts = packet->pts;
        packet_info.dts = packet->dts;
        packet_info.duration = packet->duration;
        packet_info.size = packet->size;
        packet_info.flags = packet->flags;
        packet_info.pos = packet->pos;
        packet_info.timestamp_seconds = packet_ts_sec;
        emit PacketInfoReady(packet_info);
    }

    // 时间轴与同步诊断（demux 层 packet 时间）
    if (analysis_session_.IsTimelineAnalysisEnabled()) {
        AVStream* ts_stream = (format_ctx && packet->stream_index >= 0 &&
                               packet->stream_index < static_cast<int>(format_ctx->nb_streams))
                                  ? format_ctx->streams[packet->stream_index]
                                  : nullptr;
        if (ts_stream) {
            const double tb_ms = av_q2d(ts_stream->time_base) * 1000.0;
            model::PacketTiming timing;
            timing.index = timeline_packet_index_++;
            timing.stream_index = packet->stream_index;
            timing.media_type = static_cast<int>(ts_stream->codecpar->codec_type);
            timing.pts_ms = (packet->pts != AV_NOPTS_VALUE)
                                ? static_cast<double>(packet->pts) * tb_ms
                                : model::kNoTimestamp;
            timing.dts_ms = (packet->dts != AV_NOPTS_VALUE)
                                ? static_cast<double>(packet->dts) * tb_ms
                                : model::kNoTimestamp;
            timing.duration_ms = (packet->duration > 0)
                                     ? static_cast<double>(packet->duration) * tb_ms
                                     : model::kNoTimestamp;
            timing.pos = packet->pos;
            timing.size = packet->size;
            timing.flags = packet->flags;
            timing.key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            emit TimelinePacketReady(timing);
        }
    }

    // 时间戳检查
    if (!std::isfinite(packet_ts_sec)) {
        if (!missing_packet_ts_reported_[packet->stream_index]) {
            missing_packet_ts_reported_[packet->stream_index] = true;
            EmitAnalysisEvent(tr("警告"), tr("缺失时间戳"), packet->stream_index,
                              static_cast<qint64>(ctx.raw_timestamp), packet_ts_sec,
                              tr("数据包缺少有效时间戳"),
                              tr("该流存在 PTS/DTS 缺失的数据包，后续同步与定位可能不准确。"));
        }
    } else {
        auto it = last_packet_ts_by_stream_.find(packet->stream_index);
        if (it != last_packet_ts_by_stream_.end()) {
            const double delta_sec = packet_ts_sec - it->second;
            if (delta_sec < -0.001) {
                EmitAnalysisEvent(tr("错误"), tr("时间戳回退"), packet->stream_index,
                                  static_cast<qint64>(ctx.raw_timestamp), packet_ts_sec,
                                  tr("检测到非单调递增的包时间戳"),
                                  tr("当前时间戳早于上一包，可能存在封装异常、乱序或损坏。"));
            } else if (delta_sec > 2.0) {
                EmitAnalysisEvent(tr("警告"), tr("时间戳跳变"), packet->stream_index,
                                  static_cast<qint64>(ctx.raw_timestamp), packet_ts_sec,
                                  tr("检测到较大的包时间戳跳变"),
                                  tr("相邻包时间差超过 2 秒，可能出现断流、裁切或时间基异常。"));
            }
        }
        last_packet_ts_by_stream_[packet->stream_index] = packet_ts_sec;
    }

    if (analysis_session_.IsAnalysisEnabled()) {
        analysis_session_.stream_analyzer().AnalyzePacket(packet, format_ctx);
    }
}

void MediaPlayer::OnPlaybackVideoFrame(const VideoFrameContext& ctx) {
    if (!ctx.frame || !ctx.decoder) return;
    model::FrameData& frame_data = *ctx.frame;
    const AVPacket* packet = ctx.packet;
    AVFormatContext* format_ctx = ctx.format_ctx;
    const double frame_ts = ctx.timestamp_seconds;

    if (frame_data.width <= 0 || frame_data.height <= 0 || !frame_data.data[0]) {
        LOG_WARN("跳过无效帧数据");
        EmitAnalysisEvent(tr("错误"), tr("无效视频帧"), ctx.video_stream_index,
                          static_cast<qint64>(frame_data.pts), frame_data.timestamp,
                          tr("检测到无效视频帧"), tr("视频帧的宽高或数据指针无效，已被跳过。"));
        return;
    }

    // 帧类型分析 (精确帧定位的追赶阶段跳过, 见 ctx.catching_up)
    if (!ctx.catching_up && analysis_session_.IsFrameTypeAnalysisEnabled()) {
        double ts = frame_data.timestamp;
        const int emitted_index = video_frame_index_;
        const bool is_key_frame = (packet && (packet->flags & AV_PKT_FLAG_KEY) != 0);
        if ((ts == 0.0 || std::isnan(ts) || std::isinf(ts)) && format_ctx && ctx.video_stream_index >= 0) {
            AVStream* vs = format_ctx->streams[ctx.video_stream_index];
            if (vs && vs->time_base.den != 0) {
                int64_t pts = frame_data.pts;
                if (pts == AV_NOPTS_VALUE && packet && packet->pts != AV_NOPTS_VALUE) pts = packet->pts;
                if (pts != AV_NOPTS_VALUE) ts = pts * av_q2d(vs->time_base);
            }
        }
        emit VideoFrameInfoReady(video_frame_index_++,
                                 static_cast<int>(ctx.decoder->GetLastPictureType()),
                                 is_key_frame, static_cast<qint64>(frame_data.pts), ts);
        // 时间轴与同步诊断（decode 层 frame 时间：best_effort / repeat_pict）
        if (analysis_session_.IsTimelineAnalysisEnabled() && format_ctx && ctx.video_stream_index >= 0) {
            AVStream* vs = format_ctx->streams[ctx.video_stream_index];
            const double tb_ms = (vs && vs->time_base.den != 0) ? av_q2d(vs->time_base) * 1000.0 : 0.0;
            const AVFrame* raw = ctx.raw_frame;
            model::FrameTimingInfo timing;
            timing.index = static_cast<int>(video_frame_index_ - 1);
            timing.stream_index = ctx.video_stream_index;
            timing.pts_ms = (frame_data.pts != AV_NOPTS_VALUE)
                                ? static_cast<double>(frame_data.pts) * tb_ms
                                : model::kNoTimestamp;
            if (raw) {
                timing.best_effort_ms =
                    (raw->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? static_cast<double>(raw->best_effort_timestamp) * tb_ms
                        : model::kNoTimestamp;
                timing.duration_ms = (raw->duration > 0)
                                         ? static_cast<double>(raw->duration) * tb_ms
                                         : model::kNoTimestamp;
                timing.pict_type = static_cast<int>(raw->pict_type);
                timing.repeat_pict = raw->repeat_pict;
                timing.key_frame = (raw->flags & AV_FRAME_FLAG_KEY) != 0;
            } else {
                timing.key_frame = is_key_frame;
            }
            timing.display_ms = ts * 1000.0;
            emit FrameTimingReady(timing);
        }
        if (is_key_frame) {
            EmitTimelineEvent(QStringLiteral("视频关键帧"), ts,
                              QStringLiteral("关键帧 #%1").arg(emitted_index));
        }
    }

    // 宏块分析 (运动矢量 / 块统计)
    if (!ctx.catching_up && analysis_session_.IsMacroblockAnalysisEnabled()) {
        const AVFrame* raw_frame = ctx.raw_frame;
        if (raw_frame) {
            try {
                auto mb_analysis = macroblock_analyzer_.AnalyzeFrame(
                    raw_frame, macroblock_frame_index_++, static_cast<qint64>(frame_data.pts),
                    frame_data.timestamp, static_cast<int>(ctx.decoder->GetLastPictureType()),
                    static_cast<int>(ctx.decoder->GetCodecId()));
                emit MacroblockInfoReady(mb_analysis);
            } catch (const std::exception& e) {
                LOG_ERROR("宏块分析失败: " + std::string(e.what()));
            }
        }
    }

    // 场景切换检测: 逐帧计算灰度直方图, 与上一帧比较巴氏距离。
    if (!ctx.catching_up && analysis_session_.IsSceneChangeAnalysisEnabled()) {
        try {
            auto gray_hist = ComputeGrayHistogram(frame_data);
            if (!gray_hist.empty()) {
                auto sc = scene_change_analyzer_.Feed(scene_change_frame_index_++,
                                                      frame_data.timestamp, gray_hist);
                if (sc) emit SceneChangeReady(*sc);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("场景切换检测失败: " + std::string(e.what()));
        }
    }

    // 画面质量 / 视觉缺陷: 按采样档位抽取解码帧（降采样后投递分析器）
    if (!ctx.catching_up && analysis_session_.IsVisualDefectAnalysisEnabled()) {
        try {
            double vd_ts = frame_data.timestamp;
            if ((vd_ts == 0.0 || std::isnan(vd_ts) || std::isinf(vd_ts)) &&
                format_ctx && ctx.video_stream_index >= 0) {
                AVStream* vs = format_ctx->streams[ctx.video_stream_index];
                if (vs && vs->time_base.den != 0) {
                    int64_t pts = frame_data.pts;
                    if (pts == AV_NOPTS_VALUE && packet && packet->pts != AV_NOPTS_VALUE) {
                        pts = packet->pts;
                    }
                    if (pts != AV_NOPTS_VALUE) vd_ts = pts * av_q2d(vs->time_base);
                }
            }
            // 没有音频流的素材无从判断静音，按"不静音"处理（静止画面照报冻结）
            const bool vd_silent = (playback_session_.audio_stream_index() >= 0) &&
                                   (last_audio_level_ < 0.005);
            FeedVisualDefectFrame(ctx.raw_frame, vd_ts, vd_silent);
        } catch (const std::exception& e) {
            LOG_ERROR("画面质量分析失败: " + std::string(e.what()));
        }
    }

    if (!ctx.catching_up && analysis_session_.IsAnalysisEnabled()) {
        analysis_session_.stream_analyzer().AnalyzeVideoFrame(ctx.decoder->GetLastPictureType());
        analysis_frame_counter_++;
        if (analysis_frame_counter_ % 10 == 0) {
            auto stats = analysis_session_.stream_analyzer().GetStats();
            emit StreamStatsReady(stats);
        }
    }
}

void MediaPlayer::OnPlaybackAudioFrame(const AudioFrameContext& ctx) {
    const double ts = ctx.timestamp_seconds;

    if (ctx.frame_pts == AV_NOPTS_VALUE &&
        !missing_audio_pts_reported_[ctx.audio_stream_index]) {
        missing_audio_pts_reported_[ctx.audio_stream_index] = true;
        EmitAnalysisEvent(tr("警告"), tr("音频帧缺失PTS"), ctx.audio_stream_index, ctx.frame_pts, ts,
                          tr("检测到缺少 PTS 的音频帧"),
                          tr("音频帧将退回使用包时间戳或当前位置，可能影响精确同步分析。"));
    }

    if (analysis_session_.IsAudioFrameAnalysisEnabled() && ctx.decoder) {
        emit AudioFrameInfoReady(audio_frame_index_++, ctx.frame_pts, ts,
                                 ctx.decoder->GetLastFrameSampleCount(),
                                 ctx.sample_rate, ctx.channels, ctx.byte_count);
    }
    ++audio_timeline_sample_counter_;
    if (audio_timeline_sample_counter_ % 100 == 0) {
        EmitTimelineEvent(QStringLiteral("音频采样"), ts,
                          QStringLiteral("音频帧 #%1").arg(audio_frame_index_ - 1));
    }
    if (analysis_session_.IsAnalysisEnabled()) {
        analysis_session_.stream_analyzer().AnalyzeAudioFrame();
        if (playback_session_.video_stream_index() < 0) {
            analysis_frame_counter_++;
            if (analysis_frame_counter_ % 10 == 0) {
                auto stats = analysis_session_.stream_analyzer().GetStats();
                emit StreamStatsReady(stats);
            }
        }
    }

    if (!ctx.enqueued) return;   // 字节数太小, 既没推送也没算电平

    // 冻结帧判定要用: 画面静止 + 音频还在响 才是真的卡住了
    last_audio_level_ = ctx.level;
    emit AudioLevelReady(ctx.level, ts);

    // 音频可视化 (委托给 AudioVisualizer)
    auto vis_result = audio_visualizer_.Process(ctx.samples, ctx.sample_count, ctx.sample_rate,
                                                std::max(1, ctx.channels));
    EmitAudioVisualization(vis_result, ctx.sample_rate, std::max(1, ctx.channels), ts, ctx.level);
}

void MediaPlayer::OnPlaybackSeekDone(double target_ms, model::SeekMode mode) {
    Q_UNUSED(target_ms);
    Q_UNUSED(mode);
    // 统计语义是"自上次打开/定位起的当前片段": 定位成功后必须清零,
    // 否则向后定位会重复累计帧/字节, GOP / 关键帧计数也会失真。
    if (analysis_session_.IsAnalysisEnabled()) {
        analysis_session_.stream_analyzer().Reset();
    }
}

void MediaPlayer::OnPlaybackEndOfStream() {
    // 播到结尾: 把还开着的缺陷段闭合，否则最后一条缺陷要等下次播放才出现
    if (analysis_session_.IsVisualDefectAnalysisEnabled()) {
        FlushVisualDefectSegments(visual_defect_last_sample_ts_ > 0.0
                                      ? visual_defect_last_sample_ts_
                                      : playback_session_.current_position_ms() / 1000.0);
    }
}


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
