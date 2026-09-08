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
