#include "ui/analysis_panel/DiagnosticsPage.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

#include "core/reporting/QcReportExporter.h"
#include "infrastructure/logging/ScopedTimer.h"

namespace videoeye {
namespace ui {

namespace {
// 自绘曲线一次最多喂多少点。长片分析 pts 轻松上万，全量喂进去 ComputeGeometry
// 每帧都要扫一遍点集，重绘直接把 UI 卡住；超过就按组抽稀（每组取最大值）。
constexpr int kMaxChartPoints = 4000;
}  // namespace

DiagnosticsPage::DiagnosticsPage(QWidget* parent)
    : QWidget(parent) {
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
    connect(facade_, &AnalysisFacade::ProgressReported,
            this, &DiagnosticsPage::OnFacadeProgress);
    connect(facade_, &AnalysisFacade::AnalysisFinished,
            this, &DiagnosticsPage::OnFacadeFinished);
    connect(facade_, &AnalysisFacade::AnalysisFailed,
            this, &DiagnosticsPage::OnFacadeFailed);

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
        start_button_->setToolTip(
            tr("对当前文件做一次完整 demux 扫描并按规则生成诊断报告"));
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

    summary_label_ = new QLabel(
        tr("点击「开始分析」对当前文件做一次完整扫描，将按内置 QC 规则输出问题清单与评分。"));
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
        qc_axis_bitrate_ = qc_chart_->AxisY();        // 左轴
        qc_axis_fps_ = qc_chart_->AxisY2();           // 右轴
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

    layout->addWidget(sub_tabs_);

    // 规则表初始内容
    RebuildRuleTable();
}

void DiagnosticsPage::SetupRuleTab() {
    QWidget* page = new QWidget(sub_tabs_);
    QVBoxLayout* pl = new QVBoxLayout(page);
    pl->setContentsMargins(2, 2, 2, 2);

    QLabel* hint = new QLabel(
        tr("勾选启用列可开关规则，双击阈值可直接修改；修改后会立即用当前扫描结果重算报告。"),
        page);
    hint->setWordWrap(true);
    pl->addWidget(hint);

    rule_table_ = new QTableWidget(0, 6, page);
    rule_table_->setHorizontalHeaderLabels(
        {tr("启用"), tr("规则"), tr("类别"), tr("严重度"), tr("判定"), tr("阈值")});
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

// ---------------------------------------------------------------------------
// 全文件扫描
// ---------------------------------------------------------------------------

void DiagnosticsPage::StartScan(const videoeye::AnalysisOptions& options, bool silent) {
    // 静默模式（打开失败自动补扫等）不弹模态框：这是流程里的必经一步，
    // 弹窗会打断流程、扫到一半卡在模态循环里等不到人点。条件不满足就悄悄放弃。
    if (source_path_.isEmpty()) {
        if (!silent) QMessageBox::information(this, tr("提示"), tr("请先打开一个媒体文件。"));
        return;
    }
    if (facade_->IsRunning()) {
        if (!silent) QMessageBox::information(this, tr("提示"), tr("分析正在进行中。"));
        return;
    }

    scan_start_time_ = std::chrono::steady_clock::now();
    has_result_ = false;
    issue_table_->setRowCount(0);
    export_button_->setEnabled(false);
    SetScanButtonState(true);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("准备中 %p%"));
    summary_label_->setText(tr("正在扫描: %1").arg(source_path_));

    emit ScanStarted();

    // 字幕阈值同步之类的跨页逻辑由面板在扫描前注入
    videoeye::AnalysisOptions effective = options;
    if (before_scan_) before_scan_(effective);

    generation_ = facade_->StartAnalysis(source_path_.toStdString(), effective);
}

void DiagnosticsPage::CancelScan() {
    facade_->Cancel();
    cancel_button_->setEnabled(false);
    progress_bar_->setFormat(tr("取消中..."));
    // 注意这里**不发终态信号**: 取消只是请求, 任务还在跑。终态由 OnFacadeFinished
    // 统一发出 —— 共用同一次扫描的几页在真正结束时才一起回到 Idle, 不会出现
    // "面板说取消了、后台其实还在跑"的中间态。
}

void DiagnosticsPage::SetScanButtonState(bool running) {
    start_button_->setEnabled(!running);
    cancel_button_->setEnabled(running);
}

void DiagnosticsPage::OnCancelClicked() { CancelScan(); }

void DiagnosticsPage::OnFacadeProgress(quint64 generation, double percent, const QString& stage) {
    if (generation != generation_) return;   // 旧任务的回调直接丢弃
    progress_bar_->setValue(static_cast<int>(percent));
    progress_bar_->setFormat(stage + " %p%");
    emit ProgressChanged(percent, stage);
}

void DiagnosticsPage::OnFacadeFinished(quint64 generation, bool completed,
                                       const model::AnalysisResult& result) {
    if (generation != generation_) return;
    Q_UNUSED(result);

    // 结果已经由 AnalysisFacade 在发出 AnalysisFinished 之前写好了（编排层的职责，
    // 见 AnalysisFacade 构造函数）。本页只负责展示，不再自己 SetResult ——
    // 以前这一步漏掉过一次（只置了 has_result_ 没写回 facade），
    // 导致问题清单/评分/报告全基于默认空结果。现在不存在"漏写"这个失败模式了。
    has_result_ = true;

    const double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - scan_start_time_)
                                  .count();

    {
        VE_PERF("DiagnosticsPage::Evaluate(QC 规则 + 图表)");
        Evaluate();
    }
    report_.analysis_elapsed_ms = elapsed_ms;

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
    if (generation != generation_) return;
    SetScanButtonState(false);
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("失败"));
    summary_label_->setText(message);

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
    if (!has_result_) return;
    report_ = facade_->Evaluate(facade_->result());
    RebuildIssueTable();
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
    if (!issue_table_) return;
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
            case model::IssueSeverity::Critical: color = QColor("#c62828"); break;
            case model::IssueSeverity::Error:    color = QColor("#e53935"); break;
            case model::IssueSeverity::Warning:  color = QColor("#ef6c00"); break;
            case model::IssueSeverity::Info:     color = QColor("#1565c0"); break;
        }
        if (QTableWidgetItem* cell = issue_table_->item(i, 0)) {
            cell->setForeground(color);
        }
    }
    issue_table_->resizeColumnsToContents();
}

void DiagnosticsPage::UpdateQcSummary() {
    if (!summary_label_) return;
    if (!has_result_) {
        summary_label_->setText(
            tr("点击「开始分析」对当前文件做一次完整扫描，将按内置 QC 规则输出问题清单与评分。"));
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
            .arg(critical).arg(error).arg(warning).arg(info)
            .arg(QString::fromStdString(result.container_format))
            .arg(QString::number(result.duration_seconds, 'f', 3))
            .arg(result.overall_bitrate_bps / 1000)
            .arg(result.VideoStreamCount())
            .arg(result.AudioStreamCount())
            .arg(static_cast<qlonglong>(result.total_packets))
            .arg(static_cast<qlonglong>(result.key_frame_count)));
}

void DiagnosticsPage::UpdateQcChart() {
    if (!qc_bitrate_series_ || !has_result_) return;
    qc_bitrate_series_->Clear();
    qc_fps_series_->Clear();

    const auto& result = facade_->result();
    const auto& bitrate = result.video_bitrate_kbps.IsEmpty()
                              ? result.total_bitrate_kbps
                              : result.video_bitrate_kbps;
    // 抽稀后再提交：长时间分析的采样点上万，全量喂给自绘图表重绘会卡死 UI
    AppendDecimated(qc_bitrate_series_, bitrate, kMaxChartPoints);
    AppendDecimated(qc_fps_series_, result.video_fps, kMaxChartPoints);

    double max_t = 1.0;
    if (!bitrate.samples.empty()) max_t = std::max(max_t, bitrate.samples.back().timestamp_seconds);
    if (!result.video_fps.samples.empty()) {
        max_t = std::max(max_t, result.video_fps.samples.back().timestamp_seconds);
    }
    qc_axis_x_->SetRange(0, max_t);
    qc_axis_bitrate_->SetRange(0, std::max(1.0, bitrate.Max() * 1.2));
    qc_axis_fps_->SetRange(0, std::max(1.0, result.video_fps.Max() * 1.2));
}

void DiagnosticsPage::RebuildRuleTable() {
    if (!rule_table_) return;
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
            new QTableWidgetItem(QString::number(rule.threshold, 'f', 2) +
                                 QString::fromStdString(rule.unit));
        threshold_item->setData(Qt::UserRole, QString::fromStdString(rule.id));
        threshold_item->setToolTip(tr("双击修改阈值（仅需填写数值部分）"));
        rule_table_->setItem(i, 5, threshold_item);
    }
    rule_table_->resizeColumnsToContents();
    rule_table_updating_ = false;
}

void DiagnosticsPage::OnRuleItemChanged(QTableWidgetItem* item) {
    if (!item || rule_table_updating_) return;
    const QString rule_id = item->data(Qt::UserRole).toString();
    if (rule_id.isEmpty()) return;

    model::QcRule* rule = model::FindQcRule(facade_->rules(), rule_id.toStdString());
    if (!rule) return;

    if (item->column() == 0) {
        rule->enabled = (item->checkState() == Qt::Checked);
    } else if (item->column() == 5) {
        // 允许 "12.5s" / "12.5" 这类输入，取前导数字部分
        const QString text = item->text().trimmed();
        int end = 0;
        while (end < text.size() &&
               (text.at(end).isDigit() || text.at(end) == QLatin1Char('.') ||
                text.at(end) == QLatin1Char('-') || text.at(end) == QLatin1Char('+'))) {
            ++end;
        }
        bool ok = false;
        const double value = text.left(end).toDouble(&ok);
        if (!ok) {
            // 还原显示
            rule_table_updating_ = true;
            item->setText(QString::number(rule->threshold, 'f', 2) +
                          QString::fromStdString(rule->unit));
            rule_table_updating_ = false;
            return;
        }
        rule->threshold = value;
    } else {
        return;
    }

    if (has_result_) Evaluate();
}

void DiagnosticsPage::OnResetRulesClicked() {
    facade_->SetRules(model::DefaultQcRules());
    RebuildRuleTable();
    if (has_result_) Evaluate();
}

void DiagnosticsPage::OnExportClicked() {
    if (!has_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先执行一次分析。"));
        return;
    }

    const QString default_name =
        QString::fromStdString(report_.file_name.empty()
                                   ? std::string("diagnostics")
                                   : report_.file_name) + "_diagnostics.html";
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("导出诊断报告"), default_name,
        tr("HTML 报告 (*.html);;JSON 报告 (*.json);;CSV 报告 (*.csv);;文本报告 (*.txt)"));
    if (filename.isEmpty()) return;

    if (reporting::QcReportExporter::ExportReport(filename.toStdString(), report_)) {
        QMessageBox::information(this, tr("成功"), tr("诊断报告已导出到:\n%1").arg(filename));
    } else {
        QMessageBox::warning(this, tr("失败"), tr("导出诊断报告失败。"));
    }
}

// ---------------------------------------------------------------------------
// 跳转到问题位置（问题清单里选中那一行的位置）
// ---------------------------------------------------------------------------

void DiagnosticsPage::OnJumpToIssue() {
    if (!issue_table_) return;
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
    export_button_->setEnabled(false);
}

const std::vector<model::QcRule>& DiagnosticsPage::rules() const { return facade_->rules(); }

const model::AnalysisResult& DiagnosticsPage::result() const { return facade_->result(); }

} // namespace ui
} // namespace videoeye
