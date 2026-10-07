#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

namespace videoeye {
namespace diagnostics {
namespace detail {

namespace {

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

}  // namespace

// 视频与时间戳规则（video.* / timing.dts_missing）。调用方（CheckRule 前缀路由）
// 保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue> CheckVideoRules(const model::QcRule& rule,
                                                    const model::AnalysisResult& result,
                                                    const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

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
        const model::StreamDigest* video = result.FirstVideoStream();
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
        using AnomalyType = model::BitrateAnomalyType;
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
                const model::BitrateAnomaly& a = *anomalies[i];
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
        const model::StreamDigest* video = result.FirstVideoStream();
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

    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye