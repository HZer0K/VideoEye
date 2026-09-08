#pragma once

#include <vector>

#include "core/analyzer/AnalysisTask.h"
#include "core/model/DiagnosticIssue.h"
#include "core/model/QcReport.h"
#include "core/model/QcRule.h"

namespace videoeye {
namespace analyzer {

// 质量检查规则引擎
//
// 输入: 全文件扫描结果 AnalysisResult + 规则集
// 输出: QcReport（含 DiagnosticIssue 列表、评分、结论）
//
// 规则按 id 与 QcRule 表的 id 对应；阈值全部来自规则集（UI 可改），
// 引擎本身不内置魔法数字。未启用的规则直接跳过。
class QcRuleEngine {
public:
    QcRuleEngine();
    explicit QcRuleEngine(std::vector<model::QcRule> rules);

    void SetRules(std::vector<model::QcRule> rules);
    const std::vector<model::QcRule>& rules() const { return rules_; }
    std::vector<model::QcRule>& rules() { return rules_; }

    // 执行一次评估
    model::QcReport Evaluate(const AnalysisResult& result) const;

private:
    // 单条规则判定：命中则返回 issue，否则返回 std::nullopt
    std::vector<model::DiagnosticIssue> CheckRule(const model::QcRule& rule,
                                                  const AnalysisResult& result) const;

    std::vector<model::QcRule> rules_;
};

} // namespace analyzer
} // namespace videoeye
