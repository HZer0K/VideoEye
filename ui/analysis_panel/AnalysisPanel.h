#pragma once

#include <QWidget>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QCategoryAxis>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QTreeWidget>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QTableWidget>
#include <QTimer>
#include <QComboBox>
#include <QCheckBox>
#include <QMap>
#include <QtCharts/QValueAxis>
#include <deque>
#include <vector>

#include "core/analyzer/StreamAnalyzer.h"
#include "core/analyzer/FrameAnalyzer.h"
#include "core/model/AnalysisEvent.h"
#include "core/model/AudioVisualizationFrame.h"
#include "core/model/PacketInfo.h"
#include "core/model/SyncSample.h"
#include "core/model/TimelineEvent.h"
#include "core/model/Mp4BoxInfo.h"

namespace videoeye {
namespace ui {

// 分析面板类
class AnalysisPanel : public QWidget {
    Q_OBJECT
    
public:
    // 分析功能开关枚举
    enum class AnalysisFeature {
        Master,        // 全局主开关
        StreamStats,   // 流统计
        VideoFrame,    // 视频帧
        AudioFrame,    // 音频帧
        Packet,        // 数据包
        Event,         // 分析事件
        SyncSample,    // 音视频同步
        Timeline,      // 时间线
        AudioVis,      // 音频可视化
        Histogram,     // 直方图
        Mp4Box         // MP4 Box 分析
    };

    explicit AnalysisPanel(QWidget* parent = nullptr);
    ~AnalysisPanel();
    
    // 检查某个分析功能是否启用
    bool IsFeatureEnabled(AnalysisFeature feature) const;
    
signals:
    // 分析功能开关变化信号 (供 MainWindow 连接 MediaPlayer)
    void AnalysisFeatureToggled(int feature, bool enabled);
    
public slots:
    // 更新统计数据
    void UpdateStreamStats(const analyzer::StreamStats& stats);
    void UpdateHistogram(const analyzer::HistogramData& hist);
    void ResetVideoFrameList();
    void AppendVideoFrameInfo(int index, int frame_type, bool is_key_frame, qint64 pts, double timestamp_seconds);
    void ResetAudioFrameList();
    void AppendAudioFrameInfo(int index, qint64 pts, double timestamp_seconds,
                              int sample_count, int sample_rate, int channels, int byte_count);
    void ResetPacketList();
    void AppendPacketInfo(const model::PacketInfo& packet_info);
    void ResetAnalysisEventList();
    void AppendAnalysisEvent(const model::AnalysisEvent& event_info);
    void ResetSyncSampleList();
    void AppendSyncSample(const model::SyncSample& sample);
    void ResetTimelineEventList();
    void AppendTimelineEvent(const model::TimelineEvent& event);
    void ResetAudioVisualization();
    void AppendAudioVisualization(const model::AudioVisualizationFrame& frame);
    void OnMp4BoxAnalysisReady(const model::Mp4BoxAnalysisResult& result);
    
    // 导出报告
    void OnExportReport();
    
private:
    struct VideoFrameRecord {
        int index = 0;
        int frame_type = 0;
        bool is_key_frame = false;
        qint64 pts = 0;
        double timestamp_seconds = 0.0;
        int gop_index = 0;
        int gop_position = 0;
    };

    struct GopSummary {
        int gop_index = 0;
        int start_frame = 0;
        int end_frame = 0;
        double start_ts = 0.0;
        double end_ts = 0.0;
        int total_frames = 0;
        int i_count = 0;
        int p_count = 0;
        int b_count = 0;
        int key_count = 0;
    };

    struct AudioFrameRecord {
        int index = 0;
        qint64 pts = 0;
        double timestamp_seconds = 0.0;
        int sample_count = 0;
        int sample_rate = 0;
        int channels = 0;
        int byte_count = 0;
    };

    struct PacketRecord {
        int index = 0;
        int stream_index = -1;
        int stream_type = -1;
        qint64 pts = 0;
        qint64 dts = 0;
        qint64 duration = 0;
        int size = 0;
        int flags = 0;
        qint64 pos = -1;
        double timestamp_seconds = 0.0;
    };

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

    struct AudioVisualizationRecord {
        int index = 0;
        double timestamp_seconds = 0.0;
        double level = 0.0;
        int sample_rate = 0;
        int channels = 0;
        QVector<double> waveform_points;
        QVector<double> spectrum_bins;
    };

    // 初始化UI
    void SetupUI();
    // 在每个标签页中创建带开关的标题栏
    QWidget* CreateToggleHeader(AnalysisFeature feature, const QString& title, QWidget* parent);
    void SetupStreamTab();
    void SetupFrameTab();
    void SetupAudioFrameTab();
    void SetupPacketTab();
    void SetupEventTab();
    void SetupSyncTab();
    void SetupTimelineTab();
    void SetupAudioVisualizationTab();
    void SetupHistogramTab();
    void SetupMp4BoxTab();
    void RebuildFrameTable();
    void RebuildGopTable();
    void RebuildAudioFrameTable();
    void RebuildPacketTable();
    void RebuildEventTable();
    void RebuildSyncTable();
    void RebuildTimelineTable();
    void UpdateFrameSummary();
    void UpdateAudioFrameSummary();
    void UpdatePacketSummary();
    void UpdateEventSummary();
    void UpdateSyncSummary();
    void UpdateTimelineSummary();
    void UpdateAudioVisualizationSummary();
    QString FrameTypeToString(int frame_type) const;
    QString PacketFlagsToString(int flags) const;
    bool MatchesFrameFilter(const VideoFrameRecord& record) const;
    void FlushPendingUiUpdates();
    void FlushPendingFrameTableUpdates();
    void FlushPendingGopTableUpdates();
    void FlushPendingAudioFrameTableUpdates();
    void FlushPendingPacketTableUpdates();
    void FlushPendingEventTableUpdates();
    void FlushPendingSyncTableUpdates();
    void FlushPendingTimelineTableUpdates();
    void FlushPendingAudioVisualization();
    void RefreshStreamStatsUi(const analyzer::StreamStats& stats);
    void SetTableItemText(QTableWidget* table, int row, int column, const QString& text);
    void AppendFrameRowToTable(const VideoFrameRecord& record);
    void AppendAudioFrameRowToTable(const AudioFrameRecord& record);
    void AppendPacketRowToTable(const PacketRecord& record);
    void AppendEventRowToTable(const AnalysisEventRecord& record);
    void AppendSyncRowToTable(const SyncSampleRecord& record);
    void AppendTimelineRowToTable(const TimelineEventRecord& record);
    void UpdateGopRowInTable(int row, const GopSummary& summary);
    void OnExportFrameCsv();
    void OnExportAudioFrameCsv();
    void OnExportPacketCsv();
    void OnExportEventCsv();
    void OnExportSyncCsv();
    void OnExportTimelineCsv();
    void OnExportAudioVisualizationCsv();
    void OnFrameFilterChanged();
    
    // 更新图表
    void UpdateBitrateChart(const analyzer::StreamStats& stats);
    void UpdateFPSChart(const analyzer::StreamStats& stats);
    void UpdateGOPChart(const analyzer::StreamStats& stats);
    void UpdateHistogramChart(const analyzer::HistogramData& hist);
    void UpdateSyncChart();
    void UpdateTimelineChart();
    void UpdateAudioVisualizationCharts();
    
    // 分析功能开关
    QMap<AnalysisFeature, bool> feature_enabled_;
    
    // 成员变量
    QTabWidget* tab_widget_;
    
    // 流分析标签页
    QWidget* stream_tab_;
    QTableWidget* stats_table_;
    QChartView* bitrate_chart_;
    QChartView* fps_chart_;
    QChartView* gop_chart_;

    QWidget* frame_tab_;
    QComboBox* frame_filter_combo_;
    QLabel* frame_summary_label_;
    QTableWidget* frame_table_;
    QPushButton* export_frame_csv_button_;
    QTableWidget* gop_table_;

    QWidget* audio_frame_tab_;
    QLabel* audio_frame_summary_label_;
    QTableWidget* audio_frame_table_;
    QPushButton* export_audio_frame_csv_button_;

    QWidget* packet_tab_;
    QLabel* packet_summary_label_;
    QTableWidget* packet_table_;
    QPushButton* export_packet_csv_button_;

    QWidget* event_tab_;
    QLabel* event_summary_label_;
    QTableWidget* event_table_;
    QPushButton* export_event_csv_button_;

    QWidget* sync_tab_;
    QLabel* sync_summary_label_;
    QChartView* sync_chart_;
    QTableWidget* sync_table_;
    QPushButton* export_sync_csv_button_;

    QWidget* timeline_tab_;
    QLabel* timeline_summary_label_;
    QChartView* timeline_chart_;
    QTableWidget* timeline_table_;
    QPushButton* export_timeline_csv_button_;

    QWidget* audio_visualization_tab_;
    QLabel* audio_visualization_summary_label_;
    QChartView* waveform_chart_;
    QChartView* spectrum_chart_;
    QPushButton* export_audio_visualization_csv_button_;
    
    // 直方图标签页
    QWidget* histogram_tab_;
    QChartView* histogram_chart_;
    
    // MP4 Box 分析标签页
    QWidget* mp4_box_tab_;
    QTreeWidget* box_tree_widget_;
    QTabWidget* box_detail_tabs_;
    QTableWidget* stts_table_;
    QTableWidget* stco_table_;
    QTableWidget* stsc_table_;
    QTableWidget* stsz_table_;
    QLabel* mp4_box_summary_label_;
    model::Mp4BoxAnalysisResult current_box_result_;
    
    // 控制按钮
    QPushButton* export_button_;
    
    // 图表数据系列
    QChart* bitrate_chart_object_;
    QChart* fps_chart_object_;
    QLineSeries* bitrate_series_;
    QLineSeries* fps_series_;
    QLineSeries* sync_series_;
    QLineSeries* timeline_video_series_;
    QLineSeries* timeline_audio_series_;
    QLineSeries* timeline_event_series_;
    QLineSeries* waveform_series_;
    QLineSeries* spectrum_series_;
    QValueAxis* bitrate_axis_x_;
    QValueAxis* bitrate_axis_y_;
    QValueAxis* fps_axis_x_;
    QValueAxis* fps_axis_y_;
    QValueAxis* sync_axis_x_;
    QValueAxis* sync_axis_y_;
    QValueAxis* timeline_axis_x_;
    QValueAxis* timeline_axis_y_;
    QValueAxis* waveform_axis_x_;
    QValueAxis* waveform_axis_y_;
    QValueAxis* spectrum_axis_x_;
    QValueAxis* spectrum_axis_y_;
    
    // 定时器
    QTimer* update_timer_;
    
    // 当前数据
    analyzer::StreamStats current_stats_;
    analyzer::HistogramData current_hist_;
    std::vector<VideoFrameRecord> frame_records_;
    std::vector<AudioFrameRecord> audio_frame_records_;
    std::vector<PacketRecord> packet_records_;
    std::vector<AnalysisEventRecord> analysis_event_records_;
    std::vector<SyncSampleRecord> sync_sample_records_;
    std::vector<TimelineEventRecord> timeline_event_records_;
    AudioVisualizationRecord audio_visualization_record_;
    std::vector<GopSummary> gop_summaries_;
    analyzer::StreamStats pending_stream_stats_;
    bool has_pending_stream_stats_ = false;
    bool frame_table_dirty_ = false;
    bool gop_table_dirty_ = false;
    bool frame_summary_dirty_ = false;
    bool audio_frame_table_dirty_ = false;
    bool audio_frame_summary_dirty_ = false;
    bool packet_table_dirty_ = false;
    bool packet_summary_dirty_ = false;
    bool event_table_dirty_ = false;
    bool event_summary_dirty_ = false;
    bool sync_table_dirty_ = false;
    bool sync_summary_dirty_ = false;
    bool timeline_table_dirty_ = false;
    bool timeline_summary_dirty_ = false;
    bool audio_visualization_dirty_ = false;
    bool has_audio_visualization_record_ = false;
    size_t frame_table_synced_record_count_ = 0;
    size_t gop_table_synced_count_ = 0;
    size_t audio_frame_table_synced_record_count_ = 0;
    size_t packet_table_synced_record_count_ = 0;
    size_t event_table_synced_record_count_ = 0;
    size_t sync_table_synced_record_count_ = 0;
    size_t timeline_table_synced_record_count_ = 0;
    int stream_chart_sample_index_ = 0;
    std::deque<qreal> bitrate_chart_values_;
    std::deque<qreal> fps_chart_values_;
    std::deque<qreal> sync_chart_values_;
};

} // namespace ui
} // namespace videoeye
