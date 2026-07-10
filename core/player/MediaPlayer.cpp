#include "MediaPlayer.h"
#include "utils/Logger.h"
#include <QFileInfo>
#include <QDebug>
#include <QPointer>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <limits>
#include <exception>
#include <QDir>
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

MediaPlayer::MediaPlayer(QObject* parent)
    : QObject(parent) {
    avformat_network_init();
    qRegisterMetaType<model::Mp4BoxAnalysisResult>("model::Mp4BoxAnalysisResult");
    qRegisterMetaType<model::ContainerStructureResult>("model::ContainerStructureResult");
    qRegisterMetaType<model::MacroblockFrameAnalysis>("model::MacroblockFrameAnalysis");
    qRegisterMetaType<model::MacroblockFrameAnalysis>("videoeye::model::MacroblockFrameAnalysis");
}

MediaPlayer::~MediaPlayer() {
    CancelVideoFrameExport();
    Stop();
    Cleanup();
    avformat_network_deinit();
}

// --- 分析事件发射 ---

void MediaPlayer::EmitAnalysisEvent(const QString& severity, const QString& type, int stream_index,
                                    qint64 pts, double timestamp_seconds,
                                    const QString& summary, const QString& detail) {
    if (!event_analysis_enabled_) return;
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
    if (!sync_analysis_enabled_) return;
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
    if (!timeline_analysis_enabled_) return;
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
        emit Error(QString("Unsupported PCM format: %1").arg(demuxer_name));
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
    std::lock_guard<std::mutex> lock(mutex_);
    Cleanup();

    current_url_ = url;
    should_stop_ = false;
    video_frame_index_ = 0;
    macroblock_frame_index_ = 0;
    scene_change_frame_index_ = 0;
    scene_change_analyzer_.Reset();
    audio_frame_index_ = 0;
    packet_index_ = 0;
    analysis_event_index_ = 0;
    sync_sample_index_ = 0;
    timeline_event_index_ = 0;
    audio_visualization_index_ = 0;
    audio_timeline_sample_counter_ = 0;
    last_packet_ts_by_stream_.clear();
    missing_packet_ts_reported_.clear();
    missing_audio_pts_reported_.clear();
    last_video_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    last_audio_sync_ts_ = std::numeric_limits<double>::quiet_NaN();
    emit VideoFrameListReset();
    if (audio_frame_analysis_enabled_) emit AudioFrameListReset();
    if (packet_analysis_enabled_) emit PacketListReset();
    if (event_analysis_enabled_) emit AnalysisEventListReset();
    if (sync_analysis_enabled_) emit SyncSampleListReset();
    if (timeline_analysis_enabled_) emit TimelineEventListReset();

    // 版本检查
    const unsigned header_avcodec_major = LIBAVCODEC_VERSION_MAJOR;
    const unsigned runtime_avcodec_major = static_cast<unsigned>(avcodec_version() >> 16);
    if (header_avcodec_major != runtime_avcodec_major) {
        emit Error(QString("FFmpeg libavcodec 版本不匹配：编译期头文件=%1，运行期库=%2。")
                       .arg(header_avcodec_major).arg(runtime_avcodec_major));
        return false;
    }

    // 打开输入
    std::string url_str = url.toStdString();
    AVDictionary* open_options = input_options;
    int ret = avformat_open_input(&format_ctx_, url_str.c_str(), input_format, open_options ? &open_options : nullptr);
    if (open_options) av_dict_free(&open_options);
    if (ret < 0) {
        emit Error(QString("Failed to open: %1").arg(url));
        return false;
    }
    LOG_INFO("OpenInternal: avformat_open_input OK");

    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        emit Error("Failed to find stream info");
        Cleanup();
        return false;
    }
    LOG_INFO("OpenInternal: avformat_find_stream_info OK, streams=" + std::to_string(format_ctx_->nb_streams));

    if (!format_ctx_) {
        emit Error("Format context is null");
        return false;
    }

    // 查找流
    const AVCodec* best_video_codec = nullptr;
    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &best_video_codec, 0);
    const AVCodec* best_audio_codec = nullptr;
    audio_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, &best_audio_codec, 0);

    if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
        emit Error("No video or audio stream found");
        Cleanup();
        return false;
    }

    // 处理封面图
    bool has_video = (video_stream_index_ >= 0);
    if (video_stream_index_ >= 0 && format_ctx_->streams[video_stream_index_] &&
        (format_ctx_->streams[video_stream_index_]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        AVStream* vs = format_ctx_->streams[video_stream_index_];
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
        video_stream_index_ = -1;
    }

    emit MediaModeChanged(has_video);

    // 初始化视频解码器
    if (video_stream_index_ >= 0) {
        video_decoder_ = std::make_unique<VideoDecoder>();
        AVStream* video_stream = format_ctx_->streams[video_stream_index_];
        if (!video_stream || !video_stream->codecpar) {
            emit Error("Video codec parameters not available");
            Cleanup();
            return false;
        }

        bool hw_initialized = false;
        // 尝试硬件解码
        // 宏块分析依赖软件解码导出的运动矢量 side data, 硬件解码器不产出该数据,
        // 因此启用宏块分析时直接走软件解码路径。
        if (hw_decoding_enabled_ && !macroblock_analysis_enabled_) {
#ifdef HAVE_VULKAN
            // 优先尝试 Vulkan (如果 VulkanContext 可用)
            if (!vulkan_ctx_) {
                vulkan_ctx_ = std::make_unique<VulkanContext>();
                if (!vulkan_ctx_->Initialize()) {
                    vulkan_ctx_.reset();  // Vulkan 不可用, 清理
                }
            }
            if (vulkan_ctx_ && vulkan_ctx_->IsValid()) {
                if (video_decoder_->InitializeWithVulkanDevice(
                        video_stream->codecpar,
                        vulkan_ctx_->GetAvHwDeviceContext())) {
                    hw_initialized = true;
                    LOG_INFO("Vulkan HW decoding initialized");
                } else {
                    vulkan_ctx_.reset();  // Vulkan 解码初始化失败
                }
            }
#endif

            // 回退到其他 HW 方法 (VAAPI/CUDA/QSV...)
            if (!hw_initialized) {
                auto hw_types = VideoDecoder::GetAvailableHwDeviceTypes();
                for (auto hw_type : hw_types) {
                    if (hw_type == AV_HWDEVICE_TYPE_VULKAN) continue;  // 已尝试
                    if (video_decoder_->InitializeWithHw(video_stream->codecpar, hw_type)) {
                        hw_initialized = true;
                        LOG_INFO("HW decoding initialized: " +
                                 std::string(av_hwdevice_get_type_name(hw_type)));
                        break;
                    }
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
                emit Error("Video codec not found");
                Cleanup();
                return false;
            }
            AVCodecContext* video_codec_ctx = avcodec_alloc_context3(video_codec);
            if (!video_codec_ctx) {
                emit Error("Failed to create video codec context");
                Cleanup();
                return false;
            }
            ret = avcodec_parameters_to_context(video_codec_ctx, video_stream->codecpar);
            if (ret < 0) {
                avcodec_free_context(&video_codec_ctx);
                emit Error("Failed to copy codec parameters");
                Cleanup();
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
                emit Error("Failed to open video decoder");
                Cleanup();
                return false;
            }
            if (!video_decoder_->InitializeFromContext(video_codec_ctx)) {
                avcodec_free_context(&video_codec_ctx);
                emit Error("Failed to initialize video decoder");
                Cleanup();
                return false;
            }
        }
        LOG_INFO("Video decoder initialized: " + std::to_string(video_decoder_->GetWidth()) + "x" +
                 std::to_string(video_decoder_->GetHeight()) +
                 (video_decoder_->IsHardwareDecoding() ? " (HW)" : " (SW)"));
    }

    // 初始化音频解码器
    if (audio_stream_index_ >= 0) {
        audio_decoder_ = std::make_unique<AudioDecoder>();
        AVStream* audio_stream = format_ctx_->streams[audio_stream_index_];
        if (!audio_stream->codecpar) {
            emit Error("Audio codec parameters not available");
            Cleanup();
            return false;
        }
        if (!audio_decoder_->Initialize(audio_stream->codecpar)) {
            emit Error("Failed to initialize audio decoder");
            Cleanup();
            return false;
        }
        LOG_INFO("Audio decoder initialized");

        // 初始化音频输出设备 (SDL2): 把解码后的 PCM 送进声卡
        audio_output_ = std::make_unique<AudioOutput>();
        if (!audio_output_->Open(audio_decoder_->GetSampleRate(), audio_decoder_->GetChannels())) {
            LOG_WARN("音频输出设备打开失败, 将继续无声音播放");
            audio_output_.reset();
        } else {
            audio_output_->SetVolume(volume_ / 100.0);
            LOG_INFO("AudioOutput: 音频输出设备已就绪");
        }
    }

    // 提取流信息 (委托给 StreamInfoExtractor)
    auto extract_result = stream_info_extractor_.Extract(format_ctx_, video_stream_index_, audio_stream_index_, url);
    stream_info_ = extract_result.info;
    duration_ms_ = extract_result.duration_ms;
    current_position_ms_.store(0);

    state_ = model::PlayerState::Idle;
    emit StateChanged(state_);

    // 触发容器结构分析 (统一调度) — 放到后台线程执行。
    // 容器结构分析是纯展示信息 (面板里看 Box 树 / 样本表), 不应阻塞打开 / 播放。
    // 之前同步跑在 UI 线程, 大文件的 sample 表展开会让界面冻结数秒~数十秒。
    // 改为后台线程, 完成后通过信号 (跨线程自动排队) 投递结果。
    if (container_structure_enabled_) {
        LOG_INFO("OpenInternal: 容器结构分析已派发到后台线程");
        QString url_copy = url;
        QPointer<MediaPlayer> self = this;
        std::thread([self, url_copy]() {
            model::ContainerStructureResult cs_result;
            if (self && self->container_analyzer_.Analyze(url_copy, cs_result)) {
                emit self->ContainerStructureReady(cs_result);
            }
            LOG_INFO("OpenInternal: 后台容器结构分析完成");
        }).detach();
    }

    LOG_INFO("OpenInternal success: " + url.toStdString());
    return true;
}

void MediaPlayer::Play() {
    LOG_INFO("Play ENTER: state=" + std::to_string(static_cast<int>(state_.load())));
    if (!format_ctx_) { emit Error("No media opened"); return; }
    if (state_ == model::PlayerState::Playing) return;
    if (state_ == model::PlayerState::Paused) {
        should_stop_ = false;
        state_ = model::PlayerState::Playing;
        emit StateChanged(state_);
        if (audio_output_) audio_output_->Play();
        cv_.notify_one();
        return;
    }
    if (decode_thread_.joinable()) decode_thread_.join();
    should_stop_ = false;
    state_ = model::PlayerState::Playing;
    emit StateChanged(state_);
    if (audio_output_) audio_output_->Play();
    decode_thread_ = std::thread(&MediaPlayer::DecodeThread, this);
    LOG_INFO("Play: decode thread started");
}

void MediaPlayer::Pause() {
    LOG_INFO("Pause");
    if (state_ == model::PlayerState::Playing) {
        state_ = model::PlayerState::Paused;
        emit StateChanged(state_);
        if (audio_output_) audio_output_->Pause();
        cv_.notify_one();
    }
}

void MediaPlayer::Stop() {
    LOG_INFO("Stop");
    should_stop_ = true;
    state_ = model::PlayerState::Stopped;
    cv_.notify_one();
    if (audio_output_) audio_output_->Stop(); // 先停设备, 唤醒可能在 Enqueue 中阻塞的解码线程
    if (decode_thread_.joinable() && decode_thread_.get_id() != std::this_thread::get_id()) {
        decode_thread_.join();
    }
    audio_output_.reset();
    current_position_ms_.store(0);
    emit StateChanged(state_);
}

void MediaPlayer::Seek(int position_ms) {
    LOG_INFO("Seek: target_ms=" + std::to_string(position_ms));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!format_ctx_) return;
    if (audio_output_) audio_output_->Clear(); // 丢弃已缓冲的旧音频, 避免 seek 后播放过期声音
    int64_t timestamp = position_ms * 1000LL;
    int ret = av_seek_frame(format_ctx_, -1, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) { emit Error("Seek failed"); return; }
    current_position_ms_.store(position_ms);
    emit PositionChanged(current_position_ms_.load(), duration_ms_);
}

void MediaPlayer::SetVolume(int volume) {
    volume_ = std::max(0, std::min(100, volume));
    if (audio_output_) audio_output_->SetVolume(volume_ / 100.0);
}

void MediaPlayer::EnableAnalysis(bool enable) {
    analysis_enabled_ = enable;
    if (enable) {
        stream_analyzer_.Start();
        LOG_INFO("已启用视频分析");
    } else {
        histogram_enabled_ = false;
        stream_analyzer_.Stop();
        LOG_INFO("已禁用视频分析");
    }
}

void MediaPlayer::SetFrameTypeAnalysisEnabled(bool enable) { frame_type_analysis_enabled_ = enable; }

void MediaPlayer::SetMacroblockAnalysisEnabled(bool enable) {
    bool was_enabled = macroblock_analysis_enabled_;
    macroblock_analysis_enabled_ = enable;
    if (enable && !was_enabled) {
        LOG_INFO("宏块分析已启用");
        // 运动矢量仅在软件解码下由 FFmpeg 导出; 硬件解码 (Vulkan/D3D11/CUDA) 不产出
        // AV_FRAME_DATA_MOTION_VECTORS side data。若当前正在使用硬件解码, 则重新以
        // 软件解码打开当前文件并恢复播放位置, 否则宏块分析面板将始终为空。
        if (video_decoder_ && video_decoder_->IsHardwareDecoding() && !current_url_.isEmpty()) {
            LOG_WARN("当前为硬件解码, 无法导出运动矢量。自动切换为软件解码以启用宏块分析...");
            int resume_ms = current_position_ms_.load();
            bool was_playing = (state_.load() == model::PlayerState::Playing);
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

void MediaPlayer::SetHistogramEnabled(bool enable) { histogram_enabled_ = enable; }
analyzer::StreamStats MediaPlayer::GetCurrentStats() const { return stream_analyzer_.GetStats(); }

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

    frame_exporter_ = std::make_unique<VideoFrameExporter>();
    // 转发信号
    connect(frame_exporter_.get(), &VideoFrameExporter::ExportStarted, this, &MediaPlayer::VideoFrameExportStarted);
    connect(frame_exporter_.get(), &VideoFrameExporter::ExportProgress, this, &MediaPlayer::VideoFrameExportProgress);
    connect(frame_exporter_.get(), &VideoFrameExporter::ExportFinished, this, &MediaPlayer::VideoFrameExportFinished);
    connect(frame_exporter_.get(), &VideoFrameExporter::ExportCanceled, this, &MediaPlayer::VideoFrameExportCanceled);
    connect(frame_exporter_.get(), &VideoFrameExporter::ExportError, this, &MediaPlayer::VideoFrameExportError);
    frame_exporter_->Export(current_url_, output_dir, format.toLower(), jpg_quality, std::max(1, frame_interval));
}

void MediaPlayer::CancelVideoFrameExport() {
    if (frame_exporter_) {
        frame_exporter_->Cancel();
        frame_exporter_.reset();
    }
}

// --- 解码线程 ---

void MediaPlayer::DecodeThread() {
    LOG_INFO("DecodeThread START");
    AVPacket* packet = av_packet_alloc();
    model::FrameData frame_data;
    SwsContext* sws_ctx = nullptr;
    int sws_src_w = 0, sws_src_h = 0, sws_src_fmt = AV_PIX_FMT_NONE;
    std::vector<uint8_t> audio_buffer(192000);
    int last_emitted_position_ms = -1;
    int64_t decoded_frames = 0;

    try {
    const int clock_stream_index = SelectPlaybackClockStreamIndex(audio_stream_index_, video_stream_index_);
    const bool enable_pacing = (clock_stream_index >= 0);
    const bool frame_paced_video = (clock_stream_index >= 0 && clock_stream_index == video_stream_index_);
    PlaybackClock playback_clock;
    playback_clock.Reset();

    auto emit_position_if_needed = [&](double ts_sec) {
        if (!std::isfinite(ts_sec)) return;
        current_position_ms_.store(static_cast<int>(ts_sec * 1000.0));
        const int pos = current_position_ms_.load();
        int diff = pos - last_emitted_position_ms;
        if (diff < 0) diff = -diff;
        if (last_emitted_position_ms < 0 || diff >= 100) {
            last_emitted_position_ms = pos;
            emit PositionChanged(pos, duration_ms_);
        }
    };

    while (!should_stop_) {
        if (state_ == model::PlayerState::Paused) {
            const auto pause_begin = SteadyClock::now();
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return state_ != model::PlayerState::Paused || should_stop_; });
            const auto pause_end = SteadyClock::now();
            if (enable_pacing) playback_clock.OnPaused(pause_end - pause_begin);
            continue;
        }

        int ret = av_read_frame(format_ctx_, packet);
        if (ret < 0) { LOG_INFO("DecodeThread: av_read_frame EOF -> PlaybackFinished"); emit PlaybackFinished(); break; }

        const int64_t pkt_ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
        const double packet_ts_sec = PacketTimestampSeconds(format_ctx_, packet);

        // 包分析
        if (packet_analysis_enabled_) {
            model::PacketInfo packet_info;
            packet_info.index = packet_index_++;
            packet_info.stream_index = packet->stream_index;
            if (format_ctx_ && packet->stream_index >= 0 &&
                packet->stream_index < static_cast<int>(format_ctx_->nb_streams) &&
                format_ctx_->streams[packet->stream_index] &&
                format_ctx_->streams[packet->stream_index]->codecpar) {
                packet_info.stream_type = format_ctx_->streams[packet->stream_index]->codecpar->codec_type;
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

        // 时间戳检查
        if (!std::isfinite(packet_ts_sec)) {
            if (!missing_packet_ts_reported_[packet->stream_index]) {
                missing_packet_ts_reported_[packet->stream_index] = true;
                EmitAnalysisEvent(tr("警告"), tr("缺失时间戳"), packet->stream_index,
                                  static_cast<qint64>(pkt_ts), packet_ts_sec,
                                  tr("数据包缺少有效时间戳"),
                                  tr("该流存在 PTS/DTS 缺失的数据包，后续同步与定位可能不准确。"));
            }
        } else {
            auto it = last_packet_ts_by_stream_.find(packet->stream_index);
            if (it != last_packet_ts_by_stream_.end()) {
                const double delta_sec = packet_ts_sec - it->second;
                if (delta_sec < -0.001) {
                    EmitAnalysisEvent(tr("错误"), tr("时间戳回退"), packet->stream_index,
                                      static_cast<qint64>(pkt_ts), packet_ts_sec,
                                      tr("检测到非单调递增的包时间戳"),
                                      tr("当前时间戳早于上一包，可能存在封装异常、乱序或损坏。"));
                } else if (delta_sec > 2.0) {
                    EmitAnalysisEvent(tr("警告"), tr("时间戳跳变"), packet->stream_index,
                                      static_cast<qint64>(pkt_ts), packet_ts_sec,
                                      tr("检测到较大的包时间戳跳变"),
                                      tr("相邻包时间差超过 2 秒，可能出现断流、裁切或时间基异常。"));
                }
            }
            last_packet_ts_by_stream_[packet->stream_index] = packet_ts_sec;
        }

        if (analysis_enabled_) stream_analyzer_.AnalyzePacket(packet, format_ctx_);
        if (packet->stream_index == clock_stream_index && !frame_paced_video) {
            playback_clock.PaceTo(packet_ts_sec);
        }
        if (packet->stream_index == clock_stream_index && !frame_paced_video) {
            emit_position_if_needed(packet_ts_sec);
        }

        // 视频解码
        if (packet->stream_index == video_stream_index_ && video_decoder_) {
            if (video_decoder_->SendPacket(packet)) {
                while (!should_stop_ && video_decoder_->ReceiveFrame(frame_data)) {
                    double frame_ts = frame_data.timestamp;
                    if (format_ctx_ && video_stream_index_ >= 0) {
                        AVStream* vs = format_ctx_->streams[video_stream_index_];
                        if (vs && vs->time_base.den != 0 && frame_data.pts != AV_NOPTS_VALUE) {
                            frame_ts = frame_data.pts * av_q2d(vs->time_base);
                            frame_data.timestamp = frame_ts;
                        }
                    }
                    if (frame_paced_video) {
                        playback_clock.PaceTo(frame_ts);
                        emit_position_if_needed(frame_ts);
                    }
                    last_video_sync_ts_ = frame_ts;
                    if (std::isfinite(last_audio_sync_ts_)) {
                        EmitSyncSample(last_audio_sync_ts_, last_video_sync_ts_, false);
                    }
                    if (frame_data.width <= 0 || frame_data.height <= 0 || !frame_data.data[0]) {
                        LOG_WARN("跳过无效帧数据");
                        EmitAnalysisEvent(tr("错误"), tr("无效视频帧"), video_stream_index_,
                                          static_cast<qint64>(frame_data.pts), frame_data.timestamp,
                                          tr("检测到无效视频帧"), tr("视频帧的宽高或数据指针无效，已被跳过。"));
                        continue;
                    }

                    // Vulkan 渲染路径: 零拷贝模式下使用 VulkanRenderer
                    if (vulkan_rendering_enabled_ && vulkan_renderer_ &&
                        video_decoder_->IsCurrentFrameVulkan()) {
                        const AVFrame* raw = video_decoder_->GetLastRawFrame();
                        if (raw) {
                            vulkan_renderer_->PresentFrame(raw);
                            // Continue analysis below (frame type, histogram, etc.)
                        }
                    } else if (frame_data.format >= 0) {
                        if (sws_src_w != frame_data.width || sws_src_h != frame_data.height || sws_src_fmt != frame_data.format) {
                            sws_src_w = frame_data.width; sws_src_h = frame_data.height; sws_src_fmt = frame_data.format;
                        }
                        sws_ctx = sws_getCachedContext(sws_ctx, frame_data.width, frame_data.height,
                                                       static_cast<AVPixelFormat>(frame_data.format),
                                                       frame_data.width, frame_data.height, AV_PIX_FMT_BGRA,
                                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
                    }
                    if (sws_ctx) {
                        QImage qimage(frame_data.width, frame_data.height, QImage::Format_ARGB32);
                        if (!qimage.isNull()) {
                            uint8_t* dst_slices[4] = {qimage.bits(), nullptr, nullptr, nullptr};
                            int dst_linesize[4] = {static_cast<int>(qimage.bytesPerLine()), 0, 0, 0};
                            sws_scale(sws_ctx, frame_data.data, frame_data.linesize, 0, frame_data.height,
                                      dst_slices, dst_linesize);
                            emit FrameReady(qimage);
                            if (++decoded_frames % 120 == 0) {
                                LOG_INFO("DecodeThread heartbeat: frames=" + std::to_string(decoded_frames) +
                                         " pos_ms=" + std::to_string(current_position_ms_.load()));
                            }
                        }
                    }
                    if (frame_type_analysis_enabled_) {
                        double ts = frame_data.timestamp;
                        const int emitted_index = video_frame_index_;
                        const bool is_key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                        if ((ts == 0.0 || std::isnan(ts) || std::isinf(ts)) && format_ctx_ && video_stream_index_ >= 0) {
                            AVStream* vs = format_ctx_->streams[video_stream_index_];
                            if (vs && vs->time_base.den != 0) {
                                int64_t pts = frame_data.pts;
                                if (pts == AV_NOPTS_VALUE && packet->pts != AV_NOPTS_VALUE) pts = packet->pts;
                                if (pts != AV_NOPTS_VALUE) ts = pts * av_q2d(vs->time_base);
                            }
                        }
                        emit VideoFrameInfoReady(video_frame_index_++,
                                                 static_cast<int>(video_decoder_->GetLastPictureType()),
                                                 is_key_frame, static_cast<qint64>(frame_data.pts), ts);
                        if (is_key_frame) {
                            EmitTimelineEvent(QStringLiteral("视频关键帧"), ts,
                                              QStringLiteral("关键帧 #%1").arg(emitted_index));
                        }
                    }
                    // 宏块分析 (运动矢量 / 块统计)
                    if (macroblock_analysis_enabled_) {
                        const AVFrame* raw_frame = video_decoder_->GetLastRawFrame();
                        if (raw_frame) {
                            try {
                                auto mb_analysis = macroblock_analyzer_.AnalyzeFrame(
                                    raw_frame,
                                    macroblock_frame_index_++,
                                    static_cast<qint64>(frame_data.pts),
                                    frame_data.timestamp,
                                    static_cast<int>(video_decoder_->GetLastPictureType()));
                                emit MacroblockInfoReady(mb_analysis);
                            } catch (const std::exception& e) {
                                LOG_ERROR("宏块分析失败: " + std::string(e.what()));
                            }
                        }
                    }

                    // 场景切换检测: 逐帧计算灰度直方图, 与上一帧比较巴氏距离。
                    // 独立于"直方图"开关, 仅在本开关开启时计算, 避免无谓开销。
                    if (scene_change_analysis_enabled_) {
                        try {
                            auto hist = frame_analyzer_.ComputeHistogram(frame_data);
                            auto sc = scene_change_analyzer_.Feed(
                                scene_change_frame_index_++, frame_data.timestamp, hist.gray_channel);
                            if (sc) emit SceneChangeReady(*sc);
                        } catch (const std::exception& e) {
                            LOG_ERROR("场景切换检测失败: " + std::string(e.what()));
                        }
                    }
                    if (analysis_enabled_) {
                        stream_analyzer_.AnalyzeVideoFrame(video_decoder_->GetLastPictureType());
                        analysis_frame_counter_++;
                        if (analysis_frame_counter_ % 10 == 0) {
                            if (histogram_enabled_) {
                                try {
                                    auto hist = frame_analyzer_.ComputeHistogram(frame_data);
                                    emit HistogramReady(hist);
                                } catch (const std::exception& e) {
                                    LOG_ERROR("直方图分析失败: " + std::string(e.what()));
                                }
                            }
                            auto stats = stream_analyzer_.GetStats();
                            emit StreamStatsReady(stats);
                        }
                    }
                }
            }
        }

        // 音频解码
        if (packet->stream_index == audio_stream_index_ && audio_decoder_) {
            if (audio_decoder_->SendPacket(packet)) {
                int out_size = 0;
                while (audio_decoder_->ReceiveFrame(audio_buffer.data(),
                                                    static_cast<int>(audio_buffer.size()), out_size)) {
                    const qint64 frame_pts = static_cast<qint64>(audio_decoder_->GetLastFramePts());
                    double ts = current_position_ms_.load() / 1000.0;
                    if (frame_pts == AV_NOPTS_VALUE && !missing_audio_pts_reported_[audio_stream_index_]) {
                        missing_audio_pts_reported_[audio_stream_index_] = true;
                        EmitAnalysisEvent(tr("警告"), tr("音频帧缺失PTS"), audio_stream_index_,
                                          frame_pts, ts, tr("检测到缺少 PTS 的音频帧"),
                                          tr("音频帧将退回使用包时间戳或当前位置，可能影响精确同步分析。"));
                    }
                    if (format_ctx_ && audio_stream_index_ >= 0) {
                        AVStream* as = format_ctx_->streams[audio_stream_index_];
                        if (as && as->time_base.den != 0 && frame_pts != AV_NOPTS_VALUE) {
                            ts = frame_pts * av_q2d(as->time_base);
                        } else if (std::isfinite(packet_ts_sec)) {
                            ts = packet_ts_sec;
                        }
                    } else if (std::isfinite(packet_ts_sec)) {
                        ts = packet_ts_sec;
                    }
                    if (audio_frame_analysis_enabled_) {
                        emit AudioFrameInfoReady(audio_frame_index_++, frame_pts, ts,
                                                 audio_decoder_->GetLastFrameSampleCount(),
                                                 audio_decoder_->GetLastFrameSampleRate(),
                                                 audio_decoder_->GetLastFrameChannels(), out_size);
                    }
                    ++audio_timeline_sample_counter_;
                    if (audio_timeline_sample_counter_ % 100 == 0) {
                        EmitTimelineEvent(QStringLiteral("音频采样"), ts,
                                          QStringLiteral("音频帧 #%1").arg(audio_frame_index_ - 1));
                    }
                    last_audio_sync_ts_ = ts;
                    if (std::isfinite(last_video_sync_ts_)) {
                        EmitSyncSample(last_audio_sync_ts_, last_video_sync_ts_, true);
                    }
                    if (analysis_enabled_) {
                        stream_analyzer_.AnalyzeAudioFrame();
                        if (video_stream_index_ < 0) {
                            analysis_frame_counter_++;
                            if (analysis_frame_counter_ % 10 == 0) {
                                auto stats = stream_analyzer_.GetStats();
                                emit StreamStatsReady(stats);
                            }
                        }
                    }
                    if (out_size < static_cast<int>(sizeof(int16_t))) continue;

                    // 推送 PCM 到音频输出设备 (SDL2 回调播放)
                    if (audio_output_) {
                        audio_output_->Enqueue(audio_buffer.data(), out_size);
                    }

                    const int16_t* samples = reinterpret_cast<const int16_t*>(audio_buffer.data());
                    const int sample_count = out_size / static_cast<int>(sizeof(int16_t));

                    // RMS level
                    long double sumsq = 0.0;
                    for (int i = 0; i < sample_count; ++i) {
                        const long double s = static_cast<long double>(samples[i]);
                        sumsq += s * s;
                    }
                    double level = 0.0;
                    if (sample_count > 0) {
                        level = std::sqrt(static_cast<double>(sumsq / sample_count)) / 32768.0;
                        level = std::clamp(level, 0.0, 1.0);
                    }
                    emit AudioLevelReady(level, ts);

                    // 音频可视化 (委托给 AudioVisualizer)
                    auto vis_result = audio_visualizer_.Process(
                        samples, sample_count,
                        audio_decoder_->GetLastFrameSampleRate(),
                        std::max(1, audio_decoder_->GetLastFrameChannels()));
                    EmitAudioVisualization(vis_result,
                                           audio_decoder_->GetLastFrameSampleRate(),
                                           std::max(1, audio_decoder_->GetLastFrameChannels()),
                                           ts, level);
                }
            }
        }
        av_packet_unref(packet);
    }

    } catch (const std::exception& e) {
        LOG_ERROR("DecodeThread exception: " + std::string(e.what()));
        emit Error(QString("解码线程发生异常，已停止播放: %1").arg(e.what()));
        should_stop_ = true;
    } catch (...) {
        LOG_ERROR("DecodeThread unknown exception");
        emit Error(QString("解码线程发生未知异常，已停止播放"));
        should_stop_ = true;
    }

    LOG_INFO("DecodeThread EXIT");
    if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
    av_packet_free(&packet);
    if (state_ != model::PlayerState::Stopped) {
        state_ = model::PlayerState::Stopped;
        emit StateChanged(state_);
    }
}

bool MediaPlayer::IsHardwareDecoding() const {
    return video_decoder_ && video_decoder_->IsHardwareDecoding();
}

std::string MediaPlayer::GetHwDeviceName() const {
    if (!video_decoder_ || !video_decoder_->IsHardwareDecoding()) return "none";
    return av_hwdevice_get_type_name(video_decoder_->GetHwDeviceType());
}

void MediaPlayer::Cleanup() {
    if (format_ctx_) { avformat_close_input(&format_ctx_); format_ctx_ = nullptr; }
    video_decoder_.reset();
    audio_decoder_.reset();
    audio_output_.reset();
    vulkan_renderer_.reset();
    vulkan_ctx_.reset();
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
}

void MediaPlayer::SetVulkanRenderer(VulkanRenderer* renderer) {
    vulkan_renderer_.reset(renderer);
}

} // namespace player
} // namespace videoeye
