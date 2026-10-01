#include "ui/analysis_panel/DiagnosticsPage.h"

#include <QCursor>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <limits>

#include "core/reporting/QcReportExporter.h"
#include "infrastructure/logging/ScopedTimer.h"

namespace videoeye {
namespace ui {

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

    // 播放期时间轴的批量刷新节拍：与面板其它表的刷新同频，别每包都重画
    flush_timer_ = new QTimer(this);
    connect(flush_timer_, &QTimer::timeout, this, &DiagnosticsPage::FlushTimeline);
    flush_timer_->start(120);
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

        sub_tabs_->addTab(page, tr("问题清单"));
    }

    // ---- 子页 1: 规则与阈值 ----
    SetupRuleTab();

    layout->addWidget(sub_tabs_);

    // 子页 2: 时间轴与同步
    SetupTimelineSubTab();

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

void DiagnosticsPage::SetupTimelineSubTab() {
    timeline_summary_label_ = new QLabel(
        tr("开启「事件与时间轴」分析并播放，或在本页执行一次全文件扫描，"
           "将输出 PTS/DTS、音视频同步、帧间隔与 VFR/CFR 判定。"));
    timeline_summary_label_->setWordWrap(true);

    // 帧间隔曲线 + 问题标记散点
    timeline_chart_ = new MetricChartWidget(this);
    timeline_chart_->SetTitle(tr("帧间隔与问题分布"));
    timeline_interval_series_ = timeline_chart_->AddLineSeries(tr("帧间隔 (ms)"),
                                                               QColor("#1e88e5"));
    timeline_marker_series_ = timeline_chart_->AddScatterSeries(tr("问题"), QColor("#e53935"));
    timeline_marker_series_->SetMarkerSize(10.0);
    timeline_axis_x_ = timeline_chart_->AxisX();
    timeline_axis_y_ = timeline_chart_->AxisY();
    timeline_axis_x_->SetTitleText(tr("时间 (s)"));
    timeline_axis_y_->SetTitleText(tr("间隔 (ms)"));
    timeline_chart_->setMinimumHeight(220);

    timeline_issue_table_ = new QTableWidget(0, 6, this);
    timeline_issue_table_->setHorizontalHeaderLabels(
        {tr("类型"), tr("严重度"), tr("流"), tr("时间码"), tr("次数"), tr("说明")});
    timeline_issue_table_->verticalHeader()->setVisible(false);
    timeline_issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline_issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    timeline_issue_table_->horizontalHeader()->setStretchLastSection(true);
    timeline_issue_table_->setMinimumHeight(200);

    QWidget* button_row = new QWidget(this);
    QHBoxLayout* bl = new QHBoxLayout(button_row);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addStretch();
    QPushButton* jump_button = new QPushButton(tr("跳转到问题帧"), button_row);
    jump_button->setToolTip(tr("按当前选中问题的位置执行 seek"));
    connect(jump_button, &QPushButton::clicked, this, &DiagnosticsPage::OnJumpToIssue);
    bl->addWidget(jump_button);

    QWidget* page = new QWidget(sub_tabs_);
    QVBoxLayout* pl = new QVBoxLayout(page);
    pl->setContentsMargins(2, 2, 2, 2);
    pl->addWidget(timeline_summary_label_);
    pl->addWidget(timeline_chart_);
    pl->addWidget(timeline_issue_table_);
    pl->addWidget(button_row);
    sub_tabs_->addTab(page, tr("时间轴与同步"));

    connect(timeline_chart_, &MetricChartWidget::PointHovered,
            this, &DiagnosticsPage::OnTimelineMarkerHovered);
}

// ---------------------------------------------------------------------------
// 全文件扫描
// ---------------------------------------------------------------------------

void DiagnosticsPage::StartScan(const analyzer::AnalysisOptions& options) {
    if (source_path_.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("请先打开一个媒体文件。"));
        return;
    }
    if (facade_->IsRunning()) {
        QMessageBox::information(this, tr("提示"), tr("分析正在进行中。"));
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
    analyzer::AnalysisOptions effective = options;
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
                                       const analyzer::AnalysisResult& result) {
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
        VE_PERF("DiagnosticsPage::Evaluate(QC 规则 + 时间轴 + 图表)");
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

    // 时间轴与同步诊断：直接复用全文件扫描的 demux 层结果
    timeline_result_ = facade_->result().timeline;
    timeline_offline_ = true;
    RefreshTimelineUi();

    RebuildIssueTable();
    UpdateQcChart();
    UpdateQcSummary();
    emit QcReportChanged(report_);
}

void DiagnosticsPage::ApplySceneLink(const std::vector<model::SceneChangeResult>& records,
                                     const analyzer::BitrateGopOptions& gop_options) {
    if (!has_result_) {
        QMessageBox::information(this, tr("提示"), tr("请先在本页点击「开始分析」完成一次扫描。"));
        return;
    }
    if (records.empty()) {
        QMessageBox::information(this, tr("提示"),
            tr("当前没有场景切换数据。请先在「场景切换」页启用检测并播放一段视频。"));
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
    {
        SeriesBatch batch(qc_bitrate_series_);
        batch.Reserve(static_cast<int>(bitrate.samples.size()));
        for (const auto& sample : bitrate.samples) {
            batch.Add(sample.timestamp_seconds, sample.value);
        }
    }
    {
        SeriesBatch batch(qc_fps_series_);
        batch.Reserve(static_cast<int>(result.video_fps.samples.size()));
        for (const auto& sample : result.video_fps.samples) {
            batch.Add(sample.timestamp_seconds, sample.value);
        }
    }

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
// 时间轴与同步（实时：播放逐包/逐帧累积；离线：全文件扫描结果）
// ---------------------------------------------------------------------------

void DiagnosticsPage::OnSyncSample(double audio_ms, double video_ms) {
    facade_->OnSyncSample(audio_ms, video_ms);
    timeline_dirty_ = true;
}

void DiagnosticsPage::OnPacketTiming(const model::PacketTiming& timing) {
    facade_->OnPacket(timing);
    timeline_dirty_ = true;
}

void DiagnosticsPage::OnFrameTiming(const model::FrameTimingInfo& timing) {
    facade_->OnFrame(timing);
    timeline_dirty_ = true;
}

void DiagnosticsPage::FlushTimeline() {
    if (!isVisible() || !timeline_dirty_) return;
    // 实时解码路径：用当前累积状态做一份快照（Finish 在副本上执行，不破坏累积状态）
    timeline_result_ = facade_->Snapshot();
    timeline_offline_ = false;
    RefreshTimelineUi();
    timeline_dirty_ = false;
}

void DiagnosticsPage::RefreshTimelineUi() {
    UpdateTimelineSummary();
    UpdateTimelineChart();

    if (!timeline_issue_table_) return;
    timeline_issue_table_->setRowCount(0);
    const auto& issues = timeline_result_.issues;
    timeline_issue_table_->setRowCount(static_cast<int>(issues.size()));
    for (int i = 0; i < static_cast<int>(issues.size()); ++i) {
        const auto& issue = issues[i];
        SetTableItemText(timeline_issue_table_, i, 0, QString::fromStdString(issue.title));
        SetTableItemText(timeline_issue_table_, i, 1, QString::fromStdString(issue.SeverityText()));
        SetTableItemText(timeline_issue_table_, i, 2,
                         issue.stream_index >= 0 ? QString::number(issue.stream_index)
                                                 : tr("文件级"));
        SetTableItemText(timeline_issue_table_, i, 3,
                         QString::fromStdString(model::FormatTimestamp(issue.range.start_seconds)));
        SetTableItemText(timeline_issue_table_, i, 4, QString::number(issue.occurrence_count));
        SetTableItemText(timeline_issue_table_, i, 5, QString::fromStdString(issue.detail));

        QColor color = QColor("#1565c0");
        switch (issue.severity) {
            case model::IssueSeverity::Critical: color = QColor("#c62828"); break;
            case model::IssueSeverity::Error:    color = QColor("#e53935"); break;
            case model::IssueSeverity::Warning:  color = QColor("#ef6c00"); break;
            case model::IssueSeverity::Info:     color = QColor("#1565c0"); break;
        }
        if (QTableWidgetItem* cell = timeline_issue_table_->item(i, 1)) {
            cell->setForeground(color);
        }
    }
    timeline_issue_table_->resizeColumnsToContents();
}

void DiagnosticsPage::UpdateTimelineSummary() {
    if (!timeline_summary_label_) return;
    const auto& r = timeline_result_;
    if (!r.has_data) {
        timeline_summary_label_->setText(
            tr("开启「时间轴与同步」分析并播放，或在本页执行一次全文件扫描，"
               "将输出 PTS/DTS、音视频同步、帧间隔与 VFR/CFR 判定。"));
        return;
    }

    timeline_summary_label_->setText(
        tr("数据源: %1 ｜ 最大音视频偏移 <b>%2</b> ms ｜ 首帧偏移 %3 ms ｜ 平均帧间隔 %4 ms"
           "（σ %5 ms, %6~%7 ms）｜ 帧率判定 <b>%8</b> ｜ 视频 %9 ms / 音频 %10 ms"
           " ｜ 帧 %11 ｜ 关键帧 %12 ｜ 问题 %13")
            .arg(timeline_offline_ ? tr("全文件扫描") : tr("播放实时"))
            .arg(QString::number(r.max_av_offset_ms, 'f', 1))
            .arg(QString::number(r.av_start_offset_ms, 'f', 1))
            .arg(QString::number(r.avg_frame_interval_ms, 'f', 2))
            .arg(QString::number(r.frame_interval_stddev_ms, 'f', 2))
            .arg(QString::number(r.min_frame_interval_ms, 'f', 2))
            .arg(QString::number(r.max_frame_interval_ms, 'f', 2))
            .arg(QString::fromStdString(r.FrameRateVerdict()))
            .arg(QString::number(r.video_duration_ms, 'f', 0))
            .arg(QString::number(r.audio_duration_ms, 'f', 0))
            .arg(r.frame_count)
            .arg(r.key_frame_count)
            .arg(static_cast<int>(r.issues.size())));
}

void DiagnosticsPage::UpdateTimelineChart() {
    if (!timeline_interval_series_) return;
    timeline_interval_series_->Clear();
    timeline_marker_series_->Clear();
    timeline_marker_issue_index_.clear();

    const auto& r = timeline_result_;
    double max_interval = 1.0;
    double max_time = 1.0;
    {
        SeriesBatch batch(timeline_interval_series_);
        batch.Reserve(static_cast<int>(r.frame_interval_ms.samples.size()));
        for (const auto& sample : r.frame_interval_ms.samples) {
            const double t = sample.timestamp_seconds / 1000.0;   // ms -> s
            batch.Add(t, sample.value);
            max_interval = std::max(max_interval, sample.value);
            max_time = std::max(max_time, t);
        }
    }

    // 问题标记：按严重度着色，y 取该时刻的帧间隔（无数据则取 0）
    for (int i = 0; i < static_cast<int>(r.issues.size()); ++i) {
        const auto& issue = r.issues[i];
        const double t = issue.range.start_seconds;
        if (t < 0.0) continue;
        const double y = r.frame_interval_ms.ValueAt(issue.range.start_seconds * 1000.0);
        *timeline_marker_series_ << QPointF(t, y);
        timeline_marker_issue_index_.append(i);

        QColor color = QColor("#1565c0");
        switch (issue.severity) {
            case model::IssueSeverity::Critical: color = QColor("#c62828"); break;
            case model::IssueSeverity::Error:    color = QColor("#e53935"); break;
            case model::IssueSeverity::Warning:  color = QColor("#ef6c00"); break;
            case model::IssueSeverity::Info:     color = QColor("#1565c0"); break;
        }
        timeline_marker_series_->SetColor(color);  // 颜色以最后一组为准（散点整体着色）
    }

    timeline_axis_x_->SetRange(0, max_time);
    timeline_axis_y_->SetRange(0, max_interval * 1.2);
}

void DiagnosticsPage::OnTimelineMarkerHovered(const QPointF& point, bool state) {
    if (!state || !timeline_marker_series_) return;

    // 命中点定位到问题序号（按下标顺序一一对应）
    const QList<QPointF> points = timeline_marker_series_->Points();
    int index = -1;
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i < points.size(); ++i) {
        const double dx = points[i].x() - point.x();
        const double dy = points[i].y() - point.y();
        const double dist = dx * dx + dy * dy;
        if (dist < best) { best = dist; index = i; }
    }
    if (index < 0 || index >= timeline_marker_issue_index_.size()) return;

    const int issue_index = timeline_marker_issue_index_[index];
    if (issue_index < 0 || issue_index >= static_cast<int>(timeline_result_.issues.size())) return;
    const auto& issue = timeline_result_.issues[issue_index];

    QToolTip::showText(QCursor::pos(),
        tr("流 #%1\n时间码: %2\n%3 (%4)\n实测: %5 ms / 阈值: %6 ms\n%7")
            .arg(issue.stream_index)
            .arg(QString::fromStdString(model::FormatTimestamp(issue.range.start_seconds)))
            .arg(QString::fromStdString(issue.title))
            .arg(QString::fromStdString(issue.SeverityText()))
            .arg(QString::number(issue.metric_value, 'f', 2))
            .arg(QString::number(issue.threshold, 'f', 2))
            .arg(QString::fromStdString(issue.detail)));
}

void DiagnosticsPage::OnJumpToIssue() {
    if (!timeline_issue_table_) return;
    const int row = timeline_issue_table_->currentRow();
    if (row < 0 || row >= static_cast<int>(timeline_result_.issues.size())) {
        QMessageBox::information(this, tr("提示"), tr("请先在问题列表中选择一条记录。"));
        return;
    }
    const double seconds = timeline_result_.issues[row].range.start_seconds;
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
    // 时间轴分析器的累积状态（播放期逐包/逐帧喂进来的那份）也要一起清
    facade_->Reset();
    has_result_ = false;
    timeline_result_ = model::TimelineAnalysisResult{};
    timeline_offline_ = false;
    timeline_dirty_ = false;
    issue_table_->setRowCount(0);
    timeline_issue_table_->setRowCount(0);
    timeline_marker_issue_index_.clear();
    progress_bar_->setValue(0);
    progress_bar_->setFormat(tr("未开始"));
    summary_label_->setText(tr("点击「开始分析」对当前文件做一次完整扫描，"
                               "将按内置 QC 规则输出问题清单与评分。"));
    timeline_summary_label_->setText(
        tr("开启「时间轴与同步」分析并播放，或在本页执行一次全文件扫描，"
           "将输出 PTS/DTS、音视频同步、帧间隔与 VFR/CFR 判定。"));
    timeline_interval_series_->Clear();
    timeline_marker_series_->Clear();
    export_button_->setEnabled(false);
}

const std::vector<model::QcRule>& DiagnosticsPage::rules() const { return facade_->rules(); }

const analyzer::AnalysisResult& DiagnosticsPage::result() const { return facade_->result(); }

} // namespace ui
} // namespace videoeye
