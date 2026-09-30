#pragma once

// 场景切换页（镜头边界检测）：播放时逐帧产出的切换点在这里累积成表格 + 强度柱状图。
//
// 从 AnalysisPanel 拆出来的独立页面组件。与面板的接缝只有四个：
//   - AppendResult()   播放回调喂检测结果（面板转发 MainWindow 的信号）
//   - FlushPending()   面板的批量刷新定时器到点时调用，只补增量行
//   - records()        「码率与 GOP」页画标记 / 关联关键帧时要读
//   - FeatureToggled() 页内「启用检测」开关，由面板转成统一的 feature 信号
// 页面本体就是 QWidget（外部 QStackedWidget 的一页），不需要再包一层 tab widget。

#include <QWidget>
#include <QString>

#include <QCheckBox>
#include <QLabel>
#include <QTableWidget>

#include <cstddef>
#include <vector>

#include "core/domain/model/SceneChangeResult.h"
#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

class SceneChangePage : public QWidget {
    Q_OBJECT

public:
    // feature_checked: 「启用检测」开关的初值（面板按 AnalysisFeature 表给进来）
    explicit SceneChangePage(bool feature_checked, QWidget* parent = nullptr);

    // 导出 CSV 的默认文件名用当前文件路径拼，面板换文件时同步一次
    void SetSourcePath(const QString& path) { source_path_ = path; }

    // 播放线程检测到切换点：只入队 + 置脏，真正的补表/重绘等面板的批量定时器，
    // 否则每秒几十个切换点会把主线程拖垮。
    void AppendResult(const model::SceneChangeResult& result);

    bool HasPending() const { return table_dirty_; }
    void FlushPending();

    // 换文件时清空（与视频帧 / 音频帧 / 包列表那几页保持一致）
    void Reset();

    const std::vector<model::SceneChangeResult>& records() const { return records_; }
    bool feature_checked() const { return feature_checked_; }

signals:
    // 页内开关变化：面板负责写回 feature 表并对外发射 AnalysisFeatureToggled
    void FeatureToggled(bool enabled);

private:
    void SetupUi();
    void AppendRow(const model::SceneChangeResult& result);
    void UpdateChart();
    void UpdateSummary();
    void OnExportCsv();

    bool feature_checked_ = false;
    QString source_path_;

    QLabel* summary_label_ = nullptr;
    QTableWidget* table_ = nullptr;
    MetricChartWidget* chart_ = nullptr;
    ChartSeries* series_ = nullptr;
    ChartAxis* axis_x_ = nullptr;
    ChartAxis* axis_y_ = nullptr;

    std::vector<model::SceneChangeResult> records_;
    bool table_dirty_ = false;
    std::size_t table_synced_count_ = 0;
};

} // namespace ui
} // namespace videoeye
