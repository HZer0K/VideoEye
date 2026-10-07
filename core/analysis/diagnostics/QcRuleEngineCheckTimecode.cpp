#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <string>
#include <utility>
#include <vector>

namespace videoeye {
namespace diagnostics {
namespace detail {

// 时码与章节规则（timecode.* / chapter.*）。调用方（CheckRule 前缀路由）保证
// rule.id 属于本类。
std::vector<model::DiagnosticIssue> CheckTimecodeChapterRules(const model::QcRule& rule,
                                                              const model::AnalysisResult& result,
                                                              const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

    // ---------- 时码与章节（core/analysis/diagnostics/TimecodeAnalyzer.h）----------
    if (rule.id == "timecode.missing") {
        if (!result.timecode_analyzed) return issues;
        if (Triggered(rule, result.timecode.has_primary ? 0.0 : 1.0)) {
            issues.push_back(make_issue(1.0,
                "未找到时码：既没有 MOV/MP4 的 tmcd 时码轨，也没有 metadata 里的 timecode tag。"));
        }
        return issues;
    }
    if (rule.id == "timecode.drop_frame_mismatch") {
        if (!result.timecode_analyzed || !result.timecode.has_primary) return issues;
        const double fps = result.timecode.primary_frame_rate;
        if (fps <= 0.0) return issues;
        const bool rate_is_ntsc = model::IsDropFrameRate(fps);
        const bool mismatch = (rate_is_ntsc && !result.timecode.primary_drop_frame) ||
                              (!rate_is_ntsc && result.timecode.primary_drop_frame);
        if (Triggered(rule, mismatch ? 1.0 : 0.0)) {
            issues.push_back(make_issue(1.0,
                "帧率 " + FormatValue(fps, 3) + " fps 与时码标记 " +
                (result.timecode.primary_drop_frame ? "drop-frame" : "non-drop-frame") +
                " 不一致；29.97 素材用 non-drop 时码，一小时会累积约 3.6 秒偏差。",
                model::TimeRange::At(0.0)));
        }
        return issues;
    }
    if (rule.id == "timecode.invalid_frame") {
        if (!result.timecode_analyzed || !result.timecode.has_primary) return issues;
        const double fps = result.timecode.primary_frame_rate;
        if (fps <= 0.0) return issues;
        if (Triggered(rule, model::IsValidTimecode(result.timecode.primary, fps) ? 0.0 : 1.0)) {
            issues.push_back(make_issue(1.0,
                "首帧时码 " + result.timecode.primary.ToString() + " 在 " +
                FormatValue(fps, 3) + " fps 下不合法（帧号越界或 drop-frame 跳帧位置错误）。",
                model::TimeRange::At(0.0)));
        }
        return issues;
    }

    static const std::pair<const char*, model::ChapterIssueType> kMap[] = {
        {"chapter.overlap", model::ChapterIssueType::Overlap},
        {"chapter.out_of_range", model::ChapterIssueType::OutOfRange},
        {"chapter.non_monotonic", model::ChapterIssueType::NonMonotonic},
        {"chapter.zero_duration", model::ChapterIssueType::ZeroDuration},
        {"chapter.missing_title", model::ChapterIssueType::MissingTitle},
    };
    bool matched = false;
    model::ChapterIssueType target = model::ChapterIssueType::Overlap;
    for (const auto& entry : kMap) {
        if (rule.id == entry.first) {
            matched = true;
            target = entry.second;
            break;
        }
    }
    if (!matched || !result.timecode_analyzed) return issues;

    const int count = result.timecode.CountChapterIssues(target);
    if (Triggered(rule, static_cast<double>(count))) {
        const model::ChapterIssue* first = nullptr;
        for (const model::ChapterIssue& issue : result.timecode.chapter_issues) {
            if (issue.type == target) {
                first = &issue;
                break;
            }
        }
        std::string detail = "共 " + std::to_string(count) + " 处";
        if (first != nullptr && !first->detail.empty()) detail += "，例如：" + first->detail;
        model::TimeRange range = model::TimeRange::Global();
        if (first != nullptr && first->start_seconds >= 0.0) {
            range = model::TimeRange::At(first->start_seconds);
        }
        issues.push_back(make_issue(static_cast<double>(count), detail, range, -1,
                                    count > 0 ? count : 1));
    }
    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye