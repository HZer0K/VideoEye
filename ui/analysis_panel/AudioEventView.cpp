#include "ui/analysis_panel/AudioEventView.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHeaderView>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/charts/ChartDecimation.h"

namespace videoeye {
namespace ui {

AudioEventView::AudioEventView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void AudioEventView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    event_chart_ = new MetricChartWidget(this);
    event_chart_->SetTitle(tr("静音段（方波）与削波点（三角）时间轴"));
    silence_series_ = event_chart_->AddLineSeries(tr("静音段"), QColor("#1e88e5"));
    clip_series_ = event_chart_->AddScatterSeries(tr("削波"), QColor("#e53935"));
    clip_series_->SetMarkerSize(9.0);
    clip_series_->SetMarkerShape(ChartMarkerShape::Triangle);
    clip_series_->SetBorderColor(QColor("#e53935"));
    event_axis_x_ = event_chart_->AxisX();
    event_axis_y_ = event_chart_->AxisY();
    event_axis_x_->SetTitleText(tr("时间 (s)"));
    event_axis_y_->SetTitleText(tr("静音 0/1 ｜ 削波 1.5"));
    event_axis_y_->SetRange(-0.2, 1.8);
    event_chart_->setMinimumHeight(180);
    layout->addWidget(event_chart_);

    QLabel* clip_hint = new QLabel(tr("点击任意一行跳转到该位置。"), this);
    layout->addWidget(clip_hint);
    clip_table_ = new QTableWidget(0, 5, this);
    clip_table_->setHorizontalHeaderLabels({tr("起始"), tr("结束"), tr("声道"), tr("削波样本"), tr("峰值 dBFS")});
    clip_table_->verticalHeader()->setVisible(false);
    clip_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    clip_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    clip_table_->horizontalHeader()->setStretchLastSection(true);
    clip_table_->setMinimumHeight(120);
    connect(clip_table_, &QTableWidget::cellClicked, this, &AudioEventView::OnClipCellClicked);
    layout->addWidget(clip_table_);

    silence_table_ = new QTableWidget(0, 4, this);
    silence_table_->setHorizontalHeaderLabels({tr("起始"), tr("结束"), tr("时长(s)"), tr("平均 RMS dBFS")});
    silence_table_->verticalHeader()->setVisible(false);
    silence_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    silence_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    silence_table_->horizontalHeader()->setStretchLastSection(true);
    silence_table_->setMinimumHeight(120);
    connect(silence_table_, &QTableWidget::cellClicked, this, &AudioEventView::OnSilenceCellClicked);
    layout->addWidget(silence_table_);
}

void AudioEventView::SetResult(const model::AnalysisResult* result) {
    result_ = result;

    silence_series_->Clear();
    clip_series_->Clear();
    if (result != nullptr && result->audio_qc.analyzed) {
        const auto& qc = result->audio_qc;
        // 静音段画成方波；削波点画在 y=1.5
        {
            SeriesBatch silence(silence_series_);
            silence.Reserve(static_cast<int>(qc.silence_ranges.size() * 4));
            for (const auto& range : qc.silence_ranges) {
                silence.Add(range.start_seconds, 0.0);
                silence.Add(range.start_seconds, 1.0);
                silence.Add(range.end_seconds, 1.0);
                silence.Add(range.end_seconds, 0.0);
            }
        }
        {
            SeriesBatch clip(clip_series_);
            clip.Reserve(static_cast<int>(qc.clipping_events.size()));
            for (const auto& event : qc.clipping_events) {
                clip.Add(event.start_seconds, 1.5);
            }
        }
        const double span = std::max(1.0, qc.duration_seconds);
        event_axis_x_->SetRange(0.0, span);
    }

    RebuildClipTable();
    RebuildSilenceTable();
}

int AudioEventView::ClipRows() const {
    return clip_table_ ? clip_table_->rowCount() : 0;
}

int AudioEventView::SilenceRows() const {
    return silence_table_ ? silence_table_->rowCount() : 0;
}

void AudioEventView::RebuildClipTable() {
    if (!clip_table_)
        return;
    clip_table_->setRowCount(0);
    if (result_ == nullptr)
        return;
    const auto& qc = result_->audio_qc;
    const int rows = std::min<int>(static_cast<int>(qc.clipping_events.size()), 2000);
    clip_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& e = qc.clipping_events[static_cast<size_t>(i)];
        SetTableItemText(clip_table_, i, 0, QString::number(e.start_seconds, 'f', 3));
        SetTableItemText(clip_table_, i, 1, QString::number(e.end_seconds, 'f', 3));
        QString channel = (e.channel < 0)
                              ? tr("全部")
                              : QString::fromStdString(e.channel < static_cast<int>(qc.channels.size())
                                                           ? qc.channels[static_cast<size_t>(e.channel)].name
                                                           : std::string());
        SetTableItemText(clip_table_, i, 2, channel);
        SetTableItemText(clip_table_, i, 3, QString::number(e.sample_count));
        SetTableItemText(clip_table_, i, 4,
                         QString::number(e.peak > 0.0 ? 20.0 * std::log10(e.peak) : model::kSilenceLevelDb, 'f', 2));
    }
}

void AudioEventView::RebuildSilenceTable() {
    if (!silence_table_)
        return;
    silence_table_->setRowCount(0);
    if (result_ == nullptr)
        return;
    const auto& qc = result_->audio_qc;
    const int rows = std::min<int>(static_cast<int>(qc.silence_ranges.size()), 2000);
    silence_table_->setRowCount(rows);
    for (int i = 0; i < rows; ++i) {
        const auto& r = qc.silence_ranges[static_cast<size_t>(i)];
        SetTableItemText(silence_table_, i, 0, QString::number(r.start_seconds, 'f', 3));
        SetTableItemText(silence_table_, i, 1, QString::number(r.end_seconds, 'f', 3));
        SetTableItemText(silence_table_, i, 2, QString::number(r.duration_seconds, 'f', 3));
        SetTableItemText(silence_table_, i, 3, QString::number(r.rms_dbfs, 'f', 1));
    }
}

void AudioEventView::OnClipCellClicked(int row, int) {
    if (result_ == nullptr || row < 0)
        return;
    const auto& events = result_->audio_qc.clipping_events;
    if (row >= static_cast<int>(events.size()))
        return;
    emit SeekRequested(events[static_cast<size_t>(row)].start_seconds);
}

void AudioEventView::OnSilenceCellClicked(int row, int) {
    if (result_ == nullptr || row < 0)
        return;
    const auto& ranges = result_->audio_qc.silence_ranges;
    if (row >= static_cast<int>(ranges.size()))
        return;
    emit SeekRequested(ranges[static_cast<size_t>(row)].start_seconds);
}

} // namespace ui
} // namespace videoeye
