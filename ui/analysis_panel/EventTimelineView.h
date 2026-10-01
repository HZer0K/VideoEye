#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QLabel>
#include <QTabWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QString>
#include <deque>
#include <functional>
#include <vector>

#include "ui/charts/MetricChartWidget.h"
#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"

namespace videoeye {
namespace ui {

/// 事件 / 同步 / 时间轴三张分析页的聚合视图（"事件与时间轴"页）。
///
/// 从 AnalysisPanel 拆出：这三张表连同它们的流概览/曲线/CSV 导出/批量刷新
/// 自成一体、只依赖本视图内的控件与数据，不涉及码流分析页。
/// 拆出后 AnalysisPanel 通过同名的公开方法（薄转发）继续对外提供原信号槽，
/// MainWindow 的连接无需改动。
class EventTimelineView : public QWidget {
    Q_OBJECT

public:
    explicit EventTimelineView(QWidget* parent = nullptr);
    ~EventTimelineView();

    // 功能开关的读写钩子由 AnalysisPanel 注入（视图不持有 feature_enabled_ 映射）。
    // feature 用本视图内部的 0/1/2 编号（Event/Sync/Timeline），由调用方映射到
    // AnalysisPanel::AnalysisFeature。
    //
    // 注入后会立即按钩子的真实状态刷新三个 QCheckBox：控件是在构造函数里建的，
    // 那时钩子还不存在，只能先按默认值创建；不同步的话界面会显示"启用分析"，
    // 而面板里的 feature_enabled_ 其实是关的 —— 数据被过滤掉、界面却毫无提示。
    void SetFeatureHooks(std::function<bool(int)> is_enabled,
                         std::function<void(int, bool)> set_enabled);

    // AppendSyncSample 要把同步样本喂给诊断页构建音视频偏移曲线。
    // 具体由 AnalysisPanel 在收到 SyncSampleReceived 后转交，本视图不持诊断页指针
    // —— 页面间直接互相引用会让两者无法单独构造/单独测试。

    // 以下方法由 AnalysisPanel 的同名槽薄转发（保持对外接口不变）。
    void ResetAnalysisEventList();
    void AppendAnalysisEvent(const model::AnalysisEvent& event_info);
    void ResetSyncSampleList();
    void AppendSyncSample(const model::SyncSample& sample);
    void ResetTimelineEventList();
    void AppendTimelineEvent(const model::TimelineEvent& event);

    // 批量刷新（由 AnalysisPanel 的 FlushPendingUiUpdates 统一触发，已在外层
    // 做过 isVisible 门控，这里不再重复判断）。
    void FlushPendingUiUpdates();

signals:
    // 视图内部功能开关变化（0/1/2 = Event/Sync/Timeline），由 AnalysisPanel 接回
    // feature_enabled_ 映射并转发 AnalysisFeatureToggled 信号。
    void FeatureToggled(int feature, bool enabled);

    // 收到一条同步样本（毫秒）。由 AnalysisPanel 转交诊断页构建音视频偏移曲线；
    // 本视图不关心谁在听，从而与诊断页解耦。
    void SyncSampleReceived(double audio_ms, double video_ms);

private:
    // 内部数据记录（原 AnalysisPanel 的同名结构体，移入本视图）。
    // 必须先于下面用到它们的方法声明。
    struct AnalysisEventRecord {
        int index = 0;
        QString severity;
        QString type;
        int stream_index = -1;
        qint64 pts = 0;
        double timestamp_seconds = 0.0;
        QString summary;
        QString detail;
    };

    struct SyncSampleRecord {
        int index = 0;
        double audio_timestamp_seconds = 0.0;
        double video_timestamp_seconds = 0.0;
        double diff_ms = 0.0;
        bool audio_anchor = false;
    };

    struct TimelineEventRecord {
        int index = 0;
        QString category;
        double timestamp_seconds = 0.0;
        QString label;
        QString detail;
    };

    enum class ViewFeature { Event = 0, Sync = 1, Timeline = 2 };

    // 建页（取代 AnalysisPanel::SetupEventAnalysisTab 内部的三段 Setup）
    void SetupEventTab();
    void SetupSyncTab();
    void SetupTimelineTab();

    // 按注入的钩子刷新三个"启用分析"勾选框（屏蔽信号，不触发用户操作语义）。
    void SyncTogglesFromHooks();

    void RebuildEventTable();
    void RebuildSyncTable();
    void RebuildTimelineTable();

    void UpdateEventSummary();
    void UpdateSyncSummary();
    void UpdateTimelineSummary();

    void OnExportEventCsv();
    void OnExportSyncCsv();
    void OnExportTimelineCsv();

    void AppendEventRowToTable(const AnalysisEventRecord& record);
    void AppendSyncRowToTable(const SyncSampleRecord& record);
    void AppendTimelineRowToTable(const TimelineEventRecord& record);

    void UpdateSyncChart();
    void UpdateTimelineChart();

    void FlushPendingEventTableUpdates();
    void FlushPendingSyncTableUpdates();
    void FlushPendingTimelineTableUpdates();

    // 视图持有 AnalysisPanel 注入的钩子
    std::function<bool(int)> is_enabled_;
    std::function<void(int, bool)> set_enabled_;

    // 页面容器
    QTabWidget* event_analysis_sub_tabs_ = nullptr;
    QWidget* event_tab_ = nullptr;
    QWidget* sync_tab_ = nullptr;
    QWidget* timeline_tab_ = nullptr;

    // 事件页控件
    QLabel* event_summary_label_ = nullptr;
    QTableWidget* event_table_ = nullptr;
    QPushButton* export_event_csv_button_ = nullptr;

    // 同步页控件
    QLabel* sync_summary_label_ = nullptr;
    MetricChartWidget* sync_chart_ = nullptr;
    QTableWidget* sync_table_ = nullptr;
    QPushButton* export_sync_csv_button_ = nullptr;

    // 时间轴页控件
    QLabel* timeline_summary_label_ = nullptr;
    MetricChartWidget* timeline_chart_ = nullptr;
    QTableWidget* timeline_table_ = nullptr;
    QPushButton* export_timeline_csv_button_ = nullptr;

    // 三个"启用分析"勾选框。必须留成员：SetFeatureHooks 注入钩子后要拿它们
    // 把真实开关状态回写到界面，否则界面与实际行为不一致（评审 P1）。
    QCheckBox* event_toggle_ = nullptr;
    QCheckBox* sync_toggle_ = nullptr;
    QCheckBox* timeline_toggle_ = nullptr;

    // 曲线系列与坐标轴
    ChartSeries* sync_series_ = nullptr;
    ChartAxis* sync_axis_x_ = nullptr;
    ChartAxis* sync_axis_y_ = nullptr;
    ChartSeries* timeline_video_series_ = nullptr;
    ChartSeries* timeline_audio_series_ = nullptr;
    ChartSeries* timeline_event_series_ = nullptr;
    ChartAxis* timeline_axis_x_ = nullptr;
    ChartAxis* timeline_axis_y_ = nullptr;

    // 数据记录
    std::vector<AnalysisEventRecord> analysis_event_records_;
    std::vector<SyncSampleRecord> sync_sample_records_;
    std::vector<TimelineEventRecord> timeline_event_records_;

    // 脏标记与同步计数（事件）
    bool event_table_dirty_ = false;
    bool event_summary_dirty_ = false;
    size_t event_table_synced_record_count_ = 0;
    // 同步
    bool sync_table_dirty_ = false;
    bool sync_summary_dirty_ = false;
    size_t sync_table_synced_record_count_ = 0;
    std::deque<qreal> sync_chart_values_;
    // 时间轴
    bool timeline_table_dirty_ = false;
    bool timeline_summary_dirty_ = false;
    size_t timeline_table_synced_record_count_ = 0;
};

} // namespace ui
} // namespace videoeye
