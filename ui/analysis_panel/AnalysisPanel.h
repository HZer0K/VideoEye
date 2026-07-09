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
#include <QScrollArea>
#include <QStackedWidget>
#include <QMap>
#include <QVariantList>
#include <QVariantMap>
#include <QtCharts/QValueAxis>
#include <deque>
#include <vector>
#include <string>

#include "core/analyzer/StreamAnalyzer.h"
#include "core/analyzer/FrameAnalyzer.h"
#include "core/model/AnalysisEvent.h"
#include "core/model/AudioVisualizationFrame.h"
#include "core/model/PacketInfo.h"
#include "core/model/SyncSample.h"
#include "core/model/TimelineEvent.h"
#include "core/model/Mp4BoxInfo.h"
#include "core/model/EbmlInfo.h"
#include "core/model/ContainerStructureInfo.h"
#include "core/model/MacroblockInfo.h"

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
        AudioLoudness, // 音频响度监测
        Histogram,     // 直方图
        ContainerStructure,  // 文件结构分析
        Macroblock     // 宏块分析 (运动矢量/块统计)
    };

    explicit AnalysisPanel(QWidget* parent = nullptr);
    ~AnalysisPanel();
    
    // 将分析面板各页添加到外部 QStackedWidget
    // 返回添加的页面数量 (10页: 流分析/视频帧/音频帧/数据包/异常事件/同步分析/时间轴/音频响度/直方图/容器结构)
    int PopulateStackedWidget(QStackedWidget* stack);
    
    // 设置当前显示的页面索引 (0-9)
    void SetCurrentPageIndex(int index);
    
    // 检查某个分析功能是否启用
    bool IsFeatureEnabled(AnalysisFeature feature) const;
    
    // 设置当前视频文件路径 (供导出报告使用)
    void SetCurrentVideoPath(const QString& path) { current_video_path_ = path.toStdString(); }
    
    // 重新发射所有启用状态的开关信号 (用于文件打开后同步播放器状态)
    void EmitInitialFeatureStates();
    
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
    void OnContainerStructureReady(const model::ContainerStructureResult& result);
    void UpdateAudioLoudness(const model::AudioVisualizationFrame& frame);
    void UpdateMacroblockInfo(const model::MacroblockFrameAnalysis& analysis);
    
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

    // 初始化UI
    void SetupUI();
    // 在每个标签页中创建带开关的标题栏
    QWidget* CreateToggleHeader(AnalysisFeature feature, const QString& title, QWidget* parent);
    // 将页面包裹 QScrollArea 并添加到页面列表
    void AddPageWithScroll(QWidget* tab_widget, const QString& title);
    void SetupStreamTab();
    void SetupFrameTab();
    void SetupAudioFrameTab();
    void SetupPacketTab();
    void SetupEventTab();
    void SetupSyncTab();
    void SetupTimelineTab();
    void SetupAudioLoudnessTab();
    void SetupHistogramTab();
    void SetupContainerStructureTab();
    void SetupMacroblockTab();
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
    void OnExportHistogramCsv();
    void OnExportMp4Box();
    void OnExportContainerStructure();
    void OnFrameFilterChanged();
    void RefreshMacroblockUi();
    void OnExportMacroblockCsv();
    
    // 更新图表
    void UpdateBitrateChart(const analyzer::StreamStats& stats);
    void UpdateFPSChart(const analyzer::StreamStats& stats);
    void UpdateGOPChart(const analyzer::StreamStats& stats);
    void UpdateHistogramChart(const analyzer::HistogramData& hist);
    void UpdateSyncChart();
    void UpdateTimelineChart();
    
    // 分析功能开关
    QMap<AnalysisFeature, bool> feature_enabled_;
    
    // 页面管理 (替代 QTabWidget)
    QList<QWidget*> page_widgets_;      // 各页面的 QScrollArea (含内容)
    QStringList page_titles_;           // 各页面标题
    QStackedWidget* external_stack_ = nullptr;  // 外部 QStackedWidget (由 MainWindow 提供)
    
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
    
    // 音频响度监测标签页
    QWidget* audio_loudness_tab_;
    QLabel* loudness_summary_label_;
    QChartView* loudness_chart_;
    QChart* loudness_chart_object_;
    QLineSeries* loudness_series_;
    QLineSeries* peak_series_;
    QValueAxis* loudness_axis_x_;
    QValueAxis* loudness_axis_y_;
    std::deque<double> loudness_history_;     // LUFS 历史
    std::deque<double> peak_history_;         // dBFS 历史
    double integrated_lufs_ = -70.0;
    double loudness_range_lu_ = 0.0;
    double max_true_peak_dbtp_ = -70.0;
    double max_peak_dbfs_ = -70.0;
    int loudness_sample_count_ = 0;
    double loudness_sum_ = 0.0;
    
    // 直方图标签页
    QWidget* histogram_tab_;
    QChartView* histogram_chart_;

    // 宏块分析标签页
    QWidget* macroblock_tab_;
    QLabel* macroblock_summary_label_;
    QTableWidget* macroblock_table_;        // 运动矢量表格
    QLabel* macroblock_viz_label_;          // 运动矢量可视化预览
    QTableWidget* macroblock_blocksize_table_;  // 块大小分布
    QTableWidget* macroblock_mag_table_;    // 运动幅度分布
    QPushButton* export_macroblock_csv_button_;
    model::MacroblockFrameAnalysis current_macroblock_analysis_;
    bool macroblock_dirty_ = false;
    
    // 统一文件结构分析标签页
    QWidget* container_tab_;
    QLabel* container_title_label_;       // 动态标题
    QLabel* container_summary_label_;
    QTreeWidget* container_tree_;         // 通用结构树
    QStackedWidget* container_detail_stack_;  // 右侧详情区
    // Page 0: 通用信息
    QTableWidget* container_stream_table_;
    QTableWidget* container_metadata_table_;
    // Page 1: MP4 专用
    QTabWidget* mp4_detail_tabs_;
    QTableWidget* stts_table_;
    QTableWidget* stco_table_;
    QTableWidget* stsc_table_;
    QTableWidget* stsz_table_;
    QTableWidget* co64_table_;
    QTableWidget* stss_table_;
    // Page 2: EBML 专用
    QTabWidget* ebml_detail_tabs_;
    QTableWidget* ebml_track_table_;
    QTableWidget* ebml_cue_table_;
    QTableWidget* ebml_block_table_;
    QPushButton* export_container_button_;
    model::ContainerStructureResult current_container_result_;
    
    // 控制按钮
    QPushButton* export_button_;          // 流统计导出 (HTML/JSON/TXT)
    QPushButton* export_histogram_button_;
    
    // 图表数据系列
    QChart* bitrate_chart_object_;
    QChart* fps_chart_object_;
    QLineSeries* bitrate_series_;
    QLineSeries* fps_series_;
    QLineSeries* sync_series_;
    QLineSeries* timeline_video_series_;
    QLineSeries* timeline_audio_series_;
    QLineSeries* timeline_event_series_;
    QValueAxis* bitrate_axis_x_;
    QValueAxis* bitrate_axis_y_;
    QValueAxis* fps_axis_x_;
    QValueAxis* fps_axis_y_;
    QValueAxis* sync_axis_x_;
    QValueAxis* sync_axis_y_;
    QValueAxis* timeline_axis_x_;
    QValueAxis* timeline_axis_y_;
    
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
    std::vector<GopSummary> gop_summaries_;
    analyzer::StreamStats pending_stream_stats_;
    bool has_pending_stream_stats_ = false;
    bool has_pending_histogram_ = false;
    analyzer::HistogramData pending_histogram_;
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
    
    // 当前视频文件路径 (供导出报告使用)
    std::string current_video_path_;
};

} // namespace ui
} // namespace videoeye
