#include "ui/analysis_panel/StreamOverviewView.h"

#include "core/reporting/StreamStatsExporter.h"
#include "infrastructure/logging/Logger.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"

#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPair>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <vector>

namespace videoeye {
namespace ui {

namespace {
constexpr int kMaxChartSamples = 300;   // 每条曲线最多保留的采样点
}  // namespace

StreamOverviewView::StreamOverviewView(QWidget* parent)
    : QWidget(parent) {
    SetupUi();
}

void StreamOverviewView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 第一行: 标题 + 流概览启用 toggle + 导出按钮
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("流概览"), row);
        rl->addWidget(title);
        rl->addStretch();

        toggle_ = new QCheckBox(tr("启用分析"), row);
        rl->addWidget(toggle_);

        export_button_ = new QPushButton(tr("导出分析报告"), row);
        export_button_->setToolTip(tr("将当前流统计信息导出为 HTML/JSON/TXT 报告"));
        rl->addWidget(export_button_);

        layout->addWidget(row);
    }

    // 第二行: 流概览 5 指标横排
    QStringList stream_labels = {
        tr("当前码率"), tr("平均码率"), tr("峰值码率"), tr("分析时长"), tr("最大GOP大小")
    };
    stats_table_ = new QTableWidget(1, stream_labels.size(), this);
    stats_table_->setHorizontalHeaderLabels(stream_labels);
    stats_table_->verticalHeader()->setVisible(false);
    for (int c = 0; c < stream_labels.size(); ++c) {
        stats_table_->setColumnWidth(c, 130);
        stats_table_->setItem(0, c, new QTableWidgetItem("0"));
    }
    layout->addWidget(stats_table_);

    // 第三行: 码率 / FPS / GOP 三条趋势横向并排 (而不是纵向堆叠)
    QWidget* chart_row = new QWidget(this);
    QHBoxLayout* chart_layout = new QHBoxLayout(chart_row);
    chart_layout->setContentsMargins(0, 0, 0, 0);
    chart_layout->setSpacing(8);

    auto make_chart_box = [&](const QString& title) -> MetricChartWidget* {
        MetricChartWidget* view = new MetricChartWidget(chart_row);
        view->setMinimumHeight(180);
        view->SetLegendVisible(false);   // 标题已在 GroupBox 上, 图例无信息量
        QGroupBox* box = new QGroupBox(title, chart_row);
        QVBoxLayout* bl = new QVBoxLayout(box);
        bl->setContentsMargins(4, 12, 4, 4);
        bl->addWidget(view);
        chart_layout->addWidget(box, 1);
        return view;
    };

    bitrate_chart_ = make_chart_box(tr("码率趋势 (Kbps)"));
    fps_chart_ = make_chart_box(tr("帧率趋势 (fps)"));
    gop_chart_ = make_chart_box(tr("GOP 帧数分布"));

    layout->addWidget(chart_row);

    // 码率图
    bitrate_series_ = bitrate_chart_->AddLineSeries(QString(), QColor("#42a5f5"));
    bitrate_axis_x_ = bitrate_chart_->AxisX();
    bitrate_axis_y_ = bitrate_chart_->AxisY();
    bitrate_axis_x_->SetLabelFormat("%d");
    bitrate_axis_y_->SetLabelFormat("%.0f");
    bitrate_axis_x_->SetTitleText(tr("采样"));
    bitrate_axis_y_->SetTitleText(tr("Kbps"));

    // 帧率图
    fps_series_ = fps_chart_->AddLineSeries(QString(), QColor("#66bb6a"));
    fps_axis_x_ = fps_chart_->AxisX();
    fps_axis_y_ = fps_chart_->AxisY();
    fps_axis_x_->SetLabelFormat("%d");
    fps_axis_y_->SetLabelFormat("%.1f");
    fps_axis_x_->SetTitleText(tr("采样"));
    fps_axis_y_->SetTitleText(tr("fps"));

    // GOP 图
    gop_series_ = gop_chart_->AddLineSeries(QString(), QColor("#ffa726"));
    gop_axis_x_ = gop_chart_->AxisX();
    gop_axis_y_ = gop_chart_->AxisY();
    gop_axis_x_->SetLabelFormat("%d");
    gop_axis_y_->SetLabelFormat("%d");
    gop_axis_x_->SetTitleText(tr("GOP 序号"));
    gop_axis_y_->SetTitleText(tr("帧数"));

    connect(export_button_, &QPushButton::clicked, this, &StreamOverviewView::OnExportReport);
}

void StreamOverviewView::SetFeatureHooks(std::function<bool(int)> is_enabled,
                                        std::function<void(int, bool)> set_enabled) {
    is_enabled_ = std::move(is_enabled);
    set_enabled_ = std::move(set_enabled);

    SyncToggleFromHooks();
    if (toggle_) {
        connect(toggle_, &QCheckBox::toggled, this, [this](bool checked) {
            if (set_enabled_) set_enabled_(0, checked);
        });
    }
}

void StreamOverviewView::SyncToggleFromHooks() {
    if (!toggle_) return;
    // 没有钩子时按控件当前值保留（组件可脱离面板单独构造）
    const bool enabled = is_enabled_ ? is_enabled_(0) : toggle_->isChecked();
    if (enabled == toggle_->isChecked()) return;
    const QSignalBlocker blocker(toggle_);
    toggle_->setChecked(enabled);
}

void StreamOverviewView::SetStreamStats(const model::StreamStats& stats) {
    latest_stats_ = stats;
    has_pending_stats_ = true;
}

void StreamOverviewView::SetGopSummaries(const std::vector<GopSummary>& summaries) {
    gop_summaries_ = summaries;
    gop_dirty_ = true;
}

void StreamOverviewView::ResetCharts() {
    bitrate_chart_values_.clear();
    fps_chart_values_.clear();
    if (bitrate_series_) bitrate_series_->Clear();
    if (fps_series_) fps_series_->Clear();
    if (gop_series_) gop_series_->Clear();
}

void StreamOverviewView::FlushPending() {
    if (has_pending_stats_) {
        has_pending_stats_ = false;
        RefreshStatsUi();
    }
    if (gop_dirty_) {
        gop_dirty_ = false;
        UpdateGopChart();
    }
}

void StreamOverviewView::RefreshStatsUi() {
    if (!stats_table_) return;
    const model::StreamStats& stats = latest_stats_;

    // 流级指标 (包级统计已移至「数据包」tab，避免重复维护)
    SetTableItemText(stats_table_, 0, 0, QString::number(stats.current_bitrate_bps / 1000) + " Kbps");
    SetTableItemText(stats_table_, 0, 1, QString::number(stats.avg_bitrate_bps / 1000) + " Kbps");
    SetTableItemText(stats_table_, 0, 2, QString::number(stats.peak_bitrate_bps / 1000) + " Kbps");

    const auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - stats.start_time);
    SetTableItemText(stats_table_, 0, 3, QString::number(duration.count()) + " s");
    // 最大 GOP 统一取 GopSummary 的口径 (解码帧 pict_type),
    // 与「码流分析」页的 GOP 摘要表保持一致。
    int ui_max_gop = 0;
    for (const auto& g : gop_summaries_) {
        if (g.total_frames > ui_max_gop) ui_max_gop = g.total_frames;
    }
    SetTableItemText(stats_table_, 0, 4, QString::number(ui_max_gop));

    // 收集历史数据供图表与导出报告共用
    const qreal bitrate_kbps = stats.current_bitrate_bps / 1000.0;
    const qreal fps = stats.current_fps;
    bitrate_chart_values_.push_back(bitrate_kbps);
    fps_chart_values_.push_back(fps);
    if (bitrate_chart_values_.size() > kMaxChartSamples) {
        bitrate_chart_values_.pop_front();
    }
    if (fps_chart_values_.size() > kMaxChartSamples) {
        fps_chart_values_.pop_front();
    }

    UpdateBitrateChart();
    UpdateFpsChart();
}

void StreamOverviewView::UpdateBitrateChart() {
    if (!bitrate_series_ || !bitrate_axis_x_ || !bitrate_axis_y_) return;

    bitrate_series_->Clear();
    qreal max_value = 0.0;
    {
        SeriesBatch batch(bitrate_series_);
        batch.Reserve(static_cast<int>(bitrate_chart_values_.size()));
        for (size_t i = 0; i < bitrate_chart_values_.size(); ++i) {
            const qreal v = bitrate_chart_values_[i];
            batch.Add(static_cast<qreal>(i), v);
            if (v > max_value) max_value = v;
        }
    }
    bitrate_axis_x_->SetRange(0, std::max<qreal>(1.0, bitrate_chart_values_.size()));
    // 上限留 10% 余量, 避免曲线贴顶; 全零时给一个最小量程防止坐标轴退化
    bitrate_axis_y_->SetRange(0, std::max<qreal>(100.0, max_value * 1.1));
}

void StreamOverviewView::UpdateFpsChart() {
    if (!fps_series_ || !fps_axis_x_ || !fps_axis_y_) return;

    fps_series_->Clear();
    qreal max_value = 0.0;
    {
        SeriesBatch batch(fps_series_);
        batch.Reserve(static_cast<int>(fps_chart_values_.size()));
        for (size_t i = 0; i < fps_chart_values_.size(); ++i) {
            const qreal v = fps_chart_values_[i];
            batch.Add(static_cast<qreal>(i), v);
            if (v > max_value) max_value = v;
        }
    }
    fps_axis_x_->SetRange(0, std::max<qreal>(1.0, fps_chart_values_.size()));
    fps_axis_y_->SetRange(0, std::max<qreal>(30.0, max_value * 1.1));
}

void StreamOverviewView::UpdateGopChart() {
    if (!gop_series_ || !gop_axis_x_ || !gop_axis_y_) return;

    // 数据源统一为 GopSummary (由解码帧 pict_type 推导),
    // 不再使用 StreamAnalyzer 基于 packet flags 的独立 GOP 统计。
    gop_series_->Clear();
    if (gop_summaries_.empty()) {
        gop_axis_x_->SetRange(0, 1);
        gop_axis_y_->SetRange(0, 1);
        return;
    }

    int max_frames = 0;
    {
        SeriesBatch batch(gop_series_);
        batch.Reserve(static_cast<int>(gop_summaries_.size()));
        for (const auto& g : gop_summaries_) {
            batch.Add(static_cast<qreal>(g.gop_index), static_cast<qreal>(g.total_frames));
            if (g.total_frames > max_frames) max_frames = g.total_frames;
        }
    }
    gop_axis_x_->SetRange(0, std::max(1, static_cast<int>(gop_summaries_.size())));
    gop_axis_y_->SetRange(0, std::max(1, max_frames));
}

void StreamOverviewView::OnExportReport() {
    QString filename = QFileDialog::getSaveFileName(
        this,
        tr("导出分析报告"),
        QString("videoeye_report_%1.html").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        tr("HTML文件 (*.html);;JSON文件 (*.json);;文本文件 (*.txt);;所有文件 (*)"));

    if (filename.isEmpty()) {
        return;
    }

    LOG_INFO("导出分析报告: " + filename.toStdString());

    bool success = false;
    const QString ext = QFileInfo(filename).suffix().toLower();
    const std::string fname = filename.toStdString();
    const std::string source = source_path_.toStdString();

    // 将 deque 数据转换为 vector 供导出 API 使用
    std::vector<double> fps_history(fps_chart_values_.begin(), fps_chart_values_.end());
    std::vector<int> bitrate_history;
    bitrate_history.reserve(bitrate_chart_values_.size());
    for (qreal v : bitrate_chart_values_) {
        bitrate_history.push_back(static_cast<int>(v * 1000)); // Kbps → bps
    }

    if (ext == "html") {
        success = reporting::StreamStatsExporter::ExportHTMLReport(
            fname, latest_stats_, fps_history, bitrate_history, source);
    } else if (ext == "json") {
        success = reporting::StreamStatsExporter::ExportJSON(
            fname, latest_stats_, source);
    } else if (ext == "txt") {
        success = reporting::StreamStatsExporter::ExportTextReport(
            fname, latest_stats_, source);
    } else {
        // 未知扩展名，默认生成 HTML
        filename += ".html";
        success = reporting::StreamStatsExporter::ExportHTMLReport(
            filename.toStdString(), latest_stats_, fps_history, bitrate_history, source);
    }

    if (success) {
        QMessageBox::information(this, tr("成功"), tr("分析报告已导出到:\n%1").arg(filename));
    } else {
        QMessageBox::warning(this, tr("错误"), tr("导出分析报告失败:\n%1").arg(filename));
    }
}

}  // namespace ui
}  // namespace videoeye
