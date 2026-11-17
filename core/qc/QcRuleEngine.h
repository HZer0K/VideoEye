#pragma once

#include <vector>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/DiagnosticIssue.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"

namespace videoeye {
namespace qc {

// 质量检查规则引擎
//
// 输入: 全文件扫描结果 AnalysisResult + 规则集
// 输出: QcReport（含 DiagnosticIssue 列表、评分、结论）
//
// 规则按 id 与 QcRule 表的 id 对应；阈值来自规则集（UI 可改），引擎不内置
// 魔法数字 —— 例外：个别交叉校验带固定容差（如 audio.metadata.duration_mismatch
// 的 0.5 秒，见 QcRuleEngineCheckAudio.cpp）。未启用的规则直接跳过。
class QcRuleEngine {
public:
    QcRuleEngine();
    explicit QcRuleEngine(std::vector<model::QcRule> rules);

    void SetRules(std::vector<model::QcRule> rules);
    const std::vector<model::QcRule>& rules() const {
        return rules_;
    }
    std::vector<model::QcRule>& rules() {
        return rules_;
    }

    // 执行一次评估
    model::QcReport Evaluate(const model::AnalysisResult& result) const;

private:
    // 单条规则判定：命中返回 issue 列表（一条规则可产出多条），否则返回空 vector
    std::vector<model::DiagnosticIssue> CheckRule(const model::QcRule& rule, const model::AnalysisResult& result) const;

    std::vector<model::QcRule> rules_;
};

} // namespace qc
} // namespace videoeye
