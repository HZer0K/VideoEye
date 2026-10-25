#pragma once

// 「响度与电平」子页：BS.1770 响度曲线（M/S/I + 目标线）与电平曲线（RMS / 采样峰值 / 真峰值）。
//
// 从 AudioQcPage 拆出的展示型组件。目标响度是页面上的输入框（仅影响参考线），
// 所以每次刷新都把当前目标值一起传进来；本视图不持有任何扫描选项。

#include <QWidget>

#include "core/domain/model/AnalysisResult.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class AudioLoudnessView : public QWidget {
    Q_OBJECT

public:
    explicit AudioLoudnessView(QWidget* parent = nullptr);

    // nullptr / 未分析的音频 -> 清空曲线
    void SetResult(const model::AnalysisResult* result, double target_lufs);

private:
    void SetupUi();

    MetricChartWidget* lufs_chart_ = nullptr;
    ChartSeries* momentary_series_ = nullptr;
    ChartSeries* short_term_series_ = nullptr;
    ChartSeries* integrated_series_ = nullptr;
    ChartSeries* target_series_ = nullptr;
    ChartAxis* lufs_axis_x_ = nullptr;
    ChartAxis* lufs_axis_y_ = nullptr;

    MetricChartWidget* level_chart_ = nullptr;
    ChartSeries* rms_series_ = nullptr;
    ChartSeries* peak_series_ = nullptr;
    ChartSeries* true_peak_series_ = nullptr;
    ChartAxis* level_axis_x_ = nullptr;
    ChartAxis* level_axis_y_ = nullptr;
};

} // namespace ui
} // namespace videoeye
