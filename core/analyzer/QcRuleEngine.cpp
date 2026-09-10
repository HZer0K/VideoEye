#include "core/analyzer/QcRuleEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>

namespace videoeye {
namespace analyzer {
namespace {

std::string FormatValue(double value, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

double MeanOf(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double StdDevOf(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    const double mean = MeanOf(values);
    double acc = 0.0;
    for (const double v : values) {
        const double d = v - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(values.size() - 1));
}

bool Triggered(const model::QcRule& rule, double value) {
    switch (rule.op) {
        case model::QcRuleOp::MaxExceeded: return value > rule.threshold;
        case model::QcRuleOp::MinBelow:    return value < rule.threshold;
        case model::QcRuleOp::NonZero:     return std::abs(value) > 1e-9;
    }
    return false;
}

std::string ThresholdText(const model::QcRule& rule) {
    if (rule.op == model::QcRuleOp::NonZero) return "存在";
    const std::string op_text = (rule.op == model::QcRuleOp::MaxExceeded) ? ">" : "<";
    return op_text + " " + FormatValue(rule.threshold, 2) + rule.unit;
}

}  // namespace

QcRuleEngine::QcRuleEngine() : rules_(model::DefaultQcRules()) {}

QcRuleEngine::QcRuleEngine(std::vector<model::QcRule> rules) : rules_(std::move(rules)) {}

void QcRuleEngine::SetRules(std::vector<model::QcRule> rules) {
    rules_ = std::move(rules);
}

model::QcReport QcRuleEngine::Evaluate(const AnalysisResult& result) const {
    model::QcReport report;
    report.file_path = result.file_path;
    const size_t slash = result.file_path.find_last_of("/\\");
    report.file_name = (slash == std::string::npos) ? result.file_path
                                                    : result.file_path.substr(slash + 1);
    report.container_format = result.container_format;
    report.duration_seconds = result.duration_seconds;
    report.file_size_bytes = result.file_size_bytes;
    report.overall_bitrate_bps = result.overall_bitrate_bps;
    report.video_stream_count = result.VideoStreamCount();
    report.audio_stream_count = result.AudioStreamCount();
    report.completed = result.completed;
    report.rules = rules_;
    report.generated_at = model::CurrentTimestampString();

    for (const auto& rule : rules_) {
        if (!rule.enabled) continue;
        auto issues = CheckRule(rule, result);
        for (auto& issue : issues) {
            report.issues.push_back(std::move(issue));
        }
    }

    // 并入时间轴与同步诊断问题（category=Timing，统一计分与展示，避免与上面规则重复）
    for (const auto& tl_issue : result.timeline.issues) {
        report.issues.push_back(tl_issue);
    }

    // 严重度排序: Critical > Error > Warning > Info
    std::stable_sort(report.issues.begin(), report.issues.end(),
                     [](const model::DiagnosticIssue& a, const model::DiagnosticIssue& b) {
                         return static_cast<int>(a.severity) > static_cast<int>(b.severity);
                     });

    report.score = model::ComputeQcScore(report.issues);
    report.verdict = model::ComputeQcVerdict(report.score);
    return report;
}

std::vector<model::DiagnosticIssue> QcRuleEngine::CheckRule(const model::QcRule& rule,
                                                            const AnalysisResult& result) const {
    std::vector<model::DiagnosticIssue> issues;

    auto make_issue = [&rule](double value, const std::string& detail,
                              model::TimeRange range = model::TimeRange::Global(),
                              int stream_index = -1, int occurrences = 1) {
        model::DiagnosticIssue issue;
        issue.rule_id = rule.id;
        issue.title = rule.name;
        issue.detail = detail;
        issue.suggestion = rule.suggestion;
        issue.severity = rule.severity;
        issue.category = rule.category;
        issue.range = range;
        issue.stream_index = stream_index;
        issue.metric_value = value;
        issue.threshold = rule.threshold;
        issue.occurrence_count = occurrences;
        return issue;
    };

    // ---------- 容器 ----------
    if (rule.id == "container.duration_invalid") {
        if (Triggered(rule, result.duration_seconds)) {
            issues.push_back(make_issue(result.duration_seconds,
                "容器时长为 " + FormatValue(result.duration_seconds, 3) + " 秒（阈值 " +
                ThresholdText(rule) + "）。"));
        }
        return issues;
    }
    if (rule.id == "container.unseekable") {
        if (Triggered(rule, result.seekable ? 0.0 : 1.0)) {
            issues.push_back(make_issue(1.0, "底层 IO 不支持随机访问，只能线性播放。"));
        }
        return issues;
    }
    if (rule.id == "container.moov_after_mdat") {
        if (Triggered(rule, result.moov_after_mdat ? 1.0 : 0.0)) {
            issues.push_back(make_issue(1.0,
                "MP4 索引盒 moov 位于 mdat 之后，点播需下载完整个文件才能起播。"));
        }
        return issues;
    }
    if (rule.id == "container.missing_video") {
        if (result.VideoStreamCount() == 0) {
            issues.push_back(make_issue(1.0, "未检测到视频流。"));
        }
        return issues;
    }
    if (rule.id == "container.missing_audio") {
        if (result.AudioStreamCount() == 0) {
            issues.push_back(make_issue(1.0, "未检测到音频流。"));
        }
        return issues;
    }

    // ---------- 码率 ----------
    if (rule.id == "video.bitrate.peak_ratio") {
        const auto& series = result.video_bitrate_kbps.IsEmpty() ? result.total_bitrate_kbps
                                                                 : result.video_bitrate_kbps;
        if (series.Size() >= 2) {
            const double ratio = series.PeakToMeanRatio();
            if (Triggered(rule, ratio)) {
                issues.push_back(make_issue(ratio,
                    "峰值/均值码率比为 " + FormatValue(ratio) + "（峰值 " +
                    FormatValue(series.Max(), 0) + " kbps，均值 " +
                    FormatValue(series.Mean(), 0) + " kbps，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }
    if (rule.id == "video.bitrate.low_bpp") {
        const StreamDigest* video = result.FirstVideoStream();
        if (video && video->width > 0 && video->height > 0) {
            double fps = video->avg_fps;
            if (fps <= 0.0) fps = result.video_fps.Mean();
            if (fps > 0.0) {
                const double bpp = static_cast<double>(video->bitrate_bps) /
                                   (static_cast<double>(video->width) *
                                    static_cast<double>(video->height) * fps);
                if (Triggered(rule, bpp)) {
                    issues.push_back(make_issue(bpp,
                        "每像素每帧比特数为 " + FormatValue(bpp, 4) + "（码率 " +
                        FormatValue(video->bitrate_bps / 1000.0, 0) + " kbps，" +
                        std::to_string(video->width) + "x" + std::to_string(video->height) +
                        "@" + FormatValue(fps, 2) + "fps，阈值 " + ThresholdText(rule) + "）。",
                        model::TimeRange::Global(), video->index));
                }
            }
        }
        return issues;
    }

    // ---------- GOP ----------
    if (rule.id == "video.gop.max_seconds") {
        if (result.max_gop_interval_seconds > 0.0 &&
            Triggered(rule, result.max_gop_interval_seconds)) {
            issues.push_back(make_issue(result.max_gop_interval_seconds,
                "最长关键帧间隔 " + FormatValue(result.max_gop_interval_seconds, 2) + " 秒（" +
                std::to_string(result.max_gop_frames) + " 帧，阈值 " + ThresholdText(rule) + "）。"));
        }
        return issues;
    }
    if (rule.id == "video.gop.irregular") {
        if (result.gop_intervals_seconds.size() >= 3) {
            const double mean = MeanOf(result.gop_intervals_seconds);
            const double stddev = StdDevOf(result.gop_intervals_seconds);
            const double ratio = (mean > 0.0) ? (stddev / mean) : 0.0;
            if (Triggered(rule, ratio)) {
                issues.push_back(make_issue(ratio,
                    "关键帧间隔标准差/均值为 " + FormatValue(ratio) + "（均值 " +
                    FormatValue(mean, 2) + " 秒，标准差 " + FormatValue(stddev, 2) +
                    " 秒，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }

    // ---------- 码率与 GOP 深度分析（BitrateGopAnalyzer）----------
    // 这些规则逐条展开 anomalies，让诊断表能直接给出时间区间并支持"跳转"。
    if (rule.id == "video.gop.long_count" ||
        rule.id == "video.gop.scene_without_keyframe" ||
        rule.id == "video.gop.sparse_keyframes" ||
        rule.id == "video.bitrate.peak_overshoot" ||
        rule.id == "video.frame.oversized" ||
        rule.id == "video.frame.oversized_i") {
        using AnomalyType = BitrateAnomalyType;
        const AnomalyType type =
            (rule.id == "video.gop.long_count")               ? AnomalyType::LongGop
            : (rule.id == "video.gop.scene_without_keyframe") ? AnomalyType::SceneChangeWithoutKeyframe
            : (rule.id == "video.gop.sparse_keyframes")       ? AnomalyType::SparseKeyframes
            : (rule.id == "video.bitrate.peak_overshoot")     ? AnomalyType::PeakOvershoot
            : (rule.id == "video.frame.oversized")            ? AnomalyType::OversizedFrame
                                                              : AnomalyType::OversizedIFrame;

        auto anomalies = result.bitrate_gop.AnomaliesOf(type);
        if (!anomalies.empty()) {
            // 单条规则最多展开 50 条，避免长视频把诊断表刷爆
            constexpr size_t kMaxExpanded = 50;
            const size_t count = std::min(anomalies.size(), kMaxExpanded);
            for (size_t i = 0; i < count; ++i) {
                const BitrateAnomaly& a = *anomalies[i];
                issues.push_back(make_issue(a.value, a.detail,
                                            model::TimeRange::Between(a.start_seconds,
                                                                      a.end_seconds),
                                            -1, 1));
            }
            if (anomalies.size() > count) {
                issues.push_back(make_issue(static_cast<double>(anomalies.size()),
                    "另有 " + std::to_string(anomalies.size() - count) + " 条「" + rule.name +
                    "」未展开，完整列表见「码率与 GOP」页。"));
            }
        }
        return issues;
    }

    // ---------- 视频 ----------
    if (rule.id == "video.fps.unstable") {
        if (result.video_fps.Size() >= 3) {
            const double stddev = result.video_fps.StdDev();
            if (Triggered(rule, stddev)) {
                issues.push_back(make_issue(stddev,
                    "逐秒帧率标准差为 " + FormatValue(stddev, 2) + " fps（均值 " +
                    FormatValue(result.video_fps.Mean(), 2) + " fps，最低 " +
                    FormatValue(result.video_fps.Min(), 2) + " fps，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }
    if (rule.id == "video.resolution.odd") {
        const StreamDigest* video = result.FirstVideoStream();
        if (video && (video->width % 2 != 0 || video->height % 2 != 0)) {
            issues.push_back(make_issue(1.0,
                "分辨率为 " + std::to_string(video->width) + "x" + std::to_string(video->height) +
                "，存在奇数边。",
                model::TimeRange::Global(), video->index));
        }
        return issues;
    }

    // ---------- 时间戳（PTS 非单调 / 时间戳跳变已由 TimelineAnalyzer 统一产出，并入诊断报告） ----------
    if (rule.id == "timing.dts_missing") {
        if (result.total_packets > 0) {
            const double ratio = 100.0 * static_cast<double>(result.packets_missing_dts) /
                                 static_cast<double>(result.total_packets);
            if (Triggered(rule, ratio)) {
                issues.push_back(make_issue(ratio,
                    "缺失 DTS 的包占比 " + FormatValue(ratio, 2) + "%（" +
                    std::to_string(result.packets_missing_dts) + "/" +
                    std::to_string(result.total_packets) + "，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }
    // 时间戳跳变（gap）已由 TimelineAnalyzer 统一产出，不再重复规则。

    // ---------- 音频 ----------
    if (rule.id == "audio.sample_rate_low") {
        int min_rate = 0;
        int stream_index = -1;
        for (const auto& stream : result.streams) {
            if (!stream.IsAudio()) continue;
            if (min_rate == 0 || stream.sample_rate < min_rate) {
                min_rate = stream.sample_rate;
                stream_index = stream.index;
            }
        }
        if (min_rate > 0 && Triggered(rule, static_cast<double>(min_rate))) {
            issues.push_back(make_issue(static_cast<double>(min_rate),
                "最低音频采样率为 " + std::to_string(min_rate) + " Hz（阈值 " +
                ThresholdText(rule) + "）。",
                model::TimeRange::Global(), stream_index));
        }
        return issues;
    }
    // ---------- 音频 QC（core/analyzer/AudioQcAnalyzer，需解码音频）----------
    // 未跑过音频 QC（analyzed=false）时一律不判定，避免因为"没有数据"而误报。
    if (rule.id == "audio.loudness.target_high" || rule.id == "audio.loudness.target_low" ||
        rule.id == "audio.loudness.range" || rule.id == "audio.true_peak" ||
        rule.id == "audio.clipping" || rule.id == "audio.silence.longest" ||
        rule.id == "audio.silence.ratio" || rule.id == "audio.dc_offset" ||
        rule.id == "audio.phase_correlation" || rule.id == "audio.metadata.layout" ||
        rule.id == "audio.metadata.duration_mismatch") {
        const auto& qc = result.audio_qc;
        if (!qc.analyzed) return issues;

        const StreamDigest* audio = result.FirstAudioStream();
        const int audio_stream = (audio != nullptr) ? audio->index : -1;
        const bool has_loudness = qc.integrated_lufs > model::kSilenceLufs + 1.0;

        // 在响度曲线里定位指标最值出现的时刻（供"跳转到问题位置"）
        auto extreme_time = [&qc](auto getter, bool maximum) {
            double best = maximum ? -1e30 : 1e30;
            double ts = 0.0;
            for (const auto& p : qc.loudness_points) {
                const double v = getter(p);
                if ((maximum && v > best) || (!maximum && v < best)) {
                    best = v;
                    ts = p.timestamp_seconds;
                }
            }
            return ts;
        };

        if (rule.id == "audio.loudness.target_high") {
            if (has_loudness && Triggered(rule, qc.integrated_lufs)) {
                issues.push_back(make_issue(qc.integrated_lufs,
                    "积分响度为 " + FormatValue(qc.integrated_lufs, 2) + " LUFS，高于上限 " +
                    ThresholdText(rule) + "（短期最大 " + FormatValue(qc.short_term_max_lufs, 2) +
                    " LUFS，瞬时最大 " + FormatValue(qc.momentary_max_lufs, 2) + " LUFS）。",
                    model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.loudness.target_low") {
            if (has_loudness && Triggered(rule, qc.integrated_lufs)) {
                issues.push_back(make_issue(qc.integrated_lufs,
                    "积分响度为 " + FormatValue(qc.integrated_lufs, 2) + " LUFS，低于下限 " +
                    ThresholdText(rule) + "（短期最大 " + FormatValue(qc.short_term_max_lufs, 2) +
                    " LUFS）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.loudness.range") {
            if (qc.loudness_range_lu > 0.0 && Triggered(rule, qc.loudness_range_lu)) {
                issues.push_back(make_issue(qc.loudness_range_lu,
                    "响度动态范围 LRA 为 " + FormatValue(qc.loudness_range_lu, 1) + " LU（P10 " +
                    FormatValue(qc.loudness_range_low_lufs, 1) + " LUFS，P95 " +
                    FormatValue(qc.loudness_range_high_lufs, 1) + " LUFS，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.true_peak") {
            if (Triggered(rule, qc.true_peak_dbtp)) {
                const double ts = extreme_time(
                    [](const model::LoudnessPoint& p) { return p.true_peak_dbtp; }, true);
                issues.push_back(make_issue(qc.true_peak_dbtp,
                    "4× 过采样真峰值为 " + FormatValue(qc.true_peak_dbtp, 2) + " dBTP（采样峰值 " +
                    FormatValue(qc.sample_peak_dbfs, 2) + " dBFS，阈值 " + ThresholdText(rule) +
                    "）。", model::TimeRange::At(ts), audio_stream));
            }
        } else if (rule.id == "audio.clipping") {
            if (qc.clipping_sample_count > 0) {
                const double ts = qc.clipping_events.empty()
                                      ? 0.0
                                      : qc.clipping_events.front().start_seconds;
                issues.push_back(make_issue(static_cast<double>(qc.clipping_sample_count),
                    std::to_string(qc.clipping_sample_count) + " 个样本达到满刻度（" +
                    std::to_string(qc.clipping_event_count) + " 段，阈值 " +
                    ThresholdText(rule) + "），首次出现于 " + FormatValue(ts, 2) + " 秒。",
                    model::TimeRange::At(ts), audio_stream,
                    static_cast<int>(qc.clipping_event_count)));
            }
        } else if (rule.id == "audio.silence.longest") {
            double longest = 0.0;
            model::TimeRange longest_range = model::TimeRange::Global();
            for (const auto& range : qc.silence_ranges) {
                if (range.duration_seconds > longest) {
                    longest = range.duration_seconds;
                    longest_range = model::TimeRange::Between(range.start_seconds, range.end_seconds);
                }
            }
            if (longest > 0.0 && Triggered(rule, longest)) {
                issues.push_back(make_issue(longest,
                    "最长静音段 " + FormatValue(longest, 2) + " 秒（共 " +
                    std::to_string(qc.silence_ranges.size()) + " 段，阈值 " +
                    ThresholdText(rule) + "）。", longest_range, audio_stream,
                    static_cast<int>(qc.silence_ranges.size())));
            }
        } else if (rule.id == "audio.silence.ratio") {
            const double percent = qc.silence_ratio * 100.0;
            if (Triggered(rule, percent)) {
                issues.push_back(make_issue(percent,
                    "静音时长占比 " + FormatValue(percent, 1) + "%（" +
                    std::to_string(qc.silence_ranges.size()) + " 段，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.dc_offset") {
            if (Triggered(rule, qc.max_dc_offset)) {
                issues.push_back(make_issue(qc.max_dc_offset,
                    "最大直流偏移为 " + FormatValue(qc.max_dc_offset, 5) + "（" +
                    FormatValue(qc.max_dc_offset_dbfs, 1) + " dBFS，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.phase_correlation") {
            if (qc.correlation_available && Triggered(rule, qc.correlation_min)) {
                const double ts = extreme_time(
                    [](const model::LoudnessPoint& p) { return p.correlation; }, false);
                issues.push_back(make_issue(qc.correlation_min,
                    "最差声道相关性为 " + FormatValue(qc.correlation_min, 3) + "（反相块占比 " +
                    FormatValue(qc.out_of_phase_ratio * 100.0, 1) + "%，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::At(ts), audio_stream));
            }
        } else if (rule.id == "audio.metadata.layout") {
            if (!qc.metadata.layout_confirmed && qc.metadata.channels > 2) {
                issues.push_back(make_issue(static_cast<double>(qc.metadata.channels),
                    "声道布局为 " + qc.metadata.channel_layout +
                    "，但容器未明确标注声道位置，响度加权按通用顺序估算。",
                    model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.metadata.duration_mismatch") {
            const bool container_mismatch =
                qc.metadata.container_duration_seconds > 0.0 &&
                std::abs(qc.metadata.container_delta_seconds) > 0.5;
            const bool video_mismatch =
                qc.metadata.has_video && qc.metadata.video_duration_seconds > 0.0 &&
                std::abs(qc.metadata.video_delta_seconds) > 0.5;
            if (container_mismatch || video_mismatch) {
                std::string detail;
                for (const auto& text : qc.metadata.inconsistencies) {
                    if (text.find("时长") == std::string::npos) continue;
                    if (!detail.empty()) detail += "；";
                    detail += text;
                }
                if (detail.empty()) detail = "音频流与容器/视频时长不一致。";
                issues.push_back(make_issue(
                    std::max(std::abs(qc.metadata.container_delta_seconds),
                             std::abs(qc.metadata.video_delta_seconds)),
                    detail + "音频 " + FormatValue(qc.metadata.stream_duration_seconds, 2) +
                        " 秒。", model::TimeRange::Global(), audio_stream));
            }
        }
        return issues;
    }

    if (rule.id == "audio.channel_missing") {
        int missing = 0;
        int stream_index = -1;
        for (const auto& stream : result.streams) {
            if (stream.IsAudio() && stream.channels <= 0) {
                ++missing;
                if (stream_index < 0) stream_index = stream.index;
            }
        }
        if (Triggered(rule, static_cast<double>(missing))) {
            issues.push_back(make_issue(static_cast<double>(missing),
                std::to_string(missing) + " 条音频流未标注声道数。",
                model::TimeRange::Global(), stream_index, missing));
        }
        return issues;
    }

    return issues;
}

} // namespace analyzer
} // namespace videoeye
