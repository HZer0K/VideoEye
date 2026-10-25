#include "core/domain/model/QcRule.h"

#include <algorithm>

namespace videoeye {
namespace model {

const char* ToString(IssueSeverity severity) {
    switch (severity) {
    case IssueSeverity::Info:
        return "提示";
    case IssueSeverity::Warning:
        return "警告";
    case IssueSeverity::Error:
        return "错误";
    case IssueSeverity::Critical:
        return "致命";
    }
    return "未知";
}

const char* ToString(IssueCategory category) {
    switch (category) {
    case IssueCategory::Container:
        return "容器";
    case IssueCategory::Video:
        return "视频";
    case IssueCategory::Audio:
        return "音频";
    case IssueCategory::Timing:
        return "时间戳";
    case IssueCategory::Bitrate:
        return "码率";
    case IssueCategory::Gop:
        return "GOP";
    case IssueCategory::Metadata:
        return "元数据";
    case IssueCategory::ColorHdr:
        return "色彩/HDR";
    case IssueCategory::Other:
        return "其他";
    }
    return "未知";
}

const char* ToString(QcRuleOp op) {
    switch (op) {
    case QcRuleOp::MaxExceeded:
        return "超过上限";
    case QcRuleOp::MinBelow:
        return "低于下限";
    case QcRuleOp::NonZero:
        return "存在即告警";
    }
    return "未知";
}

QcRule* FindQcRule(std::vector<QcRule>& rules, const std::string& id) {
    auto it = std::find_if(rules.begin(), rules.end(), [&id](const QcRule& r) { return r.id == id; });
    return (it == rules.end()) ? nullptr : &(*it);
}

const QcRule* FindQcRule(const std::vector<QcRule>& rules, const std::string& id) {
    auto it = std::find_if(rules.begin(), rules.end(), [&id](const QcRule& r) { return r.id == id; });
    return (it == rules.end()) ? nullptr : &(*it);
}

} // namespace model
} // namespace videoeye
