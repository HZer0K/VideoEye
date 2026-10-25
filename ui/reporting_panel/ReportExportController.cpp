#include "ui/reporting_panel/ReportExportController.h"

#include <utility>

#include "core/reporting/QcReportExporter.h"

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace ui {

ReportExportController::ReportExportController(Deps deps, QObject* parent) : QObject(parent), deps_(std::move(deps)) {
}

void ReportExportController::ExportSingleReport() {
    if (!deps_.has_file || !deps_.has_file()) {
        if (deps_.notice) {
            deps_.notice(tr("未选择文件"), tr("请先在主界面打开一个媒体文件。"));
        }
        return;
    }
    if (deps_.busy && deps_.busy()) {
        if (deps_.notice) {
            deps_.notice(tr("任务进行中"), tr("有任务正在运行，请先等待或停止。"));
        }
        return;
    }

    const QString directory = deps_.pick_output_directory ? deps_.pick_output_directory() : QString();
    if (directory.isEmpty())
        return; // 用户取消了对话框

    // 结果是否新鲜由 (文件, 模板) 共同决定（判定在 result_stamp.h）。过期时不再
    // "只启动分析就结束"——先把目录挂起，等这次分析成功后在同一目录继续导出。
    const bool fresh = deps_.has_fresh_result && deps_.has_fresh_result();
    if (!fresh) {
        pending_.Arm(directory);
        if (deps_.start_single_analysis)
            deps_.start_single_analysis();
        return;
    }
    ExportSingleTo(directory);
}

void ReportExportController::OnSingleAnalysisFinished(bool ok) {
    const QString directory = pending_.Take(); // 取走即作废：一次分析只对应一次导出
    if (directory.isEmpty())
        return;
    // 失败不再导出（会导出一份空报告）。票据已经取走，不会留到下一次成功。
    if (!ok) {
        LOG_INFO("报告页: 分析未成功，丢弃挂起的输出目录 " + directory.toStdString());
        return;
    }
    ExportSingleTo(directory);
}

void ReportExportController::AbortPendingExport() {
    if (!pending_.armed())
        return;
    LOG_INFO("报告页: 任务异常终止，丢弃挂起的输出目录");
    pending_.Clear();
}

void ReportExportController::ExportSingleTo(const QString& directory) {
    if (directory.isEmpty())
        return;

    const qc::QcRunResult result = deps_.single_result ? deps_.single_result() : qc::QcRunResult{};
    const qc::QcProfile profile = deps_.profile ? deps_.profile() : qc::QcProfile{};
    const std::vector<qc::QcReportFormat> formats = deps_.formats ? deps_.formats() : std::vector<qc::QcReportFormat>{};
    const std::string current_path = deps_.current_path ? deps_.current_path() : std::string();

    reporting::QcExportBundle bundle;
    bundle.profile_id = profile.id;
    bundle.profile_name = profile.name;
    bundle.run = result;

    // 文件名主干优先取报告里记的那个文件（批量 / 历史结果下可能与当前文件不同）
    const std::string base = SingleExportBaseName(result.report.file_path, current_path);

    bool ok = true;
    bool text_loss = false;
    const std::string dir = directory.toStdString();
    for (const qc::QcReportFormat format : formats) {
        const std::string target = reporting::QcReportOutputPath(dir, base, format);
        if (format == qc::QcReportFormat::Pdf) {
            const auto pdf = reporting::QcReportExporter::ExportPdf(target, bundle);
            ok = ok && pdf.ok;
            text_loss = text_loss || pdf.text_loss;
        } else {
            ok = ok && reporting::QcReportExporter::Export(target, bundle, format);
        }
    }
    // PDF 字库缺字只提示一次（勾选格式里 PDF 最多一个）—— 放在循环里会把同一句话
    // 弹两遍。
    if (text_loss && deps_.notice) {
        deps_.notice(tr("PDF 提示"), tr("报告里有 PDF 字体无法表示的字符（如 emoji），已替换为 '?'。"));
    }
    emit StatusMessage(ok ? tr("报告已导出到 %1").arg(directory) : tr("报告导出失败，请检查目录权限"));
}

void ReportExportController::ExportBatchSummary() {
    const std::vector<qc::BatchQcItemResult> items =
        deps_.batch_results ? deps_.batch_results() : std::vector<qc::BatchQcItemResult>{};
    if (items.empty()) {
        if (deps_.notice) {
            deps_.notice(tr("没有结果"), tr("请先跑一次批量扫描。"));
        }
        return;
    }
    const QString path = deps_.pick_summary_file ? deps_.pick_summary_file() : QString();
    if (path.isEmpty())
        return;

    qc::BatchQcRun run;
    run.items = items;
    // 汇总重算的口径在 report_export.h（ExportFailed 计 failed、completed = 无取消）
    run.summary = RecomputeBatchSummary(items);
    const qc::QcProfile profile = deps_.profile ? deps_.profile() : qc::QcProfile{};
    const std::string root = deps_.batch_root ? deps_.batch_root() : std::string();

    const auto input = reporting::QcReportExporter::MakeBatchSummary(root, run, profile);
    const bool ok = reporting::QcReportExporter::ExportBatchSummaryAuto(path.toStdString(), input);
    emit StatusMessage(ok ? tr("汇总已导出到 %1").arg(path) : tr("汇总导出失败"));
}

} // namespace ui
} // namespace videoeye
