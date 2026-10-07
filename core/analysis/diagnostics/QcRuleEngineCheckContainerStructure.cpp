#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <string>
#include <utility>

namespace videoeye {
namespace diagnostics {
namespace detail {

// MP4/fMP4 容器一致性（container.mp4.*）。调用方（CheckRule 前缀路由）保证
// rule.id 属于本类，故此处不再做前缀判断。
std::vector<model::DiagnosticIssue> CheckMp4ContainerRules(const model::QcRule& rule,
                                                           const model::AnalysisResult& result,
                                                           const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

    // ---------- MP4/fMP4 容器一致性 ----------
    // 问题由 Mp4SampleTableAnalyzer 产出（含具体样本号 / 偏移 / 分片号），
    // 规则只做三件事: 决定要不要报、按什么级别报、阈值类再核一次阈值。
    if (!result.mp4_samples_analyzed || !result.mp4_samples.valid) return issues;

    for (const auto& finding : result.mp4_samples.issues) {
        if (finding.code != rule.id) continue;
        // 阈值类规则（elst 首帧偏移 / 音视频起点差）：分析器用的是自己的默认阈值，
        // 这里按用户在规则表里改过的阈值再核一次
        if (rule.op == model::QcRuleOp::MaxExceeded && rule.threshold > 0.0 &&
            finding.metric_value > 0.0 && finding.metric_value <= rule.threshold) {
            continue;
        }
        if (rule.op == model::QcRuleOp::MinBelow && rule.threshold != 0.0 &&
            finding.metric_value != 0.0 && finding.metric_value >= rule.threshold) {
            continue;
        }

        // 尽量把问题定位到时间点（样本级发现才有意义）
        double seconds = -1.0;
        if (finding.has_sample_index && finding.track_id >= 0) {
            const model::Mp4TrackSampleTable* track =
                result.mp4_samples.FindTrack(static_cast<uint32_t>(finding.track_id));
            const model::Mp4Sample* sample =
                track ? track->FindSample(finding.sample_index) : nullptr;
            if (sample && track->media_timescale > 0) {
                seconds = sample->CtsSeconds(track->media_timescale);
            }
        }

        model::DiagnosticIssue issue = finding.ToDiagnosticIssue(seconds);
        issue.rule_id = rule.id;
        issue.title = rule.name;
        issue.category = rule.category;
        issue.suggestion = rule.suggestion.empty() ? finding.suggestion : rule.suggestion;
        issue.threshold = rule.threshold;
        // 级别取「规则」与「分析器」中更严重的一侧：用户在规则表调到 Error 可以抬高，
        // 调到 Info 也不会把 Error 级发现（如分片序号回退）降没。
        if (static_cast<int>(finding.severity) > static_cast<int>(rule.severity)) {
            issue.severity = finding.severity;
        } else {
            issue.severity = rule.severity;
        }
        issues.push_back(std::move(issue));
    }
    return issues;
}

// HLS / DASH 流媒体包（container.hls.* / container.dash.* / container.streaming.*）。
std::vector<model::DiagnosticIssue> CheckStreamingContainerRules(const model::QcRule& rule,
                                                                 const model::AnalysisResult& result,
                                                                 const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

    // ---------- HLS / DASH 流媒体包 ----------
    // 与 container.mp4.* 完全同构：问题（含 variant 号 / 分片序号 / 时间点）由
    // HlsManifestAnalyzer / DashManifestAnalyzer / SegmentQcAnalyzer 产出，
    // 规则只决定"要不要报、以什么级别报、阈值类再核一次"。
    if (!result.streaming_analyzed || !result.streaming_package.valid) return issues;

    for (const auto& finding : result.streaming_package.issues) {
        if (finding.code != rule.id) continue;
        if (rule.op == model::QcRuleOp::MaxExceeded && rule.threshold > 0.0 &&
            finding.metric_value > 0.0 && finding.metric_value <= rule.threshold) {
            continue;
        }
        if (rule.op == model::QcRuleOp::MinBelow && rule.threshold != 0.0 &&
            finding.metric_value != 0.0 && finding.metric_value >= rule.threshold) {
            continue;
        }

        model::DiagnosticIssue issue = finding.ToDiagnosticIssue();
        issue.rule_id = rule.id;
        issue.title = rule.name;
        issue.category = rule.category;
        issue.suggestion = rule.suggestion.empty() ? finding.suggestion : rule.suggestion;
        issue.threshold = rule.threshold;
        // 级别取「规则」与「分析器」中更严重的一侧
        if (static_cast<int>(finding.severity) > static_cast<int>(rule.severity)) {
            issue.severity = finding.severity;
        } else {
            issue.severity = rule.severity;
        }
        issues.push_back(std::move(issue));
    }
    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye