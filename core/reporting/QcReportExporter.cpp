#include "core/reporting/QcReportExporter.h"

namespace videoeye {
namespace reporting {

// ===========================================================================
// 非成员 helper：严重度/类别代号与导出路径拼接
// ===========================================================================

const char* SeverityCode(model::IssueSeverity severity) {
    switch (severity) {
        case model::IssueSeverity::Critical: return "critical";
        case model::IssueSeverity::Error:    return "error";
        case model::IssueSeverity::Warning:  return "warning";
        case model::IssueSeverity::Info:     return "info";
    }
    return "info";
}

const char* CategoryCode(model::IssueCategory category) {
    switch (category) {
        case model::IssueCategory::Container: return "container";
        case model::IssueCategory::Video:     return "video";
        case model::IssueCategory::Audio:     return "audio";
        case model::IssueCategory::Timing:    return "timing";
        case model::IssueCategory::Bitrate:   return "bitrate";
        case model::IssueCategory::Gop:       return "gop";
        case model::IssueCategory::Metadata:  return "metadata";
        case model::IssueCategory::ColorHdr:  return "color_hdr";
        case model::IssueCategory::Other:     return "other";
    }
    return "other";
}

std::string QcReportOutputPath(const std::string& directory, const std::string& base_name,
                               qc::QcReportFormat format) {
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += base_name;
    path += qc::QcReportExtension(format);
    return path;
}

// ===========================================================================
// 各格式的 Auto 分派入口（具体 Export* 实现见 QcReportExporter{Json,Csv,Html,Text,Pdf}.cpp）
// ===========================================================================

bool QcReportExporter::ExportComparisonAuto(const std::string& path,
                                           const qc::QcComparison& comparison) {
    qc::QcReportFormat format = qc::QcReportFormat::Json;
    if (!qc::QcReportFormatFromPath(path, format)) format = qc::QcReportFormat::Json;
    switch (format) {
        case qc::QcReportFormat::Json: return ExportComparisonJson(path, comparison);
        case qc::QcReportFormat::Csv:  return ExportComparisonCsv(path, comparison);
        case qc::QcReportFormat::Html: return ExportComparisonHtml(path, comparison);
        case qc::QcReportFormat::Text: return ExportComparisonText(path, comparison);
        case qc::QcReportFormat::Pdf:  return ExportComparisonText(path, comparison);
    }
    return false;
}

// ===========================================================================
// 批量汇总：BatchQcRun -> QcBatchSummaryInput
// ===========================================================================

QcBatchSummaryInput QcReportExporter::MakeBatchSummary(const std::string& root,
                                                       const qc::BatchQcRun& run,
                                                       const qc::QcProfile& profile) {
    QcBatchSummaryInput input;
    input.root = root;
    input.profile_id = profile.id;
    input.profile_name = profile.name;

    for (const auto& item : run.items) {
        QcBatchSummaryRow row;
        row.path = item.path;
        row.status = qc::ToString(item.status);
        row.error = item.error;
        row.verdict = item.verdict;
        row.output_path = item.output_path;
        row.score = item.score;
        row.elapsed_ms = item.elapsed_ms;
        row.critical_count = item.critical_count;
        row.error_count = item.error_count;
        row.warning_count = item.warning_count;
        row.info_count = item.info_count;
        input.rows.push_back(std::move(row));
    }

    input.succeeded = run.summary.succeeded;
    input.failed = run.summary.failed;
    input.timed_out = run.summary.timed_out;
    input.cancelled = run.summary.cancelled;
    input.skipped = run.summary.skipped;
    input.critical_count = run.summary.critical_count;
    input.error_count = run.summary.error_count;
    input.warning_count = run.summary.warning_count;
    input.info_count = run.summary.info_count;
    input.elapsed_ms = run.summary.elapsed_ms;
    input.completed = run.summary.completed;
    return input;
}

bool QcReportExporter::ExportBatchSummaryAuto(const std::string& path,
                                             const QcBatchSummaryInput& input) {
    qc::QcReportFormat format = qc::QcReportFormat::Json;
    if (!qc::QcReportFormatFromPath(path, format)) format = qc::QcReportFormat::Json;
    switch (format) {
        case qc::QcReportFormat::Json: return ExportBatchSummaryJson(path, input);
        case qc::QcReportFormat::Csv:  return ExportBatchSummaryCsv(path, input);
        case qc::QcReportFormat::Html: return ExportBatchSummaryHtml(path, input);
        case qc::QcReportFormat::Text: return ExportBatchSummaryCsv(path, input);
        case qc::QcReportFormat::Pdf:  return ExportBatchSummaryHtml(path, input);
    }
    return false;
}

// ===========================================================================
// 分派
// ===========================================================================

bool QcReportExporter::Export(const std::string& path, const QcExportBundle& bundle,
                              qc::QcReportFormat format) {
    switch (format) {
        case qc::QcReportFormat::Json: return ExportJson(path, bundle);
        case qc::QcReportFormat::Csv:  return ExportCsv(path, bundle);
        case qc::QcReportFormat::Html: return ExportHtml(path, bundle);
        case qc::QcReportFormat::Text: return ExportText(path, bundle);
        case qc::QcReportFormat::Pdf:  return ExportPdf(path, bundle).ok;
    }
    return false;
}

bool QcReportExporter::ExportReport(const std::string& path, const model::QcReport& report) {
    // 没有 profile 上下文可写，用占位值顶上 —— 报告主体（issues / metrics / streams）
    // 全部来自 report 本身，profile 只在页眉出现一次。
    QcExportBundle bundle;
    bundle.profile_id = "default";
    bundle.profile_name = "默认规则";
    bundle.run.ok = report.completed;
    bundle.run.report = report;
    if (!bundle.run.ok) bundle.run.error = "分析未完成";
    bundle.has_comparison = false;
    return ExportAuto(path, bundle);
}

bool QcReportExporter::ExportAuto(const std::string& path, const QcExportBundle& bundle) {
    qc::QcReportFormat format = qc::QcReportFormat::Json;
    if (!qc::QcReportFormatFromPath(path, format)) format = qc::QcReportFormat::Json;
    return Export(path, bundle, format);
}

}  // namespace reporting
}  // namespace videoeye