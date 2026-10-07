#pragma once

// QC 报告导出（JSON / CSV / HTML / PDF / TXT）
//
// 相对老的 videoeye::ReportExporter::ExportQcReport*（已删）多做了两件事 ——
//   1) 固定机器可读 schema（含 profile、metrics、streams、rules 快照），供 CI 二次消费；
//   2) 支持批量汇总与双文件对比的导出。
// 手头只有裸 QcReport 时用 ExportReport()，它现场包一个最小 bundle。
//
// CSV 的口径：一个问题一行，没有"文件占一行"的变体 —— 批量透视交给
// summary CSV，混在一起会让行数失去意义。
//
// PDF 的限制：内置 Helvetica（base-14）字体只编码拉丁字符，中日韩字形无法嵌入。
// 非拉丁字符会被替换成 '?'，需要完整中文请导出 HTML。

#include <string>
#include <vector>

#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcComparator.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"

namespace videoeye {
namespace reporting {

// 单次导出所需的上下文。导出器只读这些字段，不做任何二次分析。
struct QcExportBundle {
    std::string profile_id;
    std::string profile_name;
    qc::QcRunResult run;
    bool has_comparison = false;
    qc::QcComparison comparison;
};

// PDF 结果：除了成败还要告诉调用方"有没有丢字形"，CLI 据此给出提示。
struct QcPdfExportResult {
    bool ok = false;
    bool text_loss = false;   // true = 报告里有非拉丁字符被替换成 '?'
};

// 批量汇总的一行。用扁平结构而不是直接吃 BatchQcItemResult，
// 是为了让"汇总导出"不被批量扫描的内部结构绑死。
struct QcBatchSummaryRow {
    std::string path;
    std::string status;
    std::string error;
    std::string verdict;
    std::string output_path;
    double score = 0.0;
    double elapsed_ms = 0.0;
    int critical_count = 0;
    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
};

struct QcBatchSummaryInput {
    std::string root;
    std::string profile_id;
    std::string profile_name;
    std::vector<QcBatchSummaryRow> rows;
    int succeeded = 0;
    int failed = 0;
    int timed_out = 0;
    int cancelled = 0;
    int skipped = 0;
    int critical_count = 0;
    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
    double elapsed_ms = 0.0;
    bool completed = true;
};

class QcReportExporter {
public:
    // ---- 单文件报告 ----
    // 手头只有裸 QcReport（没有 QcRunResult / profile 上下文）时的便捷入口：
    // 现场包一个最小 bundle 再走 ExportAuto。UI 上"直接对诊断结果点导出"就是这种场景，
    // 让调用方为了拿一个导出按钮而先造 QcRunResult 是没有意义的。
    static bool ExportReport(const std::string& path, const model::QcReport& report);

    // 按扩展名自动选格式；认不出来时退回 JSON。
    static bool ExportAuto(const std::string& path, const QcExportBundle& bundle);
    static bool Export(const std::string& path, const QcExportBundle& bundle,
                       qc::QcReportFormat format);

    static bool ExportJson(const std::string& path, const QcExportBundle& bundle);
    static bool ExportCsv(const std::string& path, const QcExportBundle& bundle);
    static bool ExportHtml(const std::string& path, const QcExportBundle& bundle);
    static bool ExportText(const std::string& path, const QcExportBundle& bundle);
    static QcPdfExportResult ExportPdf(const std::string& path, const QcExportBundle& bundle);

    // ---- 对比报告 ----
    static bool ExportComparisonAuto(const std::string& path, const qc::QcComparison& comparison);
    static bool ExportComparisonJson(const std::string& path, const qc::QcComparison& comparison);
    static bool ExportComparisonCsv(const std::string& path, const qc::QcComparison& comparison);
    static bool ExportComparisonHtml(const std::string& path, const qc::QcComparison& comparison);
    static bool ExportComparisonText(const std::string& path, const qc::QcComparison& comparison);

    // ---- 批量汇总 ----
    static bool ExportBatchSummaryAuto(const std::string& path, const QcBatchSummaryInput& input);
    static bool ExportBatchSummaryJson(const std::string& path, const QcBatchSummaryInput& input);
    static bool ExportBatchSummaryCsv(const std::string& path, const QcBatchSummaryInput& input);
    static bool ExportBatchSummaryHtml(const std::string& path, const QcBatchSummaryInput& input);

    // BatchQcRun -> QcBatchSummaryInput
    static QcBatchSummaryInput MakeBatchSummary(const std::string& root,
                                                const qc::BatchQcRun& run,
                                                const qc::QcProfile& profile);

    // ---- 内存里的文本稿（UI 想直接贴到 QTextEdit 时用）----
    static std::string BuildText(const QcExportBundle& bundle);
    static std::string BuildHtml(const QcExportBundle& bundle);
    static std::string BuildJson(const QcExportBundle& bundle);
    static std::string BuildComparisonText(const qc::QcComparison& comparison);
    static std::string BuildComparisonHtml(const qc::QcComparison& comparison);
};

// 生成导出文件名：<base> + <扩展名>（调用方负责目录存在）
std::string QcReportOutputPath(const std::string& directory, const std::string& base_name,
                               qc::QcReportFormat format);

// 严重的级别名（英文代号，写进 JSON 的 severity 字段）
const char* SeverityCode(model::IssueSeverity severity);
const char* CategoryCode(model::IssueCategory category);

}  // namespace reporting
}  // namespace videoeye
