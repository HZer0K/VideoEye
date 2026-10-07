#include "core/reporting/QcReportExporter.h"

#include "core/reporting/QcReportExporterInternal.h"

#include <sstream>

namespace videoeye {
namespace reporting {

using detail::Fixed;
using detail::WriteUtf8File;

// ===========================================================================
// 纯文本
// ===========================================================================

std::string QcReportExporter::BuildText(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    std::ostringstream out;
    out << "========================================\n  VideoEye QC 报告\n"
        << "========================================\n";
    out << "模板: " << bundle.profile_name << " (" << bundle.profile_id << ")\n";
    out << "文件: " << report.file_path << "\n";
    out << "生成时间: " << report.generated_at << "\n";
    out << "分析耗时: " << Fixed(run.elapsed_ms, 0) << " ms\n";
    out << "评分: " << Fixed(report.score, 1) << " / 100 (" << report.verdict << ")\n";
    if (report.partial) out << "注意: 本次为抽样扫描（命中包数上限），结果仅覆盖部分数据，结论仅供参考\n";
    else if (!report.completed) out << "注意: 分析未跑完，结果不完整\n";
    if (!run.error.empty()) out << "错误: " << run.error << "\n";

    out << "\n--- 汇总 ---\n";
    out << "致命 " << report.CountBySeverity(model::IssueSeverity::Critical) << " / 错误 "
        << report.CountBySeverity(model::IssueSeverity::Error) << " / 警告 "
        << report.CountBySeverity(model::IssueSeverity::Warning) << " / 提示 "
        << report.CountBySeverity(model::IssueSeverity::Info) << "\n";
    out << "时长 " << Fixed(report.duration_seconds, 3) << " s ｜ 大小 " << report.file_size_bytes
        << " 字节 ｜ 码率 " << Fixed(report.overall_bitrate_bps / 1000.0, 1) << " kbps\n";

    out << "\n--- 问题清单 ---\n";
    if (report.issues.empty()) {
        out << "未发现问题。\n";
    } else {
        for (std::size_t i = 0; i < report.issues.size(); ++i) {
            const auto& issue = report.issues[i];
            out << "[" << (i + 1) << "] " << issue.SeverityText() << " / " << issue.CategoryText()
                << " / " << issue.title << " (" << issue.rule_id << ")\n";
            out << "    位置: " << issue.range.ToString() << "\n";
            out << "    " << issue.detail << "\n";
            if (!issue.suggestion.empty()) out << "    建议: " << issue.suggestion << "\n";
        }
    }

    if (run.analysis.audio_qc.analyzed) {
        out << "\n--- 响度 ---\n";
        out << "  积分响度: " << Fixed(run.analysis.audio_qc.integrated_lufs, 2) << " LUFS\n";
        out << "  真峰值: " << Fixed(run.analysis.audio_qc.true_peak_dbtp, 2) << " dBTP\n";
        out << "  LRA: " << Fixed(run.analysis.audio_qc.loudness_range_lu, 2) << " LU\n";
    }

    if (bundle.has_comparison) {
        out << "\n--- 对比 ---\n" << BuildComparisonText(bundle.comparison);
    }

    return out.str();
}

bool QcReportExporter::ExportText(const std::string& path, const QcExportBundle& bundle) {
    return WriteUtf8File(path, BuildText(bundle));
}

// ===========================================================================
// 对比导出
// ===========================================================================

std::string QcReportExporter::BuildComparisonText(const qc::QcComparison& comparison) {
    std::ostringstream out;
    out << "原文件  : " << comparison.left_path << "\n";
    out << "对比文件: " << comparison.right_path << "\n";
    out << "评分    : " << Fixed(comparison.left_score, 1) << " -> "
        << Fixed(comparison.right_score, 1) << "\n";
    out << "差异项  : " << comparison.DifferentCount() << " / " << comparison.AvailableRowCount()
        << "\n";

    for (const auto& group : comparison.GroupNames()) {
        out << "\n[" << group << "]\n";
        for (const auto& row : comparison.rows) {
            if (row.group != group) continue;
            out << "  " << row.field << ": " << row.left;
            if (!row.unit.empty()) out << " " << row.unit;
            out << "  ->  " << row.right;
            if (!row.unit.empty()) out << " " << row.unit;
            if (row.diff == qc::QcFieldDiff::Different) {
                out << "   (差 " << Fixed(row.delta, 3) << (row.unit.empty() ? "" : " " + row.unit) << ")";
            } else if (row.diff != qc::QcFieldDiff::Same) {
                out << "   (" << qc::ToString(row.diff) << ")";
            }
            out << "\n";
        }
    }
    return out.str();
}

bool QcReportExporter::ExportComparisonText(const std::string& path,
                                           const qc::QcComparison& comparison) {
    return WriteUtf8File(path, BuildComparisonText(comparison));
}

}  // namespace reporting
}  // namespace videoeye