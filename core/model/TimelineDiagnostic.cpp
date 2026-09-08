#include "core/model/TimelineDiagnostic.h"

#include <algorithm>

namespace videoeye {
namespace model {

const char* ToString(TimelineIssueType type) {
    switch (type) {
        case TimelineIssueType::PtsNonMonotonic:            return "PTS 非单调";
        case TimelineIssueType::DtsNonMonotonic:            return "DTS 非单调";
        case TimelineIssueType::DtsAfterPts:                return "DTS 晚于 PTS";
        case TimelineIssueType::AudioVideoStartOffset:      return "音视频首帧偏移";
        case TimelineIssueType::AudioVideoDurationMismatch: return "音视频时长不一致";
        case TimelineIssueType::FrameIntervalSpike:         return "帧间隔突刺";
        case TimelineIssueType::DuplicatedTimestamp:        return "重复时间戳";
        case TimelineIssueType::MissingKeyframeIndex:       return "关键帧索引缺失";
    }
    return "未知";
}

int TimelineAnalysisResult::CountOf(TimelineIssueType type) const {
    const char* rid = RuleIdOf(type);
    int count = 0;
    for (const auto& issue : issues) {
        if (issue.rule_id == rid) count += issue.occurrence_count;
    }
    return count;
}

bool TimelineAnalysisResult::HasIssue(TimelineIssueType type) const {
    const char* rid = RuleIdOf(type);
    return std::any_of(issues.begin(), issues.end(),
                       [rid](const DiagnosticIssue& issue) { return issue.rule_id == rid; });
}

} // namespace model
} // namespace videoeye
