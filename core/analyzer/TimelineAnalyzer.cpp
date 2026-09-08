#include "core/analyzer/TimelineAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace videoeye {
namespace analyzer {
namespace {

std::string FormatMs(double value, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

std::string FormatTimestamp(double ms) {
    return model::FormatTimestamp(ms / 1000.0);
}

}  // namespace

void TimelineAnalyzer::Reset() {
    result_ = model::TimelineAnalysisResult{};
    result_.frame_interval_ms.name = "frame_interval";
    result_.frame_interval_ms.unit = "ms";
    result_.av_offset_ms.name = "av_offset";
    result_.av_offset_ms.unit = "ms";
    result_.packet_delta_ms.name = "packet_delta";
    result_.packet_delta_ms.unit = "ms";
    streams_.clear();
    issue_index_.clear();
    issue_total_count_.clear();
    packet_index_ = 0;
    frame_index_ = 0;
    finished_ = false;
}

void TimelineAnalyzer::SetOptions(const Options& options) {
    options_ = options;
}

void TimelineAnalyzer::AddIssue(model::TimelineIssueType type, model::IssueSeverity severity,
                                int stream_index, double timestamp_ms, double metric_value_ms,
                                double threshold_ms, const std::string& detail,
                                const std::string& suggestion) {
    const int type_key = static_cast<int>(type);

    // 同类型 + 同流合并计数（限流：超过上限只累计数量，不再新增条目）
    auto& index_by_stream = issue_index_[type_key];
    auto it = index_by_stream.find(stream_index);
    if (it != index_by_stream.end()) {
        auto& issue = result_.issues[it->second];
        ++issue.occurrence_count;
        issue.range = model::TimeRange::At(timestamp_ms / 1000.0);
        if (std::abs(metric_value_ms) > std::abs(issue.metric_value)) {
            issue.metric_value = metric_value_ms;
            issue.detail = detail;
        }
        return;
    }
    if (issue_total_count_[type_key] >= options_.max_issues_per_type) {
        ++issue_total_count_[type_key];
        return;
    }
    ++issue_total_count_[type_key];

    model::DiagnosticIssue issue;
    issue.rule_id = model::RuleIdOf(type);
    issue.title = model::ToString(type);
    issue.category = model::IssueCategory::Timing;
    issue.severity = severity;
    issue.stream_index = stream_index;
    issue.range = model::TimeRange::At(timestamp_ms / 1000.0);
    issue.metric_value = metric_value_ms;
    issue.threshold = threshold_ms;
    issue.detail = detail;
    issue.suggestion = suggestion;
    issue.occurrence_count = 1;
    index_by_stream[stream_index] = result_.issues.size();
    result_.issues.push_back(std::move(issue));
}

void TimelineAnalyzer::OnPacket(const model::PacketTiming& packet) {
    if (finished_) Reset();

    StreamState& state = streams_[packet.stream_index];
    if (packet.media_type >= 0) state.media_type = packet.media_type;

    const bool has_pts = model::IsValidTimestamp(packet.pts_ms);
    const bool has_dts = model::IsValidTimestamp(packet.dts_ms);
    const double ts = has_pts ? packet.pts_ms : packet.dts_ms;

    ++state.packet_count;
    result_.has_data = true;

    if (packet.key_frame) {
        ++state.keyframe_count;
        if (state.media_type == 0) ++result_.key_frame_count;
    }

    if (model::IsValidTimestamp(ts)) {
        if (!model::IsValidTimestamp(state.first_ts_ms) || ts < state.first_ts_ms) {
            state.first_ts_ms = ts;
        }
        if (!model::IsValidTimestamp(state.last_ts_ms) || ts > state.last_ts_ms) {
            state.last_ts_ms = ts;
        }
    }

    if (has_pts) {
        if (model::IsValidTimestamp(state.last_pts_ms)) {
            const double delta = packet.pts_ms - state.last_pts_ms;
            if (delta < -0.001) {
                AddIssue(model::TimelineIssueType::PtsNonMonotonic, model::IssueSeverity::Error,
                         packet.stream_index, packet.pts_ms, -delta, 0.0,
                         "PTS 回退 " + FormatMs(-delta) + " ms（上一包 " +
                             FormatMs(state.last_pts_ms) + " ms，本包 " +
                             FormatMs(packet.pts_ms) + " ms）。",
                         "重新封装或转码；拼接场景请重算时间戳（-fflags +genpts）。");
            } else if (delta <= options_.duplicate_tolerance_ms) {
                AddIssue(model::TimelineIssueType::DuplicatedTimestamp, model::IssueSeverity::Info,
                         packet.stream_index, packet.pts_ms, delta,
                         options_.duplicate_tolerance_ms,
                         "相邻包 PTS 间隔仅 " + FormatMs(delta, 3) + " ms，疑似重复/零时长包。",
                         "检查是否存在重复帧或 duration 未写入。");
            }

            // 包时间戳 delta 曲线
            result_.packet_delta_ms.Add(packet.pts_ms, delta);

            // 非视频流用 duration 推算断流（视频流交由帧级统计判断，避免重复报）
            if (state.media_type != 0 &&
                model::IsValidTimestamp(state.expected_next_pts_ms)) {
                const double gap = packet.pts_ms - state.expected_next_pts_ms;
                const double tolerance = std::max(50.0, 3.0 * std::max(0.0, state.last_duration_ms));
                if (gap > tolerance) {
                    AddIssue(model::TimelineIssueType::FrameIntervalSpike,
                             model::IssueSeverity::Warning, packet.stream_index,
                             packet.pts_ms, gap, tolerance,
                             "包时间戳出现 " + FormatMs(gap) + " ms 空洞（按 duration 推算应连续）。",
                             "核查是否丢包/断流，或封装时未写入 duration。");
                }
            }
        }
        state.last_pts_ms = packet.pts_ms;

        state.expected_next_pts_ms = model::IsValidTimestamp(packet.duration_ms)
                                         ? packet.pts_ms + packet.duration_ms
                                         : model::kNoTimestamp;
        if (model::IsValidTimestamp(packet.duration_ms)) {
            state.last_duration_ms = packet.duration_ms;
        }
    }

    if (has_dts) {
        if (model::IsValidTimestamp(state.last_dts_ms) &&
            packet.dts_ms < state.last_dts_ms - 0.001) {
            AddIssue(model::TimelineIssueType::DtsNonMonotonic, model::IssueSeverity::Error,
                     packet.stream_index, packet.dts_ms, state.last_dts_ms - packet.dts_ms, 0.0,
                     "DTS 回退 " + FormatMs(state.last_dts_ms - packet.dts_ms) + " ms，解码顺序异常。",
                     "重新封装；含 B 帧时 DTS 必须单调递增。");
        }
        state.last_dts_ms = packet.dts_ms;
    }

    if (has_pts && has_dts && packet.dts_ms > packet.pts_ms + options_.dts_after_pts_tolerance_ms) {
        AddIssue(model::TimelineIssueType::DtsAfterPts, model::IssueSeverity::Warning,
                 packet.stream_index, packet.pts_ms, packet.dts_ms - packet.pts_ms,
                 options_.dts_after_pts_tolerance_ms,
                 "DTS 晚于 PTS " + FormatMs(packet.dts_ms - packet.pts_ms) + " ms，B 帧重排异常。",
                 "检查 muxer 是否正确写入 DTS；含 B 帧时 DTS 必须不晚于 PTS。");
    }

    ++packet_index_;
}

void TimelineAnalyzer::OnFrame(const model::FrameTimingInfo& frame) {
    if (finished_) Reset();

    double display_ms = frame.display_ms;
    if (!model::IsValidTimestamp(display_ms)) display_ms = frame.best_effort_ms;
    if (!model::IsValidTimestamp(display_ms)) display_ms = frame.pts_ms;
    if (!model::IsValidTimestamp(display_ms)) return;

    if (frame.stream_index >= 0 && result_.video_stream_index < 0) {
        result_.video_stream_index = frame.stream_index;
    }
    if (frame.key_frame) ++result_.key_frame_count;
    ++result_.frame_count;
    result_.has_data = true;

    // 仅记录显示时间；间隔与统计在 Finish() 中统一计算（保证均值/中位数口径一致）
    result_.frame_interval_ms.Add(display_ms, 0.0);
    ++frame_index_;
}

void TimelineAnalyzer::OnSyncSample(double audio_ms, double video_ms) {
    if (!model::IsValidTimestamp(audio_ms) || !model::IsValidTimestamp(video_ms)) return;
    const double offset = audio_ms - video_ms;
    result_.av_offset_ms.Add(video_ms, offset);
    if (std::abs(offset) > std::abs(result_.max_av_offset_ms)) {
        result_.max_av_offset_ms = offset;
    }
    result_.has_data = true;
}

model::TimelineAnalysisResult TimelineAnalyzer::Snapshot() const {
    TimelineAnalyzer copy = *this;
    copy.Finish();
    return copy.result();
}

void TimelineAnalyzer::Finish() {
    if (finished_) return;

    // ---- 帧间隔：用已记录的显示时间序列反推 ----
    FinalizeFrameStats();
    FinalizeStreamSummary();

    finished_ = true;
}

void TimelineAnalyzer::FinalizeFrameStats() {
    auto& series = result_.frame_interval_ms;
    if (series.samples.size() < 2) {
        if (series.samples.size() == 1) series.samples.clear();
        return;
    }

    // 由显示时间序列生成间隔序列
    std::vector<double> intervals;
    std::vector<double> timestamps;
    intervals.reserve(series.samples.size() - 1);
    timestamps.reserve(series.samples.size() - 1);
    for (size_t i = 1; i < series.samples.size(); ++i) {
        intervals.push_back(series.samples[i].timestamp_seconds -
                            series.samples[i - 1].timestamp_seconds);
        timestamps.push_back(series.samples[i].timestamp_seconds);
    }

    series.Clear();
    for (size_t i = 0; i < intervals.size(); ++i) {
        series.Add(timestamps[i], intervals[i]);
    }

    // 统计
    result_.avg_frame_interval_ms = series.Mean();
    result_.frame_interval_stddev_ms = series.StdDev();
    result_.max_frame_interval_ms = series.Max();
    result_.min_frame_interval_ms = series.Min();

    // 直方图（下标 = 毫秒）
    result_.frame_interval_histogram_ms.assign(options_.histogram_max_ms + 1, 0);
    for (const double interval : intervals) {
        int bucket = static_cast<int>(std::lround(interval));
        bucket = std::clamp(bucket, 0, options_.histogram_max_ms);
        ++result_.frame_interval_histogram_ms[bucket];
    }

    // VFR / CFR 判定：标准差 或 相对离散度 超阈值
    const double p5 = series.Percentile(0.05);
    const double p95 = series.Percentile(0.95);
    const double median = series.Percentile(0.5);
    const double spread = (std::abs(median) > 1e-6) ? ((p95 - p5) / std::abs(median)) : 0.0;
    result_.vfr = (result_.frame_interval_stddev_ms > options_.vfr_stddev_ms) ||
                  (spread > options_.vfr_relative_spread);

    // 派生问题：突刺与重复帧
    const double spike_threshold =
        std::max(options_.frame_interval_spike_min_ms,
                 result_.avg_frame_interval_ms * options_.frame_interval_spike_ratio);
    for (size_t i = 0; i < intervals.size(); ++i) {
        if (intervals[i] > spike_threshold) {
            AddIssue(model::TimelineIssueType::FrameIntervalSpike, model::IssueSeverity::Warning,
                     result_.video_stream_index, timestamps[i], intervals[i], spike_threshold,
                     "帧间隔突刺 " + FormatMs(intervals[i]) + " ms（平均 " +
                         FormatMs(result_.avg_frame_interval_ms) + " ms），可能存在丢帧或卡顿。",
                     "检查源帧率是否稳定；转码时可加 -vsync cfr 规整。");
        } else if (intervals[i] <= options_.duplicate_tolerance_ms) {
            AddIssue(model::TimelineIssueType::DuplicatedTimestamp, model::IssueSeverity::Info,
                     result_.video_stream_index, timestamps[i], intervals[i],
                     options_.duplicate_tolerance_ms,
                     "相邻帧间隔 " + FormatMs(intervals[i], 3) + " ms，疑似重复帧。",
                     "检查 repeat_pict / 3:2 pulldown 或封装重复包。");
        }
    }
}

void TimelineAnalyzer::FinalizeStreamSummary() {
    int video_stream = -1;
    int audio_stream = -1;
    for (const auto& [index, state] : streams_) {
        if (state.media_type == 0 && video_stream < 0) video_stream = index;
        if (state.media_type == 1 && audio_stream < 0) audio_stream = index;
    }
    if (result_.video_stream_index < 0) result_.video_stream_index = video_stream;
    result_.audio_stream_index = audio_stream;

    const StreamState* video = (video_stream >= 0) ? &streams_.at(video_stream) : nullptr;
    const StreamState* audio = (audio_stream >= 0) ? &streams_.at(audio_stream) : nullptr;

    if (video && model::IsValidTimestamp(video->first_ts_ms)) {
        result_.video_start_ms = video->first_ts_ms;
        result_.video_duration_ms = model::IsValidTimestamp(video->last_ts_ms)
                                        ? (video->last_ts_ms - video->first_ts_ms)
                                        : 0.0;
    }
    if (audio && model::IsValidTimestamp(audio->first_ts_ms)) {
        result_.audio_start_ms = audio->first_ts_ms;
        result_.audio_duration_ms = model::IsValidTimestamp(audio->last_ts_ms)
                                        ? (audio->last_ts_ms - audio->first_ts_ms)
                                        : 0.0;
        if (result_.audio_duration_ms > 0.0 && audio->last_duration_ms > 0.0) {
            result_.audio_duration_ms += audio->last_duration_ms;
        }
    }
    if (video && model::IsValidTimestamp(video->last_ts_ms) && video->last_duration_ms > 0.0) {
        result_.video_duration_ms += video->last_duration_ms;
    }

    // 音视频首帧偏移
    if (video && audio) {
        result_.av_start_offset_ms = result_.audio_start_ms - result_.video_start_ms;
        const double offset = std::abs(result_.av_start_offset_ms);
        if (offset >= options_.av_start_offset_error_ms) {
            AddIssue(model::TimelineIssueType::AudioVideoStartOffset, model::IssueSeverity::Error,
                     audio_stream, std::min(result_.audio_start_ms, result_.video_start_ms),
                     result_.av_start_offset_ms, options_.av_start_offset_error_ms,
                     "音频比视频晚 " + FormatMs(result_.av_start_offset_ms) + " ms 起步（阈值 " +
                         FormatMs(options_.av_start_offset_error_ms) + " ms）。",
                     "用 -itsoffset 修正，或在 mux 时对齐首帧时间戳。");
        } else if (offset >= options_.av_start_offset_warn_ms) {
            AddIssue(model::TimelineIssueType::AudioVideoStartOffset, model::IssueSeverity::Warning,
                     audio_stream, std::min(result_.audio_start_ms, result_.video_start_ms),
                     result_.av_start_offset_ms, options_.av_start_offset_warn_ms,
                     "音视频首帧偏移 " + FormatMs(result_.av_start_offset_ms) + " ms（告警阈值 " +
                         FormatMs(options_.av_start_offset_warn_ms) + " ms）。",
                     "轻微偏移可用 -itsoffset 微调。");
        }

        // 总时长不一致
        const double duration_diff = result_.audio_duration_ms - result_.video_duration_ms;
        if (std::abs(duration_diff) >= options_.av_duration_mismatch_warn_ms) {
            AddIssue(model::TimelineIssueType::AudioVideoDurationMismatch,
                     model::IssueSeverity::Warning, audio_stream,
                     std::max(result_.video_start_ms, result_.audio_start_ms), duration_diff,
                     options_.av_duration_mismatch_warn_ms,
                     "音视时长不一致：视频 " + FormatMs(result_.video_duration_ms) + " ms，音频 " +
                         FormatMs(result_.audio_duration_ms) + " ms（差 " +
                         FormatMs(duration_diff) + " ms）。",
                     "检查是否存在截断流；必要时用 -shortest 对齐。");
        }
    }

    // 关键帧索引缺失（无法 seek）
    if (video && video->packet_count > 0 && video->keyframe_count == 0) {
        AddIssue(model::TimelineIssueType::MissingKeyframeIndex, model::IssueSeverity::Error,
                 video_stream, result_.video_start_ms, 0.0, 1.0,
                 "视频流共 " + std::to_string(video->packet_count) + " 个包，未标记任何关键帧，"
                 "seek 与索引功能不可用。",
                 "重新编码并设置 GOP（-g）或补齐关键帧索引（stss）。");
    }
}

} // namespace analyzer
} // namespace videoeye
