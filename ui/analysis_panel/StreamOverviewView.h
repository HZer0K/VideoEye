#pragma once

// 「码流分析」页的顶部区：流概览 5 指标 + 码率/帧率/GOP 三条趋势曲线 + 导出分析报告。
//
// 从 AnalysisPanel 拆出来的独立页面组件。它本身不是外部 QStackedWidget 的一页，
// 而是被 SetupBitstreamTab 与 FramePacketView 一起拼进「码流分析」页（顶部固定高度，
// 让曲线一直可见）。接缝：
//   - 数据进来：SetStreamStats()（播放期逐次回吐）、SetGopSummaries()（FramePacketView
//     产出 GOP 摘要后经面板桥接过来，供「最大GOP大小」指标与 GOP 分布曲线共用）
//   - 开关出去：面板通过 SetFeatureHooks 注入，视图只认自己的编号 0 = 流统计
//   - 导出报告要用当前文件路径拼默认文件名 + 写进报告元信息
//
// 「最大 GOP」统一取 GopSummary 的口径（解码帧 pict_type 推导），不再使用
// StreamAnalyzer 基于 packet flags 的独立统计 —— 与「GOP 摘要」表保持一致。

#include <QWidget>
#include <QString>

#include <QTableWidget>

#include <deque>
#include <functional>
#include <vector>

#include "core/domain/model/StreamStats.h"
#include "ui/analysis_panel/StreamRecords.h"
#include "ui/charts/MetricChartWidget.h"

class QCheckBox;
class QPushButton;

namespace videoeye {
namespace ui {

class StreamOverviewView : public QWidget {
    Q_OBJECT

public:
    explicit StreamOverviewView(QWidget* parent = nullptr);

    // 页面只认自己的开关编号（0 = 流统计）。编号到 AnalysisFeature 的映射、以及
    // AnalysisFeatureToggled 的转发都由面板做。注入时会立即把真实开关状态回写到
    // 「启用分析」勾选框：控件是在构造函数里建的，那时钩子还不存在。
    void SetFeatureHooks(std::function<bool(int)> is_enabled,
                         std::function<void(int, bool)> set_enabled);

    // 导出分析报告时写进报告元信息（当前文件路径）
    void SetSourcePath(const QString& path) { source_path_ = path; }

    // 播放期逐次回吐的流统计：攒到 FlushPending() 再刷 UI（与面板批量刷新节拍一致）
    void SetStreamStats(const model::StreamStats& stats);

    // FramePacketView 产出 GOP 摘要后由面板喂进来：填「最大GOP大小」+ 重画 GOP 分布曲线
    void SetGopSummaries(const std::vector<GopSummary>& summaries);

    // 换文件时配合「视频帧列表」一起清空曲线（原先由面板的 ResetVideoFrameList 触发）
    void ResetCharts();

    bool HasPending() const { return has_pending_stats_ || gop_dirty_; }
    void FlushPending();

private:
    void SetupUi();
    void SyncToggleFromHooks();
    void RefreshStatsUi();
    void UpdateBitrateChart();
    void UpdateFpsChart();
    void UpdateGopChart();
    void OnExportReport();

    std::function<bool(int)> is_enabled_;
    std::function<void(int, bool)> set_enabled_;
    QString source_path_;

    QTableWidget* stats_table_ = nullptr;
    QPushButton* export_button_ = nullptr;
    // 必须留成员：SetFeatureHooks 注入钩子后要拿它把真实开关状态回写到界面
    QCheckBox* toggle_ = nullptr;

    MetricChartWidget* bitrate_chart_ = nullptr;
    ChartSeries* bitrate_series_ = nullptr;
    ChartAxis* bitrate_axis_x_ = nullptr;
    ChartAxis* bitrate_axis_y_ = nullptr;

    MetricChartWidget* fps_chart_ = nullptr;
    ChartSeries* fps_series_ = nullptr;
    ChartAxis* fps_axis_x_ = nullptr;
    ChartAxis* fps_axis_y_ = nullptr;

    MetricChartWidget* gop_chart_ = nullptr;
    ChartSeries* gop_series_ = nullptr;
    ChartAxis* gop_axis_x_ = nullptr;
    ChartAxis* gop_axis_y_ = nullptr;

    model::StreamStats latest_stats_;
    bool has_pending_stats_ = false;

    // 图表与导出报告共用的历史数据（GOP 曲线直接用 gop_summaries_，不另攒一份）
    std::deque<qreal> bitrate_chart_values_;
    std::deque<qreal> fps_chart_values_;
    std::vector<GopSummary> gop_summaries_;
    bool gop_dirty_ = false;
};

}  // namespace ui
}  // namespace videoeye
