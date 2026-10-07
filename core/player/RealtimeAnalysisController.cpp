#include "core/player/RealtimeAnalysisController.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "core/analysis/quality/QualityAnalyzer.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace player {

namespace {

// 计算单帧 256-bin 灰度直方图 (供场景切换检测使用)。
// 通过 sws_scale 将任意像素格式转为 GRAY8, 再逐字节计数; 不依赖 OpenCV。
// 返回空 vector 表示转换失败 (调用方应跳过该帧)。
std::vector<float> ComputeGrayHistogram(const model::FrameData& frame) {
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

}  // namespace

RealtimeAnalysisController::RealtimeAnalysisController(AnalysisSession& analysis,
                                                       PlaybackSession& playback,
                                                       QObject* parent)
    : QObject(parent), analysis_(analysis), playback_(playback) {
    InstallHooks();
}

// --- hook 装配 ---
//
// 解码循环只做 demux / 解码 / pacing / 画面输出; "要不要发信号、要不要计数"
// 这类分析语义全部留在下面这些 hook 里, 与分析器、计数器放在一起。
// 调用线程与改造前一致(解码线程), 因此信号仍然由 Qt 自动排队到 UI 线程。

void RealtimeAnalysisController::InstallHooks() {
    PlaybackSession::Hooks hooks;
    hooks.on_packet = [this](const PacketContext& ctx) { OnPacket(ctx); };
    hooks.on_video_frame = [this](const VideoFrameContext& ctx) { OnVideoFrame(ctx); };
    hooks.on_audio_frame = [this](const AudioFrameContext& ctx) { OnAudioFrame(ctx); };
    hooks.on_seek_done = [this](double target_ms, model::SeekMode mode) {
        OnSeekDone(target_ms, mode);
    };
    hooks.on_end_of_stream = [this]() { OnEndOfStream(); };
    hooks.on_sync_sample = [this](double audio_ts, double video_ts, bool audio_anchor) {
        EmitSyncSample(audio_ts, video_ts, audio_anchor);
    };
    playback_.SetHooks(std::move(hooks));
}

// --- 分析事件发射 ---
//
// 四个出口统一做两件事: 开关没开就不发（省掉填结构的开销），
// 以及推进各自的序号（界面上列表的行号必须连续且从 0 起）。

void RealtimeAnalysisController::EmitAnalysisEvent(const QString& severity, const QString& type,
                                                   int stream_index, qint64 pts,
                                                   double timestamp_seconds,
                                                   const QString& summary,
                                                   const QString& detail) {
    if (!analysis_.IsEventAnalysisEnabled()) return;
    model::AnalysisEvent event_info;
    event_info.index = analysis_event_index_++;
    event_info.severity = severity.toStdString();
    event_info.type = type.toStdString();
    event_info.stream_index = stream_index;
    event_info.pts = pts;
    event_info.timestamp_seconds = timestamp_seconds;
    event_info.summary = summary.toStdString();
    event_info.detail = detail.toStdString();
    emit AnalysisEventReady(event_info);
    EmitTimelineEvent(model::TimelineEventCategory::Event, timestamp_seconds, summary, detail);
}

void RealtimeAnalysisController::EmitSyncSample(double audio_ts, double video_ts,
                                                bool audio_anchor) {
    if (!analysis_.IsSyncAnalysisEnabled()) return;
    if (!std::isfinite(audio_ts) || !std::isfinite(video_ts)) return;
    model::SyncSample sample;
    sample.index = sync_sample_index_++;
    sample.audio_timestamp_seconds = audio_ts;
    sample.video_timestamp_seconds = video_ts;
    sample.diff_ms = (audio_ts - video_ts) * 1000.0;
    sample.audio_anchor = audio_anchor;
    emit SyncSampleReady(sample);
}

void RealtimeAnalysisController::EmitTimelineEvent(model::TimelineEventCategory category,
                                                   double timestamp_seconds,
                                                   const QString& label, const QString& detail) {
    if (!analysis_.IsTimelineAnalysisEnabled()) return;
    if (!std::isfinite(timestamp_seconds)) return;
    model::TimelineEvent event;
    event.index = timeline_event_index_++;
    event.category = category;
    event.timestamp_seconds = timestamp_seconds;
    event.label = label.toStdString();
    event.detail = detail.toStdString();
    emit TimelineEventReady(event);
}

void RealtimeAnalysisController::EmitAudioVisualization(const AudioVisualizationResult& vis,
                                                        int sample_rate, int channels,
                                                        double timestamp_seconds, double level) {
    model::AudioVisualizationFrame frame;
    frame.index = audio_visualization_index_++;
    frame.timestamp_seconds = timestamp_seconds;
    frame.level = level;
    frame.sample_rate = sample_rate;
    frame.channels = channels;
    frame.waveform_points = vis.waveform_points;
    frame.spectrum_bins = vis.spectrum_bins;
    frame.peak_dbfs = vis.peak_dbfs;
    frame.loudness_momentary_lufs = vis.loudness_momentary_lufs;
    frame.true_peak_dbtp = vis.true_peak_dbtp;
    emit AudioVisualizationReady(frame);
}

// --- 媒体生命周期 ---

void RealtimeAnalysisController::ResetForNewMedia() {
    video_frame_index_ = 0;
    macroblock_frame_index_ = 0;
    scene_change_frame_index_ = 0;
    scene_change_analyzer_.Reset();
    visual_defect_frame_index_ = 0;
    visual_defect_last_sample_ts_ = -1.0;
    last_audio_level_ = 0.0;
    visual_defect_analyzer_.Reset(GetVisualDefectOptions());
    // 视觉缺陷的工作线程在换媒体后要重新起: 分析器 Reset 会清掉队列与线程状态。
    if (analysis_.IsVisualDefectAnalysisEnabled() &&
        GetVisualDefectOptions().preset != videoeye::VisualSamplingPreset::OfflineFull) {
        visual_defect_analyzer_.StartWorker(8);
    }
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

    emit VideoFrameListReset();
    if (analysis_.IsAudioFrameAnalysisEnabled()) emit AudioFrameListReset();
    if (analysis_.IsPacketAnalysisEnabled()) emit PacketListReset();
    if (analysis_.IsEventAnalysisEnabled()) emit AnalysisEventListReset();
    if (analysis_.IsSyncAnalysisEnabled()) emit SyncSampleListReset();
    if (analysis_.IsTimelineAnalysisEnabled()) emit TimelineEventListReset();
    if (analysis_.IsVisualDefectAnalysisEnabled()) emit VisualDefectReset();
}

void RealtimeAnalysisController::FlushOnStop() {
    if (!analysis_.IsVisualDefectAnalysisEnabled()) return;
    FlushVisualDefectSegments(visual_defect_last_sample_ts_ > 0.0
                                  ? visual_defect_last_sample_ts_
                                  : 0.0);
}

// --- 画面质量 / 视觉缺陷 ---

void RealtimeAnalysisController::SetVisualDefectAnalysisEnabled(bool enable) {
    if (analysis_.IsVisualDefectAnalysisEnabled() == enable) return;
    analysis_.SetVisualDefectAnalysisEnabled(enable);
    if (enable) {
        const videoeye::VisualDefectOptions vd_opts = GetVisualDefectOptions();
        visual_defect_analyzer_.Reset(vd_opts);
        visual_defect_frame_index_ = 0;
        visual_defect_last_sample_ts_ = -1.0;
        // 实时档位走工作线程 + 有上限队列: 播放压力大时丢分析帧，绝不反压解码线程。
        // 离线全帧档位不需要队列（Feed 是同步的），开线程反而多一次拷贝。
        if (vd_opts.preset != videoeye::VisualSamplingPreset::OfflineFull) {
            visual_defect_analyzer_.StartWorker(8);
        }
        LOG_INFO("画面质量检测已启用");
        emit VisualDefectReset();
    } else {
        FlushVisualDefectSegments(visual_defect_last_sample_ts_ > 0.0 ? visual_defect_last_sample_ts_
                                                                      : 0.0);
        visual_defect_analyzer_.StopWorker();
        LOG_INFO("画面质量检测已禁用");
    }
}

videoeye::VisualDefectOptions RealtimeAnalysisController::GetVisualDefectOptions() const {
    return analysis_.GetVisualDefectOptions();
}

void RealtimeAnalysisController::SetVisualDefectOptions(
    const videoeye::VisualDefectOptions& options) {
    const bool was_enabled = analysis_.IsVisualDefectAnalysisEnabled();
    analysis_.SetVisualDefectOptions(options);
    if (!was_enabled) return;
    // 档位变了（尤其"离线全帧"开关）要重建线程与状态
    visual_defect_analyzer_.Reset(options);
    visual_defect_frame_index_ = 0;
    visual_defect_last_sample_ts_ = -1.0;
    if (options.preset != videoeye::VisualSamplingPreset::OfflineFull) {
        visual_defect_analyzer_.StartWorker(8);
    }
    emit VisualDefectReset();
}

void RealtimeAnalysisController::FlushVisualDefectSegments(double end_timestamp_seconds) {
    visual_defect_analyzer_.Flush(end_timestamp_seconds);
    DrainVisualDefectResults();
    EmitVisualDefectStats(true);
}

void RealtimeAnalysisController::DrainVisualDefectResults() {
    const auto metrics = visual_defect_analyzer_.TakePendingMetrics();
    for (const auto& m : metrics) {
        emit VisualDefectFrameReady(m);
    }
    const auto defects = visual_defect_analyzer_.TakePendingDefects();
    for (const auto& d : defects) {
        emit VisualDefectReady(d);
    }
}

void RealtimeAnalysisController::FeedVisualDefectFrame(const AVFrame* frame,
                                                       double timestamp_seconds,
                                                       bool audio_silent) {
    if (!frame) return;

    // 解码线程逐帧读取视觉缺陷选项: 取一次加锁副本, 避免反复加锁且与 UI 线程写入互斥。
    const videoeye::VisualDefectOptions vd_opts = GetVisualDefectOptions();

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
    if (!videoeye::QualityAnalyzer::BuildSample(frame,
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

    if (vd_opts.preset == videoeye::VisualSamplingPreset::OfflineFull) {
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

void RealtimeAnalysisController::EmitVisualDefectStats(bool force) {
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

// --- 播放回调 (由 PlaybackSession 的解码线程调用) ---

void RealtimeAnalysisController::OnPacket(const PacketContext& ctx) {
    const AVPacket* packet = ctx.packet;
    if (!packet) return;
    AVFormatContext* format_ctx = ctx.format_ctx;
    const double packet_ts_sec = ctx.timestamp_seconds;

    // 包分析
    if (analysis_.IsPacketAnalysisEnabled()) {
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
    if (analysis_.IsTimelineAnalysisEnabled()) {
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

    if (analysis_.IsAnalysisEnabled()) {
        analysis_.stream_analyzer().AnalyzePacket(packet, format_ctx);
    }
}

void RealtimeAnalysisController::OnVideoFrame(const VideoFrameContext& ctx) {
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
    if (!ctx.catching_up && analysis_.IsFrameTypeAnalysisEnabled()) {
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
        if (analysis_.IsTimelineAnalysisEnabled() && format_ctx && ctx.video_stream_index >= 0) {
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
            EmitTimelineEvent(model::TimelineEventCategory::VideoKeyframe, ts,
                              QStringLiteral("关键帧 #%1").arg(emitted_index));
        }
    }

    // 宏块分析 (运动矢量 / 块统计)
    if (!ctx.catching_up && analysis_.IsMacroblockAnalysisEnabled()) {
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
    if (!ctx.catching_up && analysis_.IsSceneChangeAnalysisEnabled()) {
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
    if (!ctx.catching_up && analysis_.IsVisualDefectAnalysisEnabled()) {
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
            const bool vd_silent = (playback_.audio_stream_index() >= 0) &&
                                   (last_audio_level_ < 0.005);
            FeedVisualDefectFrame(ctx.raw_frame, vd_ts, vd_silent);
        } catch (const std::exception& e) {
            LOG_ERROR("画面质量分析失败: " + std::string(e.what()));
        }
    }

    if (!ctx.catching_up && analysis_.IsAnalysisEnabled()) {
        analysis_.stream_analyzer().AnalyzeVideoFrame(ctx.decoder->GetLastPictureType());
        analysis_frame_counter_++;
        if (analysis_frame_counter_ % 10 == 0) {
            auto stats = analysis_.stream_analyzer().GetStats();
            emit StreamStatsReady(stats);
        }
    }
}

void RealtimeAnalysisController::OnAudioFrame(const AudioFrameContext& ctx) {
    const double ts = ctx.timestamp_seconds;

    if (ctx.frame_pts == AV_NOPTS_VALUE &&
        !missing_audio_pts_reported_[ctx.audio_stream_index]) {
        missing_audio_pts_reported_[ctx.audio_stream_index] = true;
        EmitAnalysisEvent(tr("警告"), tr("音频帧缺失PTS"), ctx.audio_stream_index, ctx.frame_pts, ts,
                          tr("检测到缺少 PTS 的音频帧"),
                          tr("音频帧将退回使用包时间戳或当前位置，可能影响精确同步分析。"));
    }

    if (analysis_.IsAudioFrameAnalysisEnabled() && ctx.decoder) {
        emit AudioFrameInfoReady(audio_frame_index_++, ctx.frame_pts, ts,
                                 ctx.decoder->GetLastFrameSampleCount(),
                                 ctx.sample_rate, ctx.channels, ctx.byte_count);
    }
    ++audio_timeline_sample_counter_;
    if (audio_timeline_sample_counter_ % 100 == 0) {
        EmitTimelineEvent(model::TimelineEventCategory::AudioSample, ts,
                          QStringLiteral("音频帧 #%1").arg(audio_frame_index_ - 1));
    }
    if (analysis_.IsAnalysisEnabled()) {
        analysis_.stream_analyzer().AnalyzeAudioFrame();
        if (playback_.video_stream_index() < 0) {
            analysis_frame_counter_++;
            if (analysis_frame_counter_ % 10 == 0) {
                auto stats = analysis_.stream_analyzer().GetStats();
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

void RealtimeAnalysisController::OnSeekDone(double target_ms, model::SeekMode mode) {
    Q_UNUSED(target_ms);
    Q_UNUSED(mode);
    // 统计语义是"自上次打开/定位起的当前片段": 定位成功后必须清零,
    // 否则向后定位会重复累计帧/字节, GOP / 关键帧计数也会失真。
    if (analysis_.IsAnalysisEnabled()) {
        analysis_.stream_analyzer().Reset();
    }
}

void RealtimeAnalysisController::OnEndOfStream() {
    // 播到结尾: 把还开着的缺陷段闭合，否则最后一条缺陷要等下次播放才出现
    if (analysis_.IsVisualDefectAnalysisEnabled()) {
        FlushVisualDefectSegments(visual_defect_last_sample_ts_ > 0.0
                                      ? visual_defect_last_sample_ts_
                                      : playback_.current_position_ms() / 1000.0);
    }
}

}  // namespace player
}  // namespace videoeye
