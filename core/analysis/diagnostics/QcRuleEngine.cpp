#include "core/analysis/diagnostics/QcRuleEngine.h"

#include <algorithm>
#include <string>
#include <utility>

#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

namespace videoeye {

QcRuleEngine::QcRuleEngine() : rules_(model::DefaultQcRules()) {}

QcRuleEngine::QcRuleEngine(std::vector<model::QcRule> rules) : rules_(std::move(rules)) {}

void QcRuleEngine::SetRules(std::vector<model::QcRule> rules) {
    rules_ = std::move(rules);
}

model::QcReport QcRuleEngine::Evaluate(const model::AnalysisResult& result) const {
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
    // 终态且非取消/失败 → 报告视为"完整/可结算"；命中包数上限的抽样属于部分结果
    report.completed = (result.scan_status == model::AnalysisStatus::Complete ||
                        result.scan_status == model::AnalysisStatus::Sampled);
    report.partial = (result.scan_status == model::AnalysisStatus::Sampled);
    report.rules = rules_;
    report.color_hdr = result.color_hdr;
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
    // 结论不能只靠分数：Critical 只扣 30 分，有 Critical 的素材分数照样能落"警告"档，
    // 结论与严重度脱钩（形同虚设）。这里把严重度也带进去再判。
    report.verdict = model::ComputeQcVerdict(
        report.score,
        report.CountBySeverity(model::IssueSeverity::Critical) > 0,
        report.CountBySeverity(model::IssueSeverity::Error) > 0);
    if (report.partial) {
        // 抽样扫描（命中 max_packets 上限）不完整，禁止给出"通过"结论：仅供参考
        report.verdict = "抽样完成·仅供参考";
    }
    return report;
}

// CheckRule 只做一件事：按 rule.id 前缀把判定派发到对应的分类翻译单元。
//
// 原单文件里那条约 740 行的 if 链，已按其中的注释分区逐字搬进各个 Check*Rules
// （见 QcRuleEngineCheck*.cpp）——判定顺序、阈值逻辑、文案、字段映射一字未改。
// 这里的前缀路由是规则「成员资格」的唯一判定点：分类函数假定 rule.id 属于本类。
// 注意分支顺序不可调换（video.color. 必须排在 video. 之前）。
std::vector<model::DiagnosticIssue> QcRuleEngine::CheckRule(const model::QcRule& rule,
                                                            const model::AnalysisResult& result) const {
    const diagnostics::detail::IssueFactory make_issue{rule};
    const std::string& id = rule.id;
    if (id.rfind("container.mp4.", 0) == 0) return diagnostics::detail::CheckMp4ContainerRules(rule, result, make_issue);
    if (id.rfind("container.hls.", 0) == 0 || id.rfind("container.dash.", 0) == 0 ||
        id.rfind("container.streaming.", 0) == 0) return diagnostics::detail::CheckStreamingContainerRules(rule, result, make_issue);
    if (id.rfind("container.", 0) == 0) return diagnostics::detail::CheckContainerRules(rule, result, make_issue);
    if (id.rfind("video.color.", 0) == 0) return diagnostics::detail::CheckColorHdrRules(rule, result, make_issue);
    if (id.rfind("video.", 0) == 0) return diagnostics::detail::CheckVideoRules(rule, result, make_issue);
    if (id.rfind("timing.", 0) == 0) return diagnostics::detail::CheckVideoRules(rule, result, make_issue);  // timing.dts_missing 与视频同 TU
    if (id.rfind("audio.", 0) == 0) return diagnostics::detail::CheckAudioRules(rule, result, make_issue);
    if (id.rfind("subtitle.", 0) == 0) return diagnostics::detail::CheckSubtitleRules(rule, result, make_issue);
    if (id.rfind("timecode.", 0) == 0 || id.rfind("chapter.", 0) == 0) return diagnostics::detail::CheckTimecodeChapterRules(rule, result, make_issue);
    if (id.rfind("scte35.", 0) == 0) return diagnostics::detail::CheckScte35Rules(rule, result, make_issue);
    return {};
}

} // namespace videoeye