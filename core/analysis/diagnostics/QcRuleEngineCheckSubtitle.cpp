#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <string>
#include <utility>
#include <vector>

namespace videoeye {
namespace diagnostics {
namespace detail {

// 字幕规则（subtitle.*）。调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue> CheckSubtitleRules(const model::QcRule& rule,
                                                       const model::AnalysisResult& result,
                                                       const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

    // ---------- 字幕（core/analysis/diagnostics/SubtitleAnalyzer.h）----------
    // 具体阈值（最短/最长停留、阅读速度）由 SubtitleOptions 决定，规则只决定
    // "这类问题要不要进报告、按什么级别进"，所以全部用计数型判定。
    static const std::pair<const char*, model::SubtitleIssueType> kMap[] = {
        {"subtitle.cue_empty", model::SubtitleIssueType::EmptyText},
        {"subtitle.cue_overlap", model::SubtitleIssueType::Overlap},
        {"subtitle.cue_too_short", model::SubtitleIssueType::TooShort},
        {"subtitle.cue_too_long", model::SubtitleIssueType::TooLong},
        {"subtitle.cue_order", model::SubtitleIssueType::NonMonotonic},
        {"subtitle.cue_invalid_duration", model::SubtitleIssueType::InvalidDuration},
        {"subtitle.cue_too_fast", model::SubtitleIssueType::TooFast},
        {"subtitle.cue_out_of_range", model::SubtitleIssueType::OutOfRange},
        {"subtitle.missing_language", model::SubtitleIssueType::MissingLanguage},
        {"subtitle.missing_handler", model::SubtitleIssueType::MissingHandler},
    };
    bool matched = false;
    model::SubtitleIssueType target = model::SubtitleIssueType::EmptyText;
    for (const auto& entry : kMap) {
        if (rule.id == entry.first) {
            matched = true;
            target = entry.second;
            break;
        }
    }
    if (!matched || !result.subtitle_analyzed) return issues;

    const int count = result.subtitle.CountIssues(target);
    if (Triggered(rule, static_cast<double>(count))) {
        const model::SubtitleIssue* first = nullptr;
        for (const model::SubtitleIssue& issue : result.subtitle.issues) {
            if (issue.type == target) {
                first = &issue;
                break;
            }
        }
        std::string detail = "共 " + std::to_string(count) + " 处";
        if (first != nullptr && !first->detail.empty()) detail += "，例如：" + first->detail;
        model::TimeRange range = model::TimeRange::Global();
        int stream_index = -1;
        if (first != nullptr && !first->IsStreamLevel() && first->start_seconds >= 0.0) {
            range = model::TimeRange::At(first->start_seconds);
            stream_index = first->stream_index;
        } else if (first != nullptr) {
            stream_index = first->stream_index;
        }
        issues.push_back(make_issue(static_cast<double>(count), detail, range, stream_index,
                                    count > 0 ? count : 1));
    }
    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye