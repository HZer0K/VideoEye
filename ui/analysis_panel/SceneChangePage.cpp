#include "ui/analysis_panel/SceneChangePage.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

#include "infrastructure/logging/ScopedTimer.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/theme/AppTheme.h"

#include <algorithm>

namespace videoeye {
namespace ui {

SceneChangePage::SceneChangePage(bool feature_checked, QWidget* parent)
    : QWidget(parent), feature_checked_(feature_checked) {
    SetupUi();
}

void SceneChangePage::SetupUi() {
    // 页面本体就是 QWidget（外部 QStackedWidget 的一页），不再多包一层
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 4);
    layout->setSpacing(4);

    // 标题 + 启用开关
    {
        QWidget* row = new QWidget(this);
        QHBoxLayout* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel* title = new QLabel(tr("场景切换检测（镜头边界）"), row);
        rl->addWidget(title);
        rl->addStretch();
        QCheckBox* toggle = new QCheckBox(tr("启用检测"), row);
        toggle->setChecked(feature_checked_);
        connect(toggle, &QCheckBox::toggled, this, [this](bool checked) {
            feature_checked_ = checked;
            emit FeatureToggled(checked);
        });
        rl->addWidget(toggle);
        layout->addWidget(row);
    }

    summary_label_ = new QLabel(
        tr("启用检测并在播放中分析视频，将在此列出检测到的镜头切换点（时间戳 + 切换强度）。"));
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    // 切换强度柱状图
    chart_ = new MetricChartWidget(this);
    chart_->SetTitle(tr("切换强度（逐切换点）"));
    chart_->SetLegendVisible(false);
    series_ = chart_->AddBarSeries(tr("强度"), QColor("#8e24aa"));
    axis_x_ = chart_->AxisX();
    axis_y_ = chart_->AxisY();
    axis_x_->SetLabelFormat("%d");
    axis_x_->SetTitleText(tr("切换点序号"));
    axis_y_->SetTitleText(tr("强度"));
    axis_y_->SetRange(0, 1);
    chart_->setMinimumHeight(180);
    layout->addWidget(chart_);

    // 切换点列表
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("帧序号"), tr("时间戳"), tr("切换强度")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setMinimumHeight(200);
    layout->addWidget(table_);

    // 导出按钮
    QPushButton* export_btn = new QPushButton(tr("导出切换点 CSV"), this);
    connect(export_btn, &QPushButton::clicked, this, &SceneChangePage::OnExportCsv);
    layout->addWidget(export_btn, 0, Qt::AlignRight);

    // 建好之后由 AnalysisPanel::AddPageWithScroll 收进外部 stack
}

void SceneChangePage::AppendResult(const model::SceneChangeResult& result) {
    records_.push_back(result);
    table_dirty_ = true;   // 真正刷新等面板的批量定时器，避免每个切换点重绘一次
}

void SceneChangePage::FlushPending() {
    if (!table_dirty_) return;
    table_dirty_ = false;
    const size_t total = records_.size();
    for (size_t i = table_synced_count_; i < total; ++i) {
        AppendRow(records_[i]);
    }
    table_synced_count_ = total;
    UpdateChart();
    UpdateSummary();
}

void SceneChangePage::AppendRow(const model::SceneChangeResult& result) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    SetTableItemText(table_, row, 0, QString::number(result.frame_index));
    SetTableItemText(table_, row, 1, theme::font::formatTime(static_cast<int>(result.timestamp * 1000)));
    SetTableItemText(table_, row, 2, QString::number(result.score, 'f', 3));
}

void SceneChangePage::UpdateChart() {
    if (!series_) return;
    series_->Clear();
    // 限制显示最近 200 个切换点, 避免柱状图过载
    const int max_bars = 200;
    const int start = records_.size() > static_cast<size_t>(max_bars)
                          ? static_cast<int>(records_.size() - max_bars) : 0;
    for (int i = start; i < static_cast<int>(records_.size()); ++i) {
        series_->Append(static_cast<double>(i), records_[i].score);
    }
    axis_x_->SetRange(start, std::max(start + 1, static_cast<int>(records_.size())));
}

void SceneChangePage::UpdateSummary() {
    const int n = static_cast<int>(records_.size());
    if (n == 0) {
        summary_label_->setText(
            tr("启用检测并在播放中分析视频，将在此列出检测到的镜头切换点（时间戳 + 切换强度）。"));
        return;
    }
    double sum = 0.0;
    double max_s = 0.0;
    for (const auto& r : records_) { sum += r.score; if (r.score > max_s) max_s = r.score; }
    summary_label_->setText(
        tr("共检测到 %1 个切换点 | 平均强度 %2 | 最大强度 %3")
            .arg(n).arg(QString::number(sum / n, 'f', 3)).arg(QString::number(max_s, 'f', 3)));
}

void SceneChangePage::OnExportCsv() {
    if (records_.empty()) {
        QMessageBox::information(this, tr("提示"), tr("当前没有可导出的切换点数据。"));
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(this, tr("导出切换点 CSV"),
        source_path_.section('/', -1) + "_scenecut.csv",
        tr("CSV 文件 (*.csv)"));
    if (filename.isEmpty()) return;
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("错误"), tr("无法打开文件: ") + filename);
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "\xEF\xBB\xBF";
    stream << "frame_index,timestamp_seconds,score\n";
    for (const auto& r : records_) {
        stream << r.frame_index << "," << QString::number(r.timestamp, 'f', 3) << ","
               << QString::number(r.score, 'f', 4) << "\n";
    }
    file.close();
    QMessageBox::information(this, tr("成功"), tr("已导出 %1 个切换点到:\n%2")
        .arg(records_.size()).arg(filename));
}

void SceneChangePage::Reset() {
    records_.clear();
    table_dirty_ = false;
    table_synced_count_ = 0;
    if (table_) table_->setRowCount(0);
    UpdateChart();
    UpdateSummary();
}


} // namespace ui
} // namespace videoeye
