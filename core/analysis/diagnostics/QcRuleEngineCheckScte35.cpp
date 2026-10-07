#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <string>
#include <vector>

namespace videoeye {
namespace diagnostics {
namespace detail {

// SCTE-35 规则（scte35.*）。调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue> CheckScte35Rules(const model::QcRule& rule,
                                                     const model::AnalysisResult& result,
                                                     const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

    // ---------- SCTE-35（core/analysis/diagnostics/Scte35Analyzer.h）----------
    if (rule.id == "scte35.parse_error") {
        if (!result.aux_data_analyzed) return issues;
        const int count = result.aux_data.parse_error_count;
        if (Triggered(rule, static_cast<double>(count))) {
            issues.push_back(make_issue(static_cast<double>(count),
                "有 " + std::to_string(count) +
                " 个 SCTE-35 载荷解析失败（table_id / section_length 或命令体异常）。"));
        }
        return issues;
    }
    if (rule.id == "scte35.crc_invalid") {
        if (!result.aux_data_analyzed) return issues;
        const int count = result.aux_data.crc_invalid_count;
        if (Triggered(rule, static_cast<double>(count))) {
            issues.push_back(make_issue(static_cast<double>(count),
                "有 " + std::to_string(count) +
                " 个 SCTE-35 section 的 CRC_32 校验不通过，可能是传输损坏或被拼接过。"));
        }
        return issues;
    }
    if (rule.id == "scte35.duration_missing") {
        if (!result.aux_data_analyzed) return issues;
        int count = 0;
        for (const model::Scte35Cue& cue : result.aux_data.cues) {
            // splice_insert 没带 break_duration、segmentation 也没给时长 -> 下游不知道插多长
            if (cue.valid && !cue.cancel_indicator && !cue.has_duration &&
                cue.command == model::Scte35Command::SpliceInsert) {
                bool seg_duration = false;
                for (const model::Scte35Segmentation& seg : cue.segmentation) {
                    if (seg.has_duration) seg_duration = true;
                }
                if (!seg_duration) ++count;
            }
        }
        if (Triggered(rule, static_cast<double>(count))) {
            issues.push_back(make_issue(static_cast<double>(count),
                "有 " + std::to_string(count) +
                " 条 splice_insert cue 没有给 duration，下游无法判断广告插入多长。"));
        }
        return issues;
    }

    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye