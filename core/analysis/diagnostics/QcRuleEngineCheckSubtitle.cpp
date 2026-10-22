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

    // ---------- 隐藏字幕服务层（CEA-608 通道 / CEA-708 服务）----------
    // 这两条与下面的 cue 级规则不同：它们看的是"整条 CC 轨有没有解出东西"，
    // 属于流级结论，所以不走 SubtitleIssue 的计数模型。
    if (rule.id == "subtitle.cc.no_text" || rule.id == "subtitle.cc.dtvcc_loss") {
        if (!result.subtitle_analyzed) return issues;

        if (rule.id == "subtitle.cc.no_text") {
            double count = 0.0;
            std::string detail;
            int stream_index = -1;
            for (const auto& stream : result.subtitle.streams) {
                if (stream.kind != model::SubtitleKind::ClosedCaption) continue;
                if (stream.cue_count > 0) continue;
                count += 1.0;
                if (detail.empty()) {
                    stream_index = stream.stream_index;
                    detail = "字幕流 " + std::to_string(stream.stream_index) + "（" +
                             model::ToString(stream.format) + "）没有解出任何上屏文本";
                }
            }
            if (Triggered(rule, count)) {
                issues.push_back(make_issue(count,
                    detail + "（共 " + FormatValue(count, 0) + " 条隐藏字幕流）。",
                    model::TimeRange::Global(), stream_index, static_cast<int>(count)));
            }
            return issues;
        }

        double gaps = 0.0;
        std::string detail;
        int stream_index = -1;
        for (const auto& stream : result.subtitle.streams) {
            if (stream.cc708.sequence_gap_count <= 0) continue;
            gaps += static_cast<double>(stream.cc708.sequence_gap_count);
            if (detail.empty()) {
                stream_index = stream.stream_index;
                detail = "字幕流 " + std::to_string(stream.stream_index) + " 的 DTVCC 包序号有 " +
                         std::to_string(stream.cc708.sequence_gap_count) + " 处跳变";
            }
        }
        if (Triggered(rule, gaps)) {
            issues.push_back(make_issue(gaps,
                detail + "（合计 " + FormatValue(gaps, 0) + " 处，成片会缺句）。",
                model::TimeRange::Global(), stream_index, static_cast<int>(gaps)));
        }
        return issues;
    }


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