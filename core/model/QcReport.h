#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/analyzer/ColorHdrAnalyzer.h"
#include "core/model/DiagnosticIssue.h"
#include "core/model/QcRule.h"

namespace videoeye {
namespace model {

// 质量检查报告：规则引擎的最终产物，也是报告导出的唯一输入。
struct QcReport {
    // 文件概要
    std::string file_path;
    std::string file_name;
    std::string container_format;
    double duration_seconds = 0.0;
    int64_t file_size_bytes = 0;
    int64_t overall_bitrate_bps = 0;
    int video_stream_count = 0;
    int audio_stream_count = 0;

    // 诊断结果
    std::vector<DiagnosticIssue> issues;
    std::vector<QcRule> rules;   // 生成报告时使用的规则快照（含阈值）

    // 色彩与 HDR 元数据快照（报告的 "Color/HDR" 章节，见 core/analyzer/ColorHdrAnalyzer.h）
    analyzer::ColorHdrAnalysis color_hdr;

    // 汇总
    double score = 100.0;            // 0..100，按问题严重度扣分
    std::string verdict;             // 通过 / 警告 / 不通过
    std::string generated_at;        // 本地时间字符串
    double analysis_elapsed_ms = 0.0;
    bool completed = true;           // false 表示分析被取消，结果不完整

    int CountBySeverity(IssueSeverity severity) const;
    int CountByCategory(IssueCategory category) const;
    bool HasBlockingIssue() const;   // 存在 Error/Critical
};

// 按严重度扣分计算 0..100 的评分（Critical -30 / Error -12 / Warning -5 / Info -1）
double ComputeQcScore(const std::vector<DiagnosticIssue>& issues);

// 评分 -> 结论文案（>=90 通过, >=70 警告, 否则不通过）
std::string ComputeQcVerdict(double score);

// 当前本地时间字符串 "YYYY-MM-DD HH:MM:SS"
std::string CurrentTimestampString();

} // namespace model
} // namespace videoeye
