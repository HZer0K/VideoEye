#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace videoeye {
namespace diagnostics {
namespace detail {

// 音频规则（audio.*）。调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue> CheckAudioRules(const model::QcRule& rule,
                                                    const model::AnalysisResult& result,
                                                    const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

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
    // ---------- 音频 QC（core/analysis/quality/AudioQcAnalyzer.h，需解码音频）----------
    // 未跑过音频 QC（analyzed=false）时一律不判定，避免因为"没有数据"而误报。
    if (rule.id == "audio.loudness.target_high" || rule.id == "audio.loudness.target_low" ||
        rule.id == "audio.loudness.range" || rule.id == "audio.true_peak" ||
        rule.id == "audio.clipping" || rule.id == "audio.silence.longest" ||
        rule.id == "audio.silence.ratio" || rule.id == "audio.dc_offset" ||
        rule.id == "audio.phase_correlation" || rule.id == "audio.metadata.layout" ||
        rule.id == "audio.metadata.duration_mismatch") {
        const auto& qc = result.audio_qc;
        if (!qc.analyzed) return issues;

        const model::StreamDigest* audio = result.FirstAudioStream();
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

    // audio.channel_missing 在原文件里排在 SCTE-35 段之后，但 id 属 audio.*，
    // 按前缀路由归入本 TU；逻辑与原处逐字一致。
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

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye