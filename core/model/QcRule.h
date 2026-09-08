#pragma once

#include <string>
#include <vector>

#include "core/model/DiagnosticIssue.h"

namespace videoeye {
namespace model {

// 规则判定方式
enum class QcRuleOp {
    MaxExceeded,  // 实测值 > threshold 时触发（上限类）
    MinBelow,     // 实测值 < threshold 时触发（下限类）
    NonZero       // 实测值 != 0 时触发（存在性/计数类）
};

const char* ToString(QcRuleOp op);

// 一条质量检查规则。阈值可在 UI 中调整，规则集可整体持久化（当前仅内存态）。
struct QcRule {
    std::string id;                 // 稳定标识, 引擎按 id 匹配实现
    std::string name;               // 展示名
    IssueCategory category = IssueCategory::Other;
    IssueSeverity severity = IssueSeverity::Warning;
    QcRuleOp op = QcRuleOp::MaxExceeded;
    double threshold = 0.0;         // 阈值（单位见 unit）
    std::string unit;               // 阈值单位, 如 "s" / "kbps" / "%" / ""
    bool enabled = true;
    std::string description;        // 规则说明
    std::string suggestion;         // 命中后的修复建议
};

// 默认规则集（顺序即 UI 展示顺序）
std::vector<QcRule> DefaultQcRules();

// 在规则集中按 id 查找（未找到返回 nullptr）
QcRule* FindQcRule(std::vector<QcRule>& rules, const std::string& id);
const QcRule* FindQcRule(const std::vector<QcRule>& rules, const std::string& id);

} // namespace model
} // namespace videoeye
