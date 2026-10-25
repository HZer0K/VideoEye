#include "ui/analysis_panel/AudioLoudnessView.h"

#include <QVBoxLayout>

#include <QColor>

#include <algorithm>
#include <vector>

#include "core/domain/model/LoudnessPoint.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"

namespace videoeye {
namespace ui {
namespace {

// 抽稀（保留每组极值，避免丢掉峰值）
void AppendAudioPoints(ChartSeries* series, const std::vector<model::LoudnessPoint>& points, int limit,
                       double(model::LoudnessPoint::*member), double floor_value) {
    if (points.empty())
        return;
    const double fallback = floor_value;
    auto get = [member, fallback](const model::LoudnessPoint& p) {
        const double v = p.*member;
        return (v <= fallback + 1.0) ? fallback : v;
    };
    // 抽稀走 ui/charts/ChartDecimation.h 的统一实现（MinMax，峰谷都留）：
    // 这里是响度曲线，"掉到底"的静音段和"冲上去"的削波同样是要看的特征，
    // 等间隔取点会把它们一并抽掉。
    SubmitDecimated(
        series, DecimateFrom(points, [](const model::LoudnessPoint& p) { return p.timestamp_seconds; }, get, limit));
}

} // namespace

AudioLoudnessView::AudioLoudnessView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void AudioLoudnessView::SetupUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    lufs_chart_ = new MetricChartWidget(this);
    lufs_chart_->SetTitle(tr("响度曲线（M 400ms / S 3s / I 累计）"));
    momentary_series_ = lufs_chart_->AddLineSeries(tr("瞬时 M"), QColor("#42a5f5"));
    short_term_series_ = lufs_chart_->AddLineSeries(tr("短期 S"), QColor("#66bb6a"));
    integrated_series_ = lufs_chart_->AddLineSeries(tr("累计 I"), QColor("#8e24aa"));
    target_series_ = lufs_chart_->AddLineSeries(tr("目标"), QColor("#e53935"));
    lufs_axis_x_ = lufs_chart_->AxisX();
    lufs_axis_y_ = lufs_chart_->AxisY();
    lufs_axis_x_->SetTitleText(tr("时间 (s)"));
    lufs_axis_y_->SetTitleText(tr("LUFS"));
    lufs_chart_->setMinimumHeight(210);
    layout->addWidget(lufs_chart_);

    level_chart_ = new MetricChartWidget(this);
    level_chart_->SetTitle(tr("电平曲线（RMS / 采样峰值 / 真峰值）"));
    rms_series_ = level_chart_->AddLineSeries(tr("RMS dBFS"), QColor("#42a5f5"));
    peak_series_ = level_chart_->AddLineSeries(tr("峰值 dBFS"), QColor("#66bb6a"));
    true_peak_series_ = level_chart_->AddLineSeries(tr("真峰值 dBTP"), QColor("#fb8c00"));
    level_axis_x_ = level_chart_->AxisX();
    level_axis_y_ = level_chart_->AxisY();
    level_axis_x_->SetTitleText(tr("时间 (s)"));
    level_axis_y_->SetTitleText(tr("dB"));
    level_chart_->setMinimumHeight(210);
    layout->addWidget(level_chart_);
}

void AudioLoudnessView::SetResult(const model::AnalysisResult* result, double target_lufs) {
    momentary_series_->Clear();
    short_term_series_->Clear();
    integrated_series_->Clear();
    target_series_->Clear();
    rms_series_->Clear();
    peak_series_->Clear();
    true_peak_series_->Clear();
    if (result == nullptr || !result->audio_qc.analyzed)
        return;

    const auto& qc = result->audio_qc;
    const auto& points = qc.loudness_points;
    AppendAudioPoints(momentary_series_, points, kMaxChartPoints, &model::LoudnessPoint::momentary_lufs,
                      model::kSilenceLufs);
    AppendAudioPoints(short_term_series_, points, kMaxChartPoints, &model::LoudnessPoint::short_term_lufs,
                      model::kSilenceLufs);
    AppendAudioPoints(integrated_series_, points, kMaxChartPoints, &model::LoudnessPoint::integrated_lufs,
                      model::kSilenceLufs);
    AppendAudioPoints(rms_series_, points, kMaxChartPoints, &model::LoudnessPoint::rms_dbfs, model::kSilenceLevelDb);
    AppendAudioPoints(peak_series_, points, kMaxChartPoints, &model::LoudnessPoint::sample_peak_dbfs,
                      model::kSilenceLevelDb);
    AppendAudioPoints(true_peak_series_, points, kMaxChartPoints, &model::LoudnessPoint::true_peak_dbtp,
                      model::kSilenceLevelDb);

    // 目标响度参考线
    if (!points.empty()) {
        target_series_->Append(points.front().timestamp_seconds, target_lufs);
        target_series_->Append(points.back().timestamp_seconds, target_lufs);
    }

    const double span = std::max(1.0, qc.duration_seconds);
    lufs_axis_x_->SetRange(0.0, span);
    level_axis_x_->SetRange(0.0, span);
    lufs_axis_y_->SetRange(-60.0, 0.0);
    level_axis_y_->SetRange(-90.0, 6.0);
}

} // namespace ui
} // namespace videoeye
