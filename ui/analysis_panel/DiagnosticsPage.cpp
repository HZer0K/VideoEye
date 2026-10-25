#include "ui/analysis_panel/DiagnosticsPage.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

#include <chrono>

#include "core/qc/QcRuleDefaults.h"
#include "core/reporting/QcReportExporter.h"
#include "infrastructure/logging/PerfMetrics.h"
#include "infrastructure/logging/ScopedTimer.h"
#include "ui/analysis_panel/DiagnosticsIssueView.h"
#include "ui/analysis_panel/EvidenceCoverageView.h"
#include "ui/analysis_panel/FrameOrderView.h"
#include "ui/analysis_panel/ParameterTimelineView.h"
#include "ui/analysis_panel/QcRuleTableView.h"

namespace videoeye {
namespace ui {
namespace {

// 内存指标只在"干完一件大事"之后采一次（分析结束 / 导出结束），不轮询：
//   * 峰值取 OS 维护的高水位，本来就不需要轮询；
//   * 每次都写日志的话，长视频分析过程中会刷出成千上万行，反而看不清。
// 采样失败返回 0 —— 与"真的用了 0 MB"区分不了，所以 0 时跳过，不写假指标。
void RecordMemoryMetrics() {
    const double peak_mb = PerfMetrics::PeakRssMb();
    if (peak_mb > 0.0) {
        PerfMetrics::GetInstance().RecordGauge(kMetricPeakMemoryMb, peak_mb, "MB");
    }
    const double rss_mb = PerfMetrics::CurrentRssMb();
    if (rss_mb > 0.0) {
        PerfMetrics::GetInstance().RecordGauge(kMetricRssMemoryMb, rss_mb, "MB");
    }
}

} // namespace

DiagnosticsPage::DiagnosticsPage(QWidget* parent) : QWidget(parent) {
    // ⚠️ facade_ 必须**先于** SetupUi() 建好。
    //
    // SetupUi() 里「规则与阈值」子页要从 facade_->rules() 取规则集重建表格。
    // 这里以前是反的（先 SetupUi() 再 new AnalysisFacade），于是重建表格时
    // 解引用了一个还是 nullptr 的 facade_ —— 对空指针调非虚成员函数是 UB：
    // 优化构建下表现为进程启动即 0xC0000005（打开软件就闪退），未优化构建下同样崩。
    // 构造函数里"先建 UI、后建数据"的顺序看着自然，但只要有一步 UI 初始化要读数据，
    // 就会踩这个坑；所以这里把 facade_ 提到最前面，并把它当成不变式：
    // **SetupUi() 返回后，facade_ / 各子控件一律可用。**
    facade_ = new AnalysisFacade(this);
    connect(facade_, &AnalysisFacade::ProgressReported, this, &DiagnosticsPage::OnFacadeProgress);
    connect(facade_, &AnalysisFacade::AnalysisFinished, this, &DiagnosticsPage::OnFacadeFinished);
    connect(facade_, &AnalysisFacade::AnalysisFailed, this, &DiagnosticsPage::OnFacadeFailed);

    SetupUi();
}

// ---------------------------------------------------------------------------
// UI 搭建
// ---------------------------------------------------------------------------

void DiagnosticsPage::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 第一行: 标题 + 控制按钮
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("诊断与报告（全文件扫描）"), row);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 1);
        title->setFont(title_font);
        rl->addWidget(title);
        rl->addStretch();

        start_button_ = new QPushButton(tr("开始分析"), row);
        start_button_->setToolTip(tr("对当前文件做一次完整 demux 扫描并按规则生成诊断报告"));
        connect(start_button_, &QPushButton::clicked, this, [this]() {
            if (source_path_.isEmpty()) {
                QMessageBox::information(this, tr("提示"), tr("请先打开一个媒体文件。"));
                return;
            }
            StartScan(options_);
        });
        rl->addWidget(start_button_);

        cancel_button_ = new QPushButton(tr("取消"), row);
        cancel_button_->setEnabled(false);
        connect(cancel_button_, &QPushButton::clicked, this, &DiagnosticsPage::OnCancelClicked);

        // 取消收敛看门狗：只在 CancelScan() 里 start，终态一到就 stop。
        // 挂在页面对象上、不设 singleShot —— 超时后要每秒续一次把等待时长刷出来。
        cancel_watchdog_ = new QTimer(this);
        cancel_watchdog_->setObjectName(QStringLiteral("cancel_watchdog"));
        cancel_watchdog_->setSingleShot(true);
        connect(cancel_watchdog_, &QTimer::timeout, this, &DiagnosticsPage::OnCancelWatchdog);
        rl->addWidget(cancel_button_);

        export_button_ = new QPushButton(tr("导出报告"), row);
        export_button_->setEnabled(false);
        connect(export_button_, &QPushButton::clicked, this, &DiagnosticsPage::OnExportClicked);
        rl->addWidget(export_button_);
        layout->addWidget(row);
    }

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(true);
    progress_bar_->setFormat(tr("未开始"));
    layout->addWidget(progress_bar_);

    summary_label_ = new QLabel(tr("点击「开始分析」对当前文件做一次完整扫描，将按内置 QC 规则输出问题清单与评分。"));
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    sub_tabs_ = new QTabWidget(this);
    sub_tabs_->setMinimumHeight(360);

    // ---- 子页 0: 问题清单（曲线 + 问题表 + 跳转）----
    issue_view_ = new DiagnosticsIssueView(sub_tabs_);
    connect(issue_view_, &DiagnosticsIssueView::SeekRequested, this, &DiagnosticsPage::SeekRequested);
    sub_tabs_->addTab(issue_view_, tr("问题清单"));

    // ---- 子页 1: 规则与阈值 ----
    // 规则集归 facade 所有，视图通过 provider 就地读写；每次有效改动由本页决定
    // 是否用当前扫描结果重算报告。
    rule_view_ = new QcRuleTableView(sub_tabs_);
    rule_view_->SetRulesProvider([this]() -> std::vector<model::QcRule>& { return facade_->rules(); });
    connect(rule_view_, &QcRuleTableView::RulesChanged, this, [this]() {
        if (has_result_)
            Evaluate();
    });
    connect(rule_view_, &QcRuleTableView::ResetRequested, this, [this]() {
        facade_->SetRules(qc::DefaultQcRules());
        rule_view_->Reload();
        if (has_result_)
            Evaluate();
    });
    sub_tabs_->addTab(rule_view_, tr("规则与阈值"));

    // ---- 子页 2/3/4: 证据与覆盖 / 参数变化 / 帧顺序 ----
    // 三张表都是"从扫描结果直接摊开的表"，不参与规则判定；各自独立成组件后
    // 本页只把结果指针分发下去。
    coverage_view_ = new EvidenceCoverageView(sub_tabs_);
    sub_tabs_->addTab(coverage_view_, tr("证据与覆盖"));

    parameter_view_ = new ParameterTimelineView(sub_tabs_);
    sub_tabs_->addTab(parameter_view_, tr("参数变化"));

    frame_order_view_ = new FrameOrderView(sub_tabs_);
    sub_tabs_->addTab(frame_order_view_, tr("帧顺序"));

    layout->addWidget(sub_tabs_);
}

// ---------------------------------------------------------------------------
// 全文件扫描
// ---------------------------------------------------------------------------

void DiagnosticsPage::StartScan(const videoeye::AnalysisOptions& options, bool silent) {
    // 静默模式（打开失败自动补扫等）不弹模态框：这是流程里的必经一步，
    // 弹窗会打断流程、扫到一半卡在模态循环里等不到人点。条件不满足就悄悄放弃。
    if (source_path_.isEmpty()) {
        if (!silent)
            QMessageBox::information(this, tr("提示"), tr("请先打开一个媒体文件。"));
        return;
    }
    if (facade_->IsRunning()) {
        if (!silent)
            QMessageBox::information(this, tr("提示"), tr("分析正在进行中。"));
        return;
    }

    scan_start_time_ = std::chrono::steady_clock::now();
    has_result_ = false;
    // 新的一次扫描：上一次的失败原因不再说明当前状态（首页「分析状态」区据此清红字）
    last_error_.clear();
    // 上一次扫描若还停在"取消中"的状态（后台没收敛就开了新的），这里必须清干净：
    // 否则看门狗还在跑，会把新扫描的进度条文案覆盖成"仍在收尾"。
    cancel_pending_ = false;
    cancel_probe_.Abandon();
    if (cancel_watchdog_)
        cancel_watchdog_->stop();
    issue_view_->Clear();
    export_button_->setEnabled(false);
    SetScanButtonState(true);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("准备中 %p%"));
    summary_label_->setText(tr("正在扫描: %1").arg(source_path_));

    emit ScanStarted();

    // 字幕阈值同步之类的跨页逻辑由面板在扫描前注入
    videoeye::AnalysisOptions effective = options;
    if (before_scan_)
        before_scan_(effective);

    generation_ = facade_->StartAnalysis(source_path_.toStdString(), effective);
}

void DiagnosticsPage::CancelScan() {
    // 延迟预算的第一段：**请求 -> 界面状态更新**。这段全在主线程上，
    // 理论上就是一个事件循环的事（kCancelResponseBudgetMs = 100ms）。
    // 埋点的意义在于：哪天有人往这条路径里塞了同步等待，它会立刻超预算报警，
    // 而不是等用户抱怨"点了取消没反应"。
    const auto requested_at = std::chrono::steady_clock::now();

    // 没有在跑的扫描时"取消"是无意义的：不进"取消中"态、也不起看门狗 ——
    // 否则看门狗会去等一个永远不会来的终态，然后每秒把进度条刷一次"仍在收尾"，
    // 而界面上其实什么都没在做。取消按钮在空闲态本来就是禁用的，这里是防
    // AnalysisPanel 那种把各页 CancelRequested 都转进来的批量入口误调。
    if (!facade_->IsRunning()) {
        facade_->Cancel();
        return;
    }

    facade_->Cancel();
    cancel_pending_ = true;
    cancel_probe_.Arm();
    cancel_button_->setEnabled(false);
    progress_bar_->setFormat(tr("取消中..."));

    // 收敛兜底：预算时间（kCancelSettleBudgetMs）之后如果还没收到终态，
    // 说明后台没能按预期退出。此时**不弹框、不自动放弃** —— 只是把等待时长
    // 持续显示在进度条上，让人知道"还在收尾"而不是"卡死了"。
    if (cancel_watchdog_) {
        cancel_watchdog_->start(static_cast<int>(videoeye::kCancelSettleBudgetMs));
    }

    const double response_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requested_at).count();
    PerfMetrics::GetInstance().RecordTiming(videoeye::kMetricCancelResponseMs, response_ms,
                                            videoeye::kCancelResponseBudgetMs);

    // 注意这里**不发终态信号**: 取消只是请求, 任务还在跑。终态由 OnFacadeFinished
    // 统一发出 —— 共用同一次扫描的几页在真正结束时才一起回到 Idle, 不会出现
    // "面板说取消了、后台其实还在跑"的中间态。
}

// 取消后超过预算仍未收敛：把已等待时长继续显示出来（每秒刷新一次）。
//
// 为什么不是"超时就强制恢复界面"：强制恢复等于承认"后台还在写结果对象"，
// 而它随后到达的终态会覆盖掉用户新开的那次分析 —— 状态分家比多等几秒糟得多。
// 所以这里只做告知，恢复仍然只认终态这一个入口。
void DiagnosticsPage::OnCancelWatchdog() {
    if (!cancel_pending_)
        return;
    const double waited_s = cancel_probe_.ElapsedMs() / 1000.0;
    progress_bar_->setFormat(tr("取消中（后台仍在收尾 %1 秒）").arg(QString::number(waited_s, 'f', 1)));
    if (cancel_watchdog_)
        cancel_watchdog_->start(1000);
}

// 终态到达：停看门狗 + 结算"取消请求 -> 后台真正收敛"的延迟。
// 成功 / 取消 / 失败三条路都走这里，避免漏掉一条导致看门狗一直转。
void DiagnosticsPage::ClearCancelPending() {
    cancel_pending_ = false;
    if (cancel_watchdog_)
        cancel_watchdog_->stop();
    if (cancel_probe_.Armed()) {
        cancel_probe_.Settle(); // 内部记 cancel_settle_ms 并按预算判定
    }
}

void DiagnosticsPage::SetScanButtonState(bool running) {
    start_button_->setEnabled(!running);
    cancel_button_->setEnabled(running);
}

void DiagnosticsPage::OnCancelClicked() {
    CancelScan();
}

void DiagnosticsPage::OnFacadeProgress(quint64 generation, double percent, const QString& stage) {
    if (generation != generation_)
        return; // 旧任务的回调直接丢弃
    progress_bar_->setValue(static_cast<int>(percent));
    progress_bar_->setFormat(stage + " %p%");
    emit ProgressChanged(percent, stage);
}

void DiagnosticsPage::OnFacadeFinished(quint64 generation, bool completed,
                                       std::shared_ptr<const model::AnalysisResult> result) {
    if (generation != generation_)
        return;
    Q_UNUSED(result);

    // 终态到达：结算取消延迟（若这次是被取消的）
    ClearCancelPending();

    // 结果已经由 AnalysisFacade 在发出 AnalysisFinished 之前写好了（编排层的职责，
    // 见 AnalysisFacade 构造函数）。本页只负责展示，不再自己 SetResult ——
    // 以前这一步漏掉过一次（只置了 has_result_ 没写回 facade），
    // 导致问题清单/评分/报告全基于默认空结果。现在不存在"漏写"这个失败模式了。
    has_result_ = true;

    const double elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scan_start_time_)
            .count();

    {
        VE_PERF("DiagnosticsPage::Evaluate(QC 规则 + 图表)");
        Evaluate();
    }
    report_.analysis_elapsed_ms = elapsed_ms;

    // 分析耗时只在**跑完**时记：被取消的那次测的是"多久退出来"，混进平均值里
    // 会把正常样本拉低，看不出一小时视频的真实成本。
    if (completed) {
        PerfMetrics::GetInstance().RecordTiming(videoeye::kMetricAnalysisMs, elapsed_ms);
    }
    RecordMemoryMetrics();

    SetScanButtonState(false);
    progress_bar_->setValue(100);
    progress_bar_->setFormat(completed ? tr("分析完成") : tr("已取消（结果不完整）"));

    // 扫描拿到结果后恢复报告导出按钮: 之前只在扫描开始禁用、完成时漏恢复,
    // 导致导出按钮永远处于禁用态。completed 与否都允许导出(已取消时也保留了部分结果)。
    export_button_->setEnabled(has_result_);

    // 终态: 一次扫描恰好发一次。共用同一次扫描的几页据此一起恢复按钮 / 进度。
    emit ScanEnded(completed ? ScanEndReason::Completed : ScanEndReason::Cancelled);
}

void DiagnosticsPage::OnFacadeFailed(quint64 generation, const QString& message) {
    if (generation != generation_)
        return;
    // 失败同样是终态：取消看门狗必须一起停，否则它会一直每秒刷进度条
    ClearCancelPending();
    SetScanButtonState(false);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("失败"));
    summary_label_->setText(message);
    last_error_ = message;

    // 不再弹模态框: 失败原因已写入上方汇总标签, 在本页直接可见
    // (异常文件打开时也会自动触发扫描, 弹窗会打断"打开即分析"的流程)。

    // 失败同样是**终态**, 必须发出去 —— 以前这里什么都不发, 于是共用同一次扫描的
    // 几页永远停在扫描态: 取消按钮还亮着、"开始分析"按钮永久禁用, 与诊断页显示的
    // "失败"互相矛盾, 用户只能靠重开文件才恢复。
    emit ScanEnded(ScanEndReason::Failed);
}

// ---------------------------------------------------------------------------
// 报告重算（规则变更 / 关联场景切换都会走这里）
// ---------------------------------------------------------------------------

void DiagnosticsPage::Evaluate() {
    if (!has_result_)
        return;
    // 同一份快照既用来算报告，也分发给各子视图 —— 不再各自拷一份。
    const std::shared_ptr<const model::AnalysisResult> result = facade_->result();
    report_ = facade_->Evaluate(*result);
    issue_view_->SetReport(*result, report_);
    coverage_view_->SetResult(result.get());
    parameter_view_->SetResult(result.get());
    frame_order_view_->SetResult(result.get());
    UpdateQcSummary();
    emit QcReportChanged(report_);
}

void DiagnosticsPage::ApplySceneLink(const std::vector<model::SceneChangeResult>& records,
                                     const videoeye::BitrateGopOptions& gop_options) {
    if (!has_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先在本页点击「开始分析」完成一次扫描。"));
        return;
    }
    if (records.empty()) {
        QMessageBox::information(this, tr("提示"),
                                 tr("当前没有场景切换数据。请先在「场景切换」页开启分析并播放一段视频。"));
        return;
    }
    facade_->ApplySceneChanges(records, gop_options);
    Evaluate();
}

void DiagnosticsPage::UpdateQcSummary() {
    if (!summary_label_)
        return;
    if (!has_result_) {
        summary_label_->setText(tr("点击「开始分析」对当前文件做一次完整扫描，将按内置 QC 规则输出问题清单与评分。"));
        return;
    }

    const int critical = report_.CountBySeverity(model::IssueSeverity::Critical);
    const int error = report_.CountBySeverity(model::IssueSeverity::Error);
    const int warning = report_.CountBySeverity(model::IssueSeverity::Warning);
    const int info = report_.CountBySeverity(model::IssueSeverity::Info);
    const model::AnalysisResult& result = *facade_->result();

    summary_label_->setText(
        tr("评分 <b>%1</b>/100（%2）｜ 致命 %3 · 错误 %4 · 警告 %5 · 提示 %6<br>"
           "容器 %7 ｜ 时长 %8 s ｜ 平均码率 %9 kbps ｜ 视频流 %10 · 音频流 %11 ｜ 包 %12 ｜ 关键帧 %13")
            .arg(QString::number(report_.score, 'f', 1))
            .arg(QString::fromStdString(report_.verdict))
            .arg(critical)
            .arg(error)
            .arg(warning)
            .arg(info)
            .arg(QString::fromStdString(result.container_format))
            .arg(QString::number(result.duration_seconds, 'f', 3))
            .arg(result.overall_bitrate_bps / 1000)
            .arg(result.VideoStreamCount())
            .arg(result.AudioStreamCount())
            .arg(static_cast<qlonglong>(result.total_packets))
            .arg(static_cast<qlonglong>(result.key_frame_count)));
}

void DiagnosticsPage::OnExportClicked() {
    if (!has_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先执行一次分析。"));
        return;
    }

    const QString default_name =
        QString::fromStdString(report_.file_name.empty() ? std::string("diagnostics") : report_.file_name) +
        "_diagnostics.html";
    const QString filename =
        QFileDialog::getSaveFileName(this, tr("导出诊断报告"), default_name,
                                     tr("HTML 报告 (*.html);;JSON 报告 (*.json);;CSV 报告 (*.csv);;文本报告 (*.txt)"));
    if (filename.isEmpty())
        return;

    bool exported = false;
    {
        // 只量"写文件"这一段：文件选择对话框是人操作，不该算进导出耗时
        VE_METRIC_SCOPE(videoeye::kMetricReportExportMs, videoeye::kReportExportBudgetMs);
        exported = reporting::QcReportExporter::ExportReport(filename.toStdString(), report_);
    }
    RecordMemoryMetrics();

    if (exported) {
        QMessageBox::information(this, tr("成功"), tr("诊断报告已导出到:\n%1").arg(filename));
    } else {
        QMessageBox::warning(this, tr("失败"), tr("导出诊断报告失败。"));
    }
}

// ---------------------------------------------------------------------------
// 只读快照（面板分发结果给共用同一次扫描的其它页）
// ---------------------------------------------------------------------------

void DiagnosticsPage::ResetForNewFile() {
    has_result_ = false;
    issue_view_->Clear();
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("未开始"));
    summary_label_->setText(tr("点击「开始分析」对当前文件做一次完整扫描，"
                               "将按内置 QC 规则输出问题清单与评分。"));
    // 证据表与参数表也是"上一文件"的结果，必须一起清 —— 留着会让用户拿旧文件的
    // 覆盖结论去读新文件的问题清单。
    coverage_view_->SetResult(nullptr);
    parameter_view_->SetResult(nullptr);
    frame_order_view_->SetResult(nullptr);
    export_button_->setEnabled(false);
}

const std::vector<model::QcRule>& DiagnosticsPage::rules() const {
    return facade_->rules();
}

std::shared_ptr<const model::AnalysisResult> DiagnosticsPage::result() const {
    return facade_->result();
}

} // namespace ui
} // namespace videoeye
