#include "core/model/QcReport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace videoeye {
namespace model {

int QcReport::CountBySeverity(IssueSeverity severity) const {
    int count = 0;
    for (const auto& issue : issues) {
        if (issue.severity == severity) ++count;
    }
    return count;
}

int QcReport::CountByCategory(IssueCategory category) const {
    int count = 0;
    for (const auto& issue : issues) {
        if (issue.category == category) ++count;
    }
    return count;
}

bool QcReport::HasBlockingIssue() const {
    return std::any_of(issues.begin(), issues.end(), [](const DiagnosticIssue& issue) {
        return issue.severity == IssueSeverity::Error || issue.severity == IssueSeverity::Critical;
    });
}

double ComputeQcScore(const std::vector<DiagnosticIssue>& issues) {
    double penalty = 0.0;
    for (const auto& issue : issues) {
        switch (issue.severity) {
            case IssueSeverity::Critical: penalty += 30.0; break;
            case IssueSeverity::Error:    penalty += 12.0; break;
            case IssueSeverity::Warning:  penalty += 5.0;  break;
            case IssueSeverity::Info:     penalty += 1.0;  break;
        }
    }
    const double score = 100.0 - penalty;
    return (score < 0.0) ? 0.0 : score;
}

std::string ComputeQcVerdict(double score) {
    if (score >= 90.0) return "通过";
    if (score >= 70.0) return "警告";
    return "不通过";
}

std::string CurrentTimestampString() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_now{};
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                  tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    return std::string(buf);
}

} // namespace model
} // namespace videoeye
