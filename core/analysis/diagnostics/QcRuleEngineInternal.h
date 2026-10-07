#pragma once

// QcRuleEngine 拆分后的跨翻译单元内部头（不对上层暴露）。
//
// 调用契约：
//   * QcRuleEngine::CheckRule 负责按 rule.id 前缀路由，是规则「成员资格」的
//     唯一判定点 —— 分类函数不再重复做前缀判断；
//   * 下方 Check*Rules 分类函数假定传入的 rule.id 属于本类，只负责本类的
//     判定逻辑（阈值、文案、字段映射、分支顺序与拆分前逐字一致）；
//   * 只被单一 TU 使用的 helper 一律放在该 TU 的匿名 namespace，不进此头。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/DiagnosticIssue.h"
#include "core/domain/model/QcRule.h"

namespace videoeye {
namespace diagnostics {
namespace detail {

inline std::string FormatValue(double value, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

inline bool Triggered(const model::QcRule& rule, double value) {
    switch (rule.op) {
        case model::QcRuleOp::MaxExceeded: return value > rule.threshold;
        case model::QcRuleOp::MinBelow:    return value < rule.threshold;
        case model::QcRuleOp::NonZero:     return std::abs(value) > 1e-9;
    }
    return false;
}

inline std::string ThresholdText(const model::QcRule& rule) {
    if (rule.op == model::QcRuleOp::NonZero) return "存在";
    const std::string op_text = (rule.op == model::QcRuleOp::MaxExceeded) ? ">" : "<";
    return op_text + " " + FormatValue(rule.threshold, 2) + rule.unit;
}

using IssueList = std::vector<model::DiagnosticIssue>;

// 原 CheckRule 开头 make_issue lambda 的逐字替身：按规则模板装配 issue。
struct IssueFactory {
    const model::QcRule& rule;

    model::DiagnosticIssue operator()(double value, const std::string& detail,
                                      model::TimeRange range = model::TimeRange::Global(),
                                      int stream_index = -1, int occurrences = 1) const {
        model::DiagnosticIssue issue;
        issue.rule_id = rule.id;
        issue.title = rule.name;
        issue.detail = detail;
        issue.suggestion = rule.suggestion;
        issue.severity = rule.severity;
        issue.category = rule.category;
        issue.range = range;
        issue.stream_index = stream_index;
        issue.metric_value = value;
        issue.threshold = rule.threshold;
        issue.occurrence_count = occurrences;
        return issue;
    }
};

// 分类入口（各 TU 定义）。签名统一：rule + 全文件结果 + issue 工厂。
std::vector<model::DiagnosticIssue> CheckContainerRules(const model::QcRule& rule,
                                                        const model::AnalysisResult& result,
                                                        const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckMp4ContainerRules(const model::QcRule& rule,
                                                           const model::AnalysisResult& result,
                                                           const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckStreamingContainerRules(const model::QcRule& rule,
                                                                 const model::AnalysisResult& result,
                                                                 const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckVideoRules(const model::QcRule& rule,
                                                    const model::AnalysisResult& result,
                                                    const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckAudioRules(const model::QcRule& rule,
                                                    const model::AnalysisResult& result,
                                                    const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckColorHdrRules(const model::QcRule& rule,
                                                       const model::AnalysisResult& result,
                                                       const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckSubtitleRules(const model::QcRule& rule,
                                                       const model::AnalysisResult& result,
                                                       const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckTimecodeChapterRules(const model::QcRule& rule,
                                                              const model::AnalysisResult& result,
                                                              const IssueFactory& make_issue);
std::vector<model::DiagnosticIssue> CheckScte35Rules(const model::QcRule& rule,
                                                     const model::AnalysisResult& result,
                                                     const IssueFactory& make_issue);

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye