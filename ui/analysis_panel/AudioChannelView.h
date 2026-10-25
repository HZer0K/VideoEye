#pragma once

// 「声道与相位」子页：声道能量柱状图 + 声道相关性曲线 + metadata / 位流参数表。
//
// 从 AudioQcPage 拆出的展示型组件。metadata 表的后半段来自帧头解析（与解码无关），
// 所以即使响度那一段因解码失败没有数据，这里仍然有结论。

#include <QWidget>

#include <QTableWidget>

#include "core/domain/model/AnalysisResult.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class AudioChannelView : public QWidget {
    Q_OBJECT

public:
    explicit AudioChannelView(QWidget* parent = nullptr);

    // nullptr = 尚无扫描结果（清空图表与 metadata 表）
    void SetResult(const model::AnalysisResult* result);

private:
    void SetupUi();
    void UpdateCharts();
    void RebuildMetadataTable();

    MetricChartWidget* channel_chart_ = nullptr;
    ChartSeries* channel_series_ = nullptr;      // RMS dBFS
    ChartSeries* channel_peak_series_ = nullptr; // 峰值 dBFS
    ChartAxis* channel_axis_x_ = nullptr;
    ChartAxis* channel_axis_y_ = nullptr;

    MetricChartWidget* corr_chart_ = nullptr;
    ChartSeries* corr_series_ = nullptr;
    ChartAxis* corr_axis_x_ = nullptr;
    ChartAxis* corr_axis_y_ = nullptr;

    QTableWidget* metadata_table_ = nullptr;

    // 页面保证引用在下次 SetResult 前有效（facade 的结果快照）
    const model::AnalysisResult* result_ = nullptr;
};

} // namespace ui
} // namespace videoeye
