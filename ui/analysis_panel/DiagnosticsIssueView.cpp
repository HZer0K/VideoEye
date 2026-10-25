#include "ui/analysis_panel/DiagnosticsIssueView.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {

DiagnosticsIssueView::DiagnosticsIssueView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void DiagnosticsIssueView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    chart_ = new MetricChartWidget(this);
    chart_->SetTitle(tr("逐秒码率 / 帧率"));
    bitrate_series_ = chart_->AddLineSeries(tr("码率 (kbps)"), QColor("#1e88e5"));
    fps_series_ = chart_->AddLineSeries(tr("帧率 (fps)"), QColor("#43a047"));
    axis_x_ = chart_->AxisX();
    axis_bitrate_ = chart_->AxisY(); // 左轴
    axis_fps_ = chart_->AxisY2();    // 右轴
    chart_->SetAxisY2Visible(true);
    chart_->AttachAxis(fps_series_, axis_fps_);
    axis_x_->SetTitleText(tr("时间 (s)"));
    axis_bitrate_->SetTitleText(tr("kbps"));
    axis_fps_->SetTitleText(tr("fps"));
    chart_->setMinimumHeight(220);
    layout->addWidget(chart_);

    issue_table_ = new QTableWidget(0, 6, this);
    issue_table_->setHorizontalHeaderLabels({tr("严重度"), tr("类别"), tr("问题"), tr("位置"), tr("说明"), tr("建议")});
    issue_table_->verticalHeader()->setVisible(false);
    issue_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    issue_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    issue_table_->horizontalHeader()->setStretchLastSection(true);
    issue_table_->setMinimumHeight(200);
    layout->addWidget(issue_table_);

    // 跳转是按问题清单里选中那一行的位置 seek；时间轴的样本/曲线/导出已收敛到
    // 「事件与时间轴」页，本子页只保留"问题 -> 跳转"这一条动作。
    QWidget* jump_row = new QWidget(this);
    QHBoxLayout* jl = new QHBoxLayout(jump_row);
    jl->setContentsMargins(0, 0, 0, 0);
    jl->addStretch();
    QPushButton* jump_button = new QPushButton(tr("跳转到问题位置"), jump_row);
    jump_button->setToolTip(tr("按当前选中问题的位置执行 seek"));
    connect(jump_button, &QPushButton::clicked, this, &DiagnosticsIssueView::OnJumpToIssue);
    jl->addWidget(jump_button);
    layout->addWidget(jump_row);
}

void DiagnosticsIssueView::SetReport(const model::AnalysisResult& result, const model::QcReport& report) {
    result_ = &result;
    issues_ = report.issues;
    RebuildIssueTable();
    UpdateChart();
}

void DiagnosticsIssueView::Clear() {
    result_ = nullptr;
    issues_.clear();
    if (issue_table_)
        issue_table_->setRowCount(0);
    if (bitrate_series_)
        bitrate_series_->Clear();
    if (fps_series_)
        fps_series_->Clear();
}

void DiagnosticsIssueView::RebuildIssueTable() {
    if (!issue_table_)
        return;
    issue_table_->setRowCount(0);
    issue_table_->setRowCount(static_cast<int>(issues_.size()));

    for (int i = 0; i < static_cast<int>(issues_.size()); ++i) {
        const auto& issue = issues_[i];
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

void DiagnosticsIssueView::UpdateChart() {
    if (!bitrate_series_ || !fps_series_ || result_ == nullptr)
        return;
    bitrate_series_->Clear();
    fps_series_->Clear();

    const auto& bitrate =
        result_->video_bitrate_kbps.IsEmpty() ? result_->total_bitrate_kbps : result_->video_bitrate_kbps;
    // 抽稀后再提交：长时间分析的采样点上万，全量喂给自绘图表重绘会卡死 UI
    AppendDecimated(bitrate_series_, bitrate, kMaxChartPoints);
    AppendDecimated(fps_series_, result_->video_fps, kMaxChartPoints);

    double max_t = 1.0;
    if (!bitrate.samples.empty())
        max_t = std::max(max_t, bitrate.samples.back().timestamp_seconds);
    if (!result_->video_fps.samples.empty()) {
        max_t = std::max(max_t, result_->video_fps.samples.back().timestamp_seconds);
    }
    axis_x_->SetRange(0, max_t);
    axis_bitrate_->SetRange(0, std::max(1.0, bitrate.Max() * 1.2));
    axis_fps_->SetRange(0, std::max(1.0, result_->video_fps.Max() * 1.2));
}

void DiagnosticsIssueView::OnJumpToIssue() {
    if (!issue_table_)
        return;
    const int row = issue_table_->currentRow();
    if (row < 0 || row >= static_cast<int>(issues_.size())) {
        QMessageBox::information(this, tr("提示"), tr("请先在问题列表中选择一条记录。"));
        return;
    }
    const double seconds = issues_[row].range.start_seconds;
    if (seconds < 0.0) {
        QMessageBox::information(this, tr("提示"), tr("该问题为文件级问题，没有可跳转的时间点。"));
        return;
    }
    emit SeekRequested(seconds);
}

} // namespace ui
} // namespace videoeye
