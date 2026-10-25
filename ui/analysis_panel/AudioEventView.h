#pragma once

// 「静音与削波」子页：时间轴（静音方波 / 削波三角）+ 削波表 + 静音段表。
//
// 从 AudioQcPage 拆出的展示型组件。点表里的任意一行只发 SeekRequested，
// 由页面转给播放器；两张表的行数也是侧栏导航的 IssueCount 口径。

#include <QString>
#include <QWidget>

#include <QLabel>
#include <QTableWidget>

#include "core/domain/model/AnalysisResult.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class AudioEventView : public QWidget {
    Q_OBJECT

public:
    explicit AudioEventView(QWidget* parent = nullptr);

    // nullptr = 尚无扫描结果（清空图表与两张表）
    void SetResult(const model::AnalysisResult* result);

    int ClipRows() const;
    int SilenceRows() const;

signals:
    void SeekRequested(double seconds);

private:
    void SetupUi();
    void RebuildClipTable();
    void RebuildSilenceTable();
    void OnClipCellClicked(int row, int column);
    void OnSilenceCellClicked(int row, int column);

    MetricChartWidget* event_chart_ = nullptr;
    ChartSeries* silence_series_ = nullptr;
    ChartSeries* clip_series_ = nullptr;
    ChartAxis* event_axis_x_ = nullptr;
    ChartAxis* event_axis_y_ = nullptr;
    QTableWidget* clip_table_ = nullptr;
    QTableWidget* silence_table_ = nullptr;

    // 页面保证引用在下次 SetResult 前有效（facade 的结果快照）
    const model::AnalysisResult* result_ = nullptr;
};

} // namespace ui
} // namespace videoeye
