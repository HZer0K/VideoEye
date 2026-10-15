#pragma once

// 报告页的导出控制器（单文件报告 / 批量汇总）。
//
// 从 ReportingPanel 搬出来的三块：
//   * 「点导出」这一次交互的完整决策：结果新鲜就直接导；不新鲜就先取输出目录挂起、
//     启动分析，等成功后在同一目录继续导出（用户点一次就能拿到当前文件 + 当前模板
//     的报告，不会静默导出上一次的结果）；
//   * 单文件报告按勾选格式落盘（PDF 走单独入口，字库缺字要提示）；
//   * 批量汇总的导出（条目列表 -> summary -> CSV/JSON/HTML）。
//
// 与 ExportCoordinator / OpenSessionController 同一套约定：
//   * 依赖在构造时注入（裸指针 / lambda，控制器不拥有它们）；
//   * 只发信号、不碰控件；对话框与弹窗以 std::function 注入，控制器不认识 QFileDialog。
// 于是上面那三条决策可以脱离 QWidget 直接单测（见 test_report_export.cpp）。

#include <functional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"
#include "core/qc/QcAnalyzeRequest.h"  // qc::QcRunResult

#include "ui/reporting_panel/report_export.h"  // DeferredExportTicket / RecomputeBatchSummary

namespace videoeye {
namespace ui {

class ReportExportController : public QObject {
    Q_OBJECT

public:
    struct Deps {
        // ---- 查询：控制器看得到面板状态，但不持有控件 ----
        std::function<bool()> has_file;          // 是否打开了媒体文件
        std::function<bool()> busy;              // 是否正在跑任务
        std::function<bool()> has_fresh_result;  // 结果是否与当前文件 + 当前模板匹配
        std::function<std::string()> current_path;
        std::function<qc::QcRunResult()> single_result;  // last_result_
        std::function<qc::QcProfile()> profile;
        std::function<std::vector<qc::QcReportFormat>()> formats;
        std::function<std::vector<qc::BatchQcItemResult>()> batch_results;
        std::function<std::string()> batch_root;  // 汇总里记的"扫描目录"

        // ---- 动作 ----
        // 结果不新鲜时挂起输出目录后要跑的那次分析。控制器不知道分析怎么起。
        std::function<void()> start_single_analysis;

        // ---- UI 交互 ----
        std::function<QString()> pick_output_directory;
        std::function<QString()> pick_summary_file;
        std::function<void(const QString& title, const QString& body)> notice;
    };

    explicit ReportExportController(Deps deps, QObject* parent = nullptr);

    // 是否挂着一个"等这次分析成功后导出到某目录"的票据。
    bool HasPendingExport() const { return pending_.armed(); }

    // 「导出报告…」按钮：新鲜就直接导，不新鲜就挂起 + 启动分析。
    void ExportSingleReport();
    // 批量汇总导出按钮。
    void ExportBatchSummary();

    // 单文件分析收尾时消费票据：ok 才导出；无论 ok 与否取走即用掉（一次分析一次导出）。
    // 由**主线程**的完成回调调用（导出要写文件、还要弹窗）。
    void OnSingleAnalysisFinished(bool ok);
    // 分析线程抛异常 / 被取消时丢弃票据 —— 免得下一次成功时导出一份不相干的东西。
    void AbortPendingExport();

signals:
    // 让报告页转发到主窗口状态栏
    void StatusMessage(const QString& text);

private:
    void ExportSingleTo(const QString& directory);

    Deps deps_;
    DeferredExportTicket pending_;
};

}  // namespace ui
}  // namespace videoeye
