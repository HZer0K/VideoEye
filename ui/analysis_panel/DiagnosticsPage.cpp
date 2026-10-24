#include "ui/analysis_panel/DiagnosticsPage.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

#include "core/reporting/QcReportExporter.h"
#include "infrastructure/logging/PerfMetrics.h"
#include "infrastructure/logging/ScopedTimer.h"

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

// 曲线点数上限不再本地定义：统一走 ui/charts/ChartDecimation.h 的 kMaxChartPoints。
// 这里以前有一份同名常量（4000），与统一常量处在同一命名空间层级，
// 名字查找会直接歧义 —— 更要紧的是"多个页面各定一把尺子"本身就是本轮要消除的问题。

DiagnosticsPage::DiagnosticsPage(QWidget* parent) : QWidget(parent) {
    // ⚠️ facade_ 必须**先于** SetupUi() 建好。
    //
    // SetupUi() 最后一步是 RebuildRuleTable()，而规则表的来源就是 facade_->rules()。
    // 这里以前是反的（先 SetupUi() 再 new AnalysisFacade），于是 RebuildRuleTable()
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

    // ---- 子页 0: 问题清单 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        qc_chart_ = new MetricChartWidget(page);
        qc_chart_->SetTitle(tr("逐秒码率 / 帧率"));
        qc_bitrate_series_ = qc_chart_->AddLineSeries(tr("码率 (kbps)"), QColor("#1e88e5"));
        qc_fps_series_ = qc_chart_->AddLineSeries(tr("帧率 (fps)"), QColor("#43a047"));
        qc_axis_x_ = qc_chart_->AxisX();
        qc_axis_bitrate_ = qc_chart_->AxisY(); // 左轴
        qc_axis_fps_ = qc_chart_->AxisY2();    // 右轴
        qc_chart_->SetAxisY2Visible(true);
        qc_chart_->AttachAxis(qc_fps_series_, qc_axis_fps_);
        qc_axis_x_->SetTitleText(tr("时间 (s)"));
        qc_axis_bitrate_->SetTitleText(tr("kbps"));
        qc_axis_fps_->SetTitleText(tr("fps"));
        qc_chart_->setMinimumHeight(220);
        pl->addWidget(qc_chart_);

        issue_table_ = new QTableWidget(0, 6, page);
        issue_table_->setHorizontalHeaderLabels(
            {tr("严重度"), tr("类别"), tr("问题"), tr("位置"), tr("说明"), tr("建议")});
        issue_table_->verticalHeader()->setVisible(false);
        issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        issue_table_->horizontalHeader()->setStretchLastSection(true);
        issue_table_->setMinimumHeight(200);
        pl->addWidget(issue_table_);

        // 跳转是按问题清单里选中那一行的位置 seek；时间轴的样本/曲线/导出已收敛到
        // 「事件与时间轴」页，本页只保留"问题 -> 跳转"这一条动作。
        QWidget* jump_row = new QWidget(page);
        QHBoxLayout* jl = new QHBoxLayout(jump_row);
        jl->setContentsMargins(0, 0, 0, 0);
        jl->addStretch();
        QPushButton* jump_button = new QPushButton(tr("跳转到问题位置"), jump_row);
        jump_button->setToolTip(tr("按当前选中问题的位置执行 seek"));
        connect(jump_button, &QPushButton::clicked, this, &DiagnosticsPage::OnJumpToIssue);
        jl->addWidget(jump_button);
        pl->addWidget(jump_row);

        sub_tabs_->addTab(page, tr("问题清单"));
    }

    // ---- 子页 1: 规则与阈值 ----
    SetupRuleTab();

    // ---- 子页 2/3: 证据与覆盖 / 参数变化 ----
    SetupEvidenceTabs();

    layout->addWidget(sub_tabs_);

    // 规则表初始内容
    RebuildRuleTable();
}

void DiagnosticsPage::SetupRuleTab() {
    QWidget* page = new QWidget(sub_tabs_);
    QVBoxLayout* pl = new QVBoxLayout(page);
    pl->setContentsMargins(2, 2, 2, 2);

    QLabel* hint =
        new QLabel(tr("勾选启用列可开关规则，双击阈值可直接修改；修改后会立即用当前扫描结果重算报告。"), page);
    hint->setWordWrap(true);
    pl->addWidget(hint);

    rule_table_ = new QTableWidget(0, 6, page);
    rule_table_->setHorizontalHeaderLabels({tr("启用"), tr("规则"), tr("类别"), tr("严重度"), tr("判定"), tr("阈值")});
    rule_table_->verticalHeader()->setVisible(false);
    rule_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rule_table_->horizontalHeader()->setStretchLastSection(true);
    pl->addWidget(rule_table_);
    connect(rule_table_, &QTableWidget::itemChanged, this, &DiagnosticsPage::OnRuleItemChanged);

    QPushButton* reset_btn = new QPushButton(tr("恢复默认规则"), page);
    connect(reset_btn, &QPushButton::clicked, this, &DiagnosticsPage::OnResetRulesClicked);
    pl->addWidget(reset_btn, 0, Qt::AlignRight);

    sub_tabs_->addTab(page, tr("规则与阈值"));
}

void DiagnosticsPage::SetupEvidenceTabs() {
    // ---- 子页 2: 证据与覆盖 ----
    //
    // 这一页要回答的问题不是"有没有问题"，而是"报告里的结论覆盖了多少内容、
    // 没覆盖的部分为什么没覆盖"。所以列全部维度（含未执行的），不做过滤 ——
    // 只列异常的表格没法证明"没漏"，而那正是这张表的用途。
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        dimension_hint_ =
            new QLabel(tr("「结论可用」是唯一权威口径：只有该维度确实执行、能力完整、无失败且覆盖达标时才为「是」。"
                          "未执行的行会写明原因，据此判断报告里的「未发现问题」是「真的没问题」还是「没证据」。"),
                       page);
        dimension_hint_->setWordWrap(true);
        pl->addWidget(dimension_hint_);

        dimension_table_ = new QTableWidget(0, 8, page);
        dimension_table_->setHorizontalHeaderLabels({tr("维度"), tr("状态"), tr("能力"), tr("结论可用"), tr("处理对象"),
                                                     tr("扫描 / 总量"), tr("覆盖率"), tr("说明")});
        dimension_table_->verticalHeader()->setVisible(false);
        dimension_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        dimension_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        dimension_table_->horizontalHeader()->setStretchLastSection(true);
        pl->addWidget(dimension_table_);

        sub_tabs_->addTab(page, tr("证据与覆盖"));
    }

    // ---- 子页 3: 参数变化 ----
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        parameter_hint_ = new QLabel(
            tr("按时间顺序列出 SPS/PPS/VPS/AV1 序列头的配置变化。"
               "同一参数集 id 的内容被替换（标红）意味着前后帧不是同一套配置，是拼接或编码器重启的直接证据。"),
            page);
        parameter_hint_->setWordWrap(true);
        pl->addWidget(parameter_hint_);

        parameter_table_ = new QTableWidget(0, 7, page);
        parameter_table_->setHorizontalHeaderLabels(
            {tr("流"), tr("时间"), tr("参数集"), tr("变化前"), tr("变化后"), tr("包号 / 偏移"), tr("说明")});
        parameter_table_->verticalHeader()->setVisible(false);
        parameter_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        parameter_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        parameter_table_->horizontalHeader()->setStretchLastSection(true);
        pl->addWidget(parameter_table_);

        sub_tabs_->addTab(page, tr("参数变化"));
    }

    // ---- 子页 4: 帧顺序 ----
    //
    // 与「参数变化」的区别：那条记"配置怎么变"，这条记"帧怎么排"。
    // 表按**解码顺序**逐帧一行，POC 列可以直接看出显示顺序 —— POC 比上一行小的
    // 那一行就是显示时要往回插的帧（标红）。这是重排/GOP/缺帧最直观的呈现。
    {
        QWidget* page = new QWidget(sub_tabs_);
        QVBoxLayout* pl = new QVBoxLayout(page);
        pl->setContentsMargins(2, 2, 2, 2);

        frame_order_hint_ = new QLabel(tr("按解码顺序列出每一帧的 POC（显示顺序值）。POC 比上一行小的行标红，"
                                          "那是显示顺序要往回插的帧 —— 重排的存在与深度因此一目了然。"
                                          "支持范围：仅 H.264；pic_order_cnt_type=1 与 HEVC/AV1/VP9/VVC 不支持；"
                                          "不重建参考帧列表。"),
                                       page);
        frame_order_hint_->setWordWrap(true);
        pl->addWidget(frame_order_hint_);

        frame_order_table_ = new QTableWidget(0, 6, page);
        frame_order_table_->setHorizontalHeaderLabels(
            {tr("流"), tr("解码序"), tr("类型"), tr("frame_num"), tr("POC"), tr("包号 / 时间")});
        frame_order_table_->verticalHeader()->setVisible(false);
        frame_order_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        frame_order_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        frame_order_table_->horizontalHeader()->setStretchLastSection(true);
        pl->addWidget(frame_order_table_);

        sub_tabs_->addTab(page, tr("帧顺序"));
    }
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
    issue_table_->setRowCount(0);
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

void DiagnosticsPage::OnFacadeFinished(quint64 generation, bool completed, const model::AnalysisResult& result) {
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
    report_ = facade_->Evaluate(facade_->result());
    RebuildIssueTable();
    RebuildEvidenceTables();
    UpdateQcChart();
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

void DiagnosticsPage::RebuildIssueTable() {
    if (!issue_table_)
        return;
    issue_table_->setRowCount(0);
    issue_table_->setRowCount(static_cast<int>(report_.issues.size()));

    for (int i = 0; i < static_cast<int>(report_.issues.size()); ++i) {
        const auto& issue = report_.issues[i];
        SetTableItemText(issue_table_, i, 0, QString::fromStdString(issue.SeverityText()));
        SetTableItemText(issue_table_, i, 1, QString::fromStdString(issue.CategoryText()));
        SetTableItemText(issue_table_, i, 2, QString::fromStdString(issue.title));
        SetTableItemText(issue_table_, i, 3, QString::fromStdString(issue.range.ToString()));
        SetTableItemText(issue_table_, i, 4, QString::fromStdString(issue.detail));
        SetTableItemText(issue_table_, i, 5, QString::fromStdString(issue.suggestion));

        QColor color = QColor("#1565c0");
        switch (issue.severity) {
        case model::IssueSeverity::Critical:
            color = QColor("#c62828");
            break;
        case model::IssueSeverity::Error:
            color = QColor("#e53935");
            break;
        case model::IssueSeverity::Warning:
            color = QColor("#ef6c00");
            break;
        case model::IssueSeverity::Info:
            color = QColor("#1565c0");
            break;
        }
        if (QTableWidgetItem* cell = issue_table_->item(i, 0)) {
            cell->setForeground(color);
        }
    }
    issue_table_->resizeColumnsToContents();
}

void DiagnosticsPage::RebuildEvidenceTables() {
    const model::AnalysisResult* result = has_result_ ? &facade_->result() : nullptr;

    // ---- 证据与覆盖 ----
    if (dimension_table_) {
        dimension_table_->setRowCount(0);
        if (result == nullptr) {
            if (dimension_hint_)
                dimension_hint_->setText(tr("尚无扫描结果。点击「开始分析」后可查看每个维度的覆盖情况。"));
        } else {
            if (dimension_hint_)
                dimension_hint_->setText(
                    tr("「结论可用」是唯一权威口径：只有该维度确实执行、能力完整、无失败且覆盖达标时才为「是」。"
                       "未执行的行会写明原因，据此判断报告里的「未发现问题」是「真的没问题」还是「没证据」。"));
            const auto& dimensions = result->dimensions;
            dimension_table_->setRowCount(static_cast<int>(dimensions.size()));
            for (int i = 0; i < static_cast<int>(dimensions.size()); ++i) {
                const model::DimensionStatus& status = dimensions[i];
                SetTableItemText(dimension_table_, i, 0,
                                 QString::fromStdString(model::DimensionName(status.dimension)));
                SetTableItemText(dimension_table_, i, 1, status.executed ? tr("已执行") : tr("未执行"));
                SetTableItemText(dimension_table_, i, 2,
                                 QString::fromStdString(model::CapabilityToString(status.capability)));
                SetTableItemText(dimension_table_, i, 3, status.conclusive ? tr("是") : tr("否"));
                SetTableItemText(dimension_table_, i, 4,
                                 status.scope_unit.empty() ? QStringLiteral("-")
                                                           : QString::fromStdString(status.scope_unit));
                // 总量未知时写"未知"而不是"0"：0 会被读成"一个都没扫到"
                const QString units = QString::number(static_cast<qlonglong>(status.units_scanned)) + " / " +
                                      (status.has_coverage ? QString::number(static_cast<qlonglong>(status.units_total))
                                                           : QStringLiteral("未知"));
                SetTableItemText(dimension_table_, i, 5, units);
                SetTableItemText(dimension_table_, i, 6,
                                 status.has_coverage ? QString::number(status.CoveragePercent(), 'f', 1) + " %"
                                                     : QStringLiteral("-"));
                QString remark = QString::fromStdString(status.failure_reason);
                for (const std::string& note : status.notes) {
                    if (!remark.isEmpty())
                        remark += QStringLiteral("；");
                    remark += QString::fromStdString(note);
                }
                if (status.failures > 0) {
                    if (!remark.isEmpty())
                        remark += QStringLiteral("；");
                    remark += tr("失败 %1").arg(static_cast<qlonglong>(status.failures));
                }
                SetTableItemText(dimension_table_, i, 7, remark);

                // 未执行 / 失败的行整行标色：这两类最容易被误读成"通过"
                QColor color;
                if (!status.executed)
                    color = QColor("#9e9e9e");
                else if (status.capability == model::AnalysisCapability::Failed)
                    color = QColor("#c62828");
                else if (status.capability == model::AnalysisCapability::Partial)
                    color = QColor("#ef6c00");
                if (color.isValid()) {
                    for (int c = 0; c < dimension_table_->columnCount(); ++c) {
                        if (QTableWidgetItem* cell = dimension_table_->item(i, c))
                            cell->setForeground(color);
                    }
                }
            }
            dimension_table_->resizeColumnsToContents();
        }
    }

    // ---- 参数变化 ----
    if (parameter_table_) {
        parameter_table_->setRowCount(0);
        if (result == nullptr) {
            if (parameter_hint_)
                parameter_hint_->setText(tr("尚无扫描结果。"));
        } else if (result->parameter_timelines.empty()) {
            if (parameter_hint_)
                parameter_hint_->setText(
                    tr("本次未追踪参数变化（没有 H.264/HEVC/AV1 视频轨，或「参数变化追踪」选项已关闭）。"));
        } else {
            // 先把所有轨的变化事件摊平，再一次性填表 —— 一条轨没有变化也要留一行
            // 「未检测到配置变化」，否则"这条轨没变"和"这条轨没扫"在界面上分不出来。
            struct Row {
                int stream_index = -1;
                QString time;
                QString source;
                QString old_config;
                QString new_config;
                QString position;
                QString note;
                bool replaced_same_id = false;
            };
            std::vector<Row> rows;
            int tracked_streams = 0;
            int truncated_streams = 0;
            int64_t missing_refs = 0;
            for (const model::ParameterTimeline& timeline : result->parameter_timelines) {
                ++tracked_streams;
                if (timeline.truncated)
                    ++truncated_streams;
                missing_refs += timeline.missing_reference_count;
                if (timeline.changes.empty()) {
                    Row row;
                    row.stream_index = timeline.stream_index;
                    row.time = QStringLiteral("-");
                    row.source = QString::fromStdString(timeline.codec_name);
                    row.note =
                        tr("未检测到配置变化（当前配置：%1）").arg(QString::fromStdString(timeline.ActiveSummary()));
                    rows.push_back(row);
                    continue;
                }
                for (const model::ParameterChangeEvent& event : timeline.changes) {
                    Row row;
                    row.stream_index = event.stream_index >= 0 ? event.stream_index : timeline.stream_index;
                    // 无 PTS 时不能写 0.000 s —— 那会被读成"在文件开头"
                    row.time = event.has_time ? QString::number(event.time_seconds, 'f', 3) + " s" : tr("无 PTS");
                    row.source = QString::fromStdString(event.source);
                    row.old_config = QString::fromStdString(event.old_config);
                    row.new_config = QString::fromStdString(event.new_config);
                    QString position = tr("包 %1").arg(static_cast<qlonglong>(event.packet_index));
                    if (event.file_offset >= 0)
                        position += QStringLiteral(" / 0x") +
                                    QString::number(static_cast<qlonglong>(event.file_offset), 16).toUpper();
                    row.position = position;
                    row.note = QString::fromStdString(event.changes);
                    row.replaced_same_id = event.replaced_same_id;
                    rows.push_back(row);
                }
            }

            QString hint =
                tr("追踪 %1 条视频轨，共 %2 次配置变化。").arg(tracked_streams).arg(static_cast<int>(rows.size()));
            if (missing_refs > 0)
                hint +=
                    tr(" 有 %1 处引用了从未传输的参数集（解码必然失败）。").arg(static_cast<qlonglong>(missing_refs));
            if (truncated_streams > 0)
                hint += tr(" 有 %1 条轨已达追踪上限，后面的变化未记录。").arg(truncated_streams);
            if (parameter_hint_)
                parameter_hint_->setText(hint);

            parameter_table_->setRowCount(static_cast<int>(rows.size()));
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                const Row& row = rows[i];
                SetTableItemText(parameter_table_, i, 0, QString::number(row.stream_index));
                SetTableItemText(parameter_table_, i, 1, row.time);
                SetTableItemText(parameter_table_, i, 2, row.source);
                SetTableItemText(parameter_table_, i, 3,
                                 row.old_config.isEmpty() ? QStringLiteral("-") : row.old_config);
                SetTableItemText(parameter_table_, i, 4,
                                 row.new_config.isEmpty() ? QStringLiteral("-") : row.new_config);
                SetTableItemText(parameter_table_, i, 5, row.position);
                SetTableItemText(parameter_table_, i, 6,
                                 row.replaced_same_id
                                     ? tr("%1 ｜ 同一 id 内容被替换：前后帧不是同一套配置").arg(row.note)
                                     : row.note);
                if (row.replaced_same_id) {
                    for (int c = 0; c < parameter_table_->columnCount(); ++c) {
                        if (QTableWidgetItem* cell = parameter_table_->item(i, c))
                            cell->setForeground(QColor("#c62828"));
                    }
                }
            }
            parameter_table_->resizeColumnsToContents();
        }
    }

    // ---- 帧顺序（POC）----
    if (frame_order_table_) {
        frame_order_table_->setRowCount(0);
        if (result == nullptr) {
            if (frame_order_hint_)
                frame_order_hint_->setText(tr("尚无扫描结果。"));
        } else if (result->frame_order_timelines.empty()) {
            if (frame_order_hint_)
                frame_order_hint_->setText(tr("本次未追踪帧顺序（没有 H.264 视频轨，或「帧级顺序追踪」选项已关闭）。"));
        } else {
            // 与参数变化表同理：每条轨至少留一行，不可用的轨留「不可用 + 原因」，
            // 否则"没有 H.264 轨"和"有轨但算不出 POC"在界面上长得一样。
            struct Row {
                int stream_index = -1;
                QString decode_index;
                QString type;
                QString frame_num;
                QString poc;
                QString position;
                bool reorder = false;
                bool abnormal = false;
            };
            std::vector<Row> rows;
            int tracked = 0;
            int unavailable = 0;
            for (const model::FrameOrderTimeline& timeline : result->frame_order_timelines) {
                ++tracked;
                if (!timeline.available) {
                    ++unavailable;
                    Row row;
                    row.stream_index = timeline.stream_index;
                    row.decode_index = QStringLiteral("-");
                    row.type = tr("不可用");
                    row.frame_num = QStringLiteral("-");
                    row.poc = QString::fromStdString(timeline.unavailable_reason);
                    row.position = QString::fromStdString(timeline.Summary());
                    row.abnormal = true;
                    rows.push_back(row);
                    continue;
                }
                for (std::size_t i = 0; i < timeline.frames.size(); ++i) {
                    const model::FrameOrderEntry& entry = timeline.frames[i];
                    Row row;
                    row.stream_index = timeline.stream_index;
                    row.decode_index = QString::number(static_cast<qulonglong>(i));
                    const char type_char = entry.TypeChar();
                    row.type = QString::fromLatin1(&type_char, 1) + (entry.idr ? QStringLiteral("(IDR)") : QString());
                    row.frame_num = QString::number(entry.frame_num);
                    row.poc = QString::number(static_cast<qlonglong>(entry.poc));
                    row.position = tr("包 %1").arg(static_cast<qlonglong>(entry.packet_index));
                    if (entry.has_time)
                        row.position += QStringLiteral(" / ") + QString::number(entry.time_seconds, 'f', 3) + " s";
                    row.reorder = i > 0 && entry.poc < timeline.frames[i - 1].poc;
                    rows.push_back(row);
                }
            }

            QString hint =
                tr("追踪 %1 条视频轨，共列出 %2 帧（解码顺序）。").arg(tracked).arg(static_cast<int>(rows.size()));
            if (unavailable > 0)
                hint += tr(" 其中 %1 条轨不可用，原因见表内。").arg(unavailable);
            if (frame_order_hint_)
                frame_order_hint_->setText(hint);

            frame_order_table_->setRowCount(static_cast<int>(rows.size()));
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                const Row& row = rows[i];
                SetTableItemText(frame_order_table_, i, 0, QString::number(row.stream_index));
                SetTableItemText(frame_order_table_, i, 1, row.decode_index);
                SetTableItemText(frame_order_table_, i, 2, row.type);
                SetTableItemText(frame_order_table_, i, 3, row.frame_num);
                SetTableItemText(frame_order_table_, i, 4, row.poc);
                SetTableItemText(frame_order_table_, i, 5, row.position);
                // 重排（POC 回退）是正常编码结构，用橙色提示而非红色；
                // 红色留给"这条轨算不出 POC"这种真的没结论的情况。
                if (row.abnormal) {
                    for (int c = 0; c < frame_order_table_->columnCount(); ++c) {
                        if (QTableWidgetItem* cell = frame_order_table_->item(i, c))
                            cell->setForeground(QColor("#c62828"));
                    }
                } else if (row.reorder) {
                    for (int c = 0; c < frame_order_table_->columnCount(); ++c) {
                        if (QTableWidgetItem* cell = frame_order_table_->item(i, c))
                            cell->setForeground(QColor("#ef6c00"));
                    }
                }
            }
            frame_order_table_->resizeColumnsToContents();
        }
    }
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
    const auto& result = facade_->result();

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

void DiagnosticsPage::UpdateQcChart() {
    if (!qc_bitrate_series_ || !has_result_)
        return;
    qc_bitrate_series_->Clear();
    qc_fps_series_->Clear();

    const auto& result = facade_->result();
    const auto& bitrate = result.video_bitrate_kbps.IsEmpty() ? result.total_bitrate_kbps : result.video_bitrate_kbps;
    // 抽稀后再提交：长时间分析的采样点上万，全量喂给自绘图表重绘会卡死 UI
    AppendDecimated(qc_bitrate_series_, bitrate, kMaxChartPoints);
    AppendDecimated(qc_fps_series_, result.video_fps, kMaxChartPoints);

    double max_t = 1.0;
    if (!bitrate.samples.empty())
        max_t = std::max(max_t, bitrate.samples.back().timestamp_seconds);
    if (!result.video_fps.samples.empty()) {
        max_t = std::max(max_t, result.video_fps.samples.back().timestamp_seconds);
    }
    qc_axis_x_->SetRange(0, max_t);
    qc_axis_bitrate_->SetRange(0, std::max(1.0, bitrate.Max() * 1.2));
    qc_axis_fps_->SetRange(0, std::max(1.0, result.video_fps.Max() * 1.2));
}

void DiagnosticsPage::RebuildRuleTable() {
    if (!rule_table_)
        return;
    rule_table_updating_ = true;
    rule_table_->setRowCount(0);

    const auto& rules = facade_->rules();
    rule_table_->setRowCount(static_cast<int>(rules.size()));
    for (int i = 0; i < static_cast<int>(rules.size()); ++i) {
        const auto& rule = rules[i];

        QTableWidgetItem* enable_item = new QTableWidgetItem();
        enable_item->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);
        enable_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        rule_table_->setItem(i, 0, enable_item);

        QTableWidgetItem* name_item = new QTableWidgetItem(QString::fromStdString(rule.name));
        name_item->setToolTip(QString::fromStdString(rule.description));
        name_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        rule_table_->setItem(i, 1, name_item);

        SetTableItemText(rule_table_, i, 2, QString::fromStdString(ToString(rule.category)));
        SetTableItemText(rule_table_, i, 3, QString::fromStdString(ToString(rule.severity)));
        SetTableItemText(rule_table_, i, 4, QString::fromStdString(ToString(rule.op)));

        QTableWidgetItem* threshold_item =
            new QTableWidgetItem(QString::number(rule.threshold, 'f', 2) + QString::fromStdString(rule.unit));
        threshold_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        threshold_item->setToolTip(tr("双击修改阈值（仅需填写数值部分）"));
        rule_table_->setItem(i, 5, threshold_item);
    }
    rule_table_->resizeColumnsToContents();
    rule_table_updating_ = false;
}

void DiagnosticsPage::OnRuleItemChanged(QTableWidgetItem* item) {
    if (!item || rule_table_updating_)
        return;
    const QString rule_id = item->data(Qt::UserRole).toString();
    if (rule_id.isEmpty())
        return;

    model::QcRule* rule = model::FindQcRule(facade_->rules(), rule_id.toStdString());
    if (!rule)
        return;

    if (item->column() == 0) {
        rule->enabled = (item->checkState() == Qt::Checked);
    } else if (item->column() == 5) {
        // 允许 "12.5s" / "12.5" 这类输入，取前导数字部分
        const QString text = item->text().trimmed();
        int end = 0;
        while (end < text.size() && (text.at(end).isDigit() || text.at(end) == QLatin1Char('.') ||
                                     text.at(end) == QLatin1Char('-') || text.at(end) == QLatin1Char('+'))) {
            ++end;
        }
        bool ok = false;
        const double value = text.left(end).toDouble(&ok);
        if (!ok) {
            // 还原显示
            rule_table_updating_ = true;
            item->setText(QString::number(rule->threshold, 'f', 2) + QString::fromStdString(rule->unit));
            rule_table_updating_ = false;
            return;
        }
        rule->threshold = value;
    } else {
        return;
    }

    if (has_result_)
        Evaluate();
}

void DiagnosticsPage::OnResetRulesClicked() {
    facade_->SetRules(model::DefaultQcRules());
    RebuildRuleTable();
    if (has_result_)
        Evaluate();
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
// 跳转到问题位置（问题清单里选中那一行的位置）
// ---------------------------------------------------------------------------

void DiagnosticsPage::OnJumpToIssue() {
    if (!issue_table_)
        return;
    const int row = issue_table_->currentRow();
    if (row < 0 || row >= static_cast<int>(report_.issues.size())) {
        QMessageBox::information(this, tr("提示"), tr("请先在问题列表中选择一条记录。"));
        return;
    }
    const double seconds = report_.issues[row].range.start_seconds;
    if (seconds < 0.0) {
        QMessageBox::information(this, tr("提示"), tr("该问题为文件级问题，没有可跳转的时间点。"));
        return;
    }
    emit SeekRequested(seconds);
}

// ---------------------------------------------------------------------------
// 只读快照（面板分发结果给共用同一次扫描的其它页）
// ---------------------------------------------------------------------------

void DiagnosticsPage::ResetForNewFile() {
    has_result_ = false;
    issue_table_->setRowCount(0);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("未开始"));
    summary_label_->setText(tr("点击「开始分析」对当前文件做一次完整扫描，"
                               "将按内置 QC 规则输出问题清单与评分。"));
    qc_bitrate_series_->Clear();
    qc_fps_series_->Clear();
    // 证据表与参数表也是"上一文件"的结果，必须一起清 —— 留着会让用户拿旧文件的
    // 覆盖结论去读新文件的问题清单。
    RebuildEvidenceTables();
    export_button_->setEnabled(false);
}

const std::vector<model::QcRule>& DiagnosticsPage::rules() const {
    return facade_->rules();
}

const model::AnalysisResult& DiagnosticsPage::result() const {
    return facade_->result();
}

} // namespace ui
} // namespace videoeye
