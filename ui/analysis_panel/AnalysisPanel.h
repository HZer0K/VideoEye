#pragma once

#include <QWidget>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QBarCategoryAxis>
#include <QtCharts/QCategoryAxis>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QTreeWidget>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QTableWidget>
#include <QProgressBar>
#include <QTimer>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QMap>
#include <QVariantList>
#include <QVariantMap>
#include <QtCharts/QValueAxis>
#include <QtCharts/QScatterSeries>
#include <chrono>
#include <deque>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <memory>

#include "core/analyzer/StreamAnalyzer.h"
#include "core/model/AnalysisEvent.h"
#include "core/model/AudioVisualizationFrame.h"
#include "core/model/PacketInfo.h"
#include "core/model/SyncSample.h"
#include "core/model/TimelineEvent.h"
#include "core/model/Mp4BoxInfo.h"
#include "core/model/EbmlInfo.h"
#include "core/model/ContainerStructureInfo.h"
#include "core/model/MacroblockInfo.h"
#include "core/analyzer/SceneChangeAnalyzer.h"
#include "core/analyzer/AnalysisCoordinator.h"
#include "core/analyzer/BitrateGopAnalyzer.h"
#include "core/analyzer/QcRuleEngine.h"
#include "core/analyzer/TimelineAnalyzer.h"
#include "core/model/FrameTimingInfo.h"
#include "core/model/TimelineDiagnostic.h"
#include "core/model/QcReport.h"
#include "core/model/AudioQcResult.h"

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
        ContainerStructure,  // 文件结构分析
        Macroblock,    // 宏块分析 (运动矢量/块统计)
        SceneChange,   // 场景切换检测 (镜头边界)
        Diagnostics    // 诊断与报告 (全文件扫描 + QC 规则引擎)
    };

    explicit AnalysisPanel(QWidget* parent = nullptr);
    ~AnalysisPanel();
    
    // 将分析面板各页添加到外部 QStackedWidget
    // 返回添加的页面数量 (流分析/视频帧/音频帧/数据包/异常事件/同步分析/时间轴/音频 QC/容器结构等)
    int PopulateStackedWidget(QStackedWidget* stack);

    // 设置当前显示的页面索引
    void SetCurrentPageIndex(int index);

    // 检查某个分析功能是否启用
    bool IsFeatureEnabled(AnalysisFeature feature) const;

    // 设置当前视频文件路径 (供导出报告)
    void SetCurrentVideoPath(const QString& path) {
        current_video_path_ = path.toStdString();
        // 切换文件后重置时间轴累计状态 (避免上一文件的数据混入)
        timeline_analyzer_.Reset();
        timeline_result_ = model::TimelineAnalysisResult{};
        timeline_offline_ = false;
        timeline_dirty_ = false;
    }
    
    // 重新发射所有启用状态的开关信号 (用于文件打开后同步播放器状态)
    void EmitInitialFeatureStates();

    // 用面板当前的诊断选项对 current_video_path_ 发起全文件诊断扫描。
    // 播放器打开失败进入"分析模式"时由 MainWindow 调用, 让诊断页展示具体错误原因。
    void StartDiagnosticsScanForCurrentFile();
    
signals:
    // 分析功能开关变化信号 (供 MainWindow 连接 MediaPlayer)
    void AnalysisFeatureToggled(int feature, bool enabled);

    // 跳转到指定时间（秒）: 由"跳转到问题帧"触发, MainWindow 连接到播放器 Seek
    void SeekRequested(double seconds);
    
public slots:
    // 更新统计数据
    void UpdateStreamStats(const analyzer::StreamStats& stats);

    // 码率与 GOP 深度分析
    void OnStartBitrateGopAnalysis();
    void OnCancelBitrateGopAnalysis();
    void OnBitrateGopWindowChanged();
    void OnBitrateGopOptionChanged();
    void OnLinkSceneChanges();
    void OnBitrateGopCellClicked(int row, int column);
    void OnBitrateAnomalyCellClicked(int row, int column);
    void OnExportBitrateGopCsv();
    void OnExportBitrateCurveCsv();
    void OnExportBitrateAnomalyCsv();
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
    void UpdateMacroblockInfo(const model::MacroblockFrameAnalysis& analysis);
    void OnSceneChangeDetected(const analyzer::SceneChangeResult& result);

    // 音频 QC（响度 / 真峰值 / 削波 / 静音 / 声道相位）
    void OnStartAudioQcAnalysis();
    void OnCancelAudioQcAnalysis();
    void OnAudioQcOptionChanged();
    void OnAudioQcClipCellClicked(int row, int column);
    void OnAudioQcSilenceCellClicked(int row, int column);
    void OnExportAudioQcCsv();

    // 导出报告
    void OnExportReport();

    // 诊断与报告 (全文件扫描 + QC 规则引擎)
    void OnStartDiagnostics();
    void OnCancelDiagnostics();
    void OnExportQcReport();
    void OnResetQcRules();
    void OnQcRuleItemChanged(QTableWidgetItem* item);
    void OnDiagnosticsProgress(quint64 generation, double percent, const QString& stage);
    void OnDiagnosticsFinished(quint64 generation, bool completed,
                               const analyzer::AnalysisResult& result);
    void OnDiagnosticsFailed(quint64 generation, const QString& message);

    // 时间轴与同步诊断（播放实时数据）
    void OnTimelinePacket(const model::PacketTiming& timing);
    void OnFrameTiming(const model::FrameTimingInfo& timing);
    void OnTimelineMarkerHovered(const QPointF& point, bool state);
    void OnJumpToTimelineIssue();

    // 包表/帧表按 PTS 互跳联动
    void OnPacketTableSelectionChanged();
    void OnVideoFrameTableSelectionChanged();

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
    void SetupPacketTab();
    void SetupBitstreamTab();
    void SetupEventAnalysisTab();
    void SetupEventTab();
    void SetupSyncTab();
    void SetupTimelineTab();
    void SetupContainerStructureTab();
    void SetupMacroblockTab();
    void SetupSceneChangeTab();
    void SetupBitrateGopTab();
    void SetupDiagnosticsTab();
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
    void UpdateTimelineChart();
    QString FrameTypeToString(int frame_type) const;
    QString PacketFlagsToString(int flags) const;
    QString PacketStreamTypeToName(int type) const;
    bool PacketMatchesFilter(const PacketRecord& record) const;
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
    void OnExportGopCsv();
    void OnExportPacketCsv();
    void OnExportEventCsv();
    void OnExportSyncCsv();
    void OnExportTimelineCsv();
    void OnExportMp4Box();
    void OnExportContainerStructure();
    void OnFrameFilterChanged();
    void RefreshMacroblockUi();
    void OnExportMacroblockCsv();

    // 场景切换检测
    void FlushPendingSceneChangeTable();
    void AppendSceneChangeRow(const analyzer::SceneChangeResult& result);
    void UpdateSceneChangeChart();
    void UpdateSceneChangeSummary();
    void OnExportSceneChangeCsv();

    // 码率与 GOP 深度分析页
    void StartDiagnosticsScan(const analyzer::AnalysisOptions& options);
    void ApplyBitrateGopOptionsFromUi();
    void UpdateBitrateGopUi();          // 汇总 + 曲线 + GOP 表 + 异常 + 建议 一次刷新
    void UpdateBitrateGopSummary();
    void UpdateBitrateGopChart();
    void RebuildBitrateGopTable();
    void RebuildBitrateAnomalyTable();
    void UpdateBitrateGopSuggestions();

    // 音频 QC 页
    void SetupAudioQcTab();
    void ApplyAudioQcOptionsFromUi();
    void UpdateAudioQcUi();          // 汇总 + 曲线 + 表格 + 判定 一次刷新
    void UpdateAudioQcSummary();
    void UpdateAudioQcCharts();
    void RebuildAudioQcClipTable();
    void RebuildAudioQcSilenceTable();
    void RebuildAudioQcVerdictTable();
    void RebuildAudioQcMetadataTable();

    // 诊断与报告页
    void RebuildIssueTable();
    void UpdateQcSummary();
    void UpdateQcChart();
    void RebuildRuleTable();
    // 用 diagnostics_result_ + qc_rule_engine_ 生成报告并刷新 UI
    void EvaluateDiagnostics();
    // 时间轴与同步页
    void SetupTimelineDiagnosticsSubPage();
    void RefreshTimelineUi();
    void UpdateTimelineDiagnosticChart();
    void UpdateTimelineDiagnosticSummary();

    // 更新图表
    void UpdateBitrateChart(const analyzer::StreamStats& stats);
    void UpdateFPSChart(const analyzer::StreamStats& stats);
    // GOP 曲线数据源为 UI 侧 gop_summaries_ (统一口径, 见 RebuildGopTable)
    void UpdateGOPChart();
    void ResetStreamCharts();
    void UpdateSyncChart();
    
    // 分析功能开关
    QMap<AnalysisFeature, bool> feature_enabled_;
    
    // 页面管理 (替代 QTabWidget)
    QList<QWidget*> page_widgets_;      // 各页面的 QScrollArea (含内容)
    QStringList page_titles_;           // 各页面标题
    QStackedWidget* external_stack_ = nullptr;  // 外部 QStackedWidget (由 MainWindow 提供)
    int bitstream_page_index_ = -1;      // 码流分析页在 page_widgets_ 中的索引 (合并了旧的流/帧/包三页)
    bool linking_ = false;              // 包/帧互跳回调重入保护

    // 码流分析页 (合并自原流分析/帧分析/包分析三个独立页)
    QWidget* bitstream_tab_;
    QWidget* stream_section_;           // 顶部区: 6 指标 + 3 chart + 导出按钮
    QWidget* detail_sub_tabs_container_; // 底部区: QTabWidget 容器

    // 流概览子区 (bitstream_tab_ 顶部)
    QTableWidget* stats_table_;
    QChartView* bitrate_chart_;
    QChart* bitrate_chart_object_;
    QLineSeries* bitrate_series_;
    QValueAxis* bitrate_axis_x_;
    QValueAxis* bitrate_axis_y_;
    QChartView* fps_chart_;
    QChart* fps_chart_object_;
    QLineSeries* fps_series_;
    QValueAxis* fps_axis_x_;
    QValueAxis* fps_axis_y_;
    QChartView* gop_chart_;
    QChart* gop_chart_object_;
    QLineSeries* gop_series_;
    QValueAxis* gop_axis_x_;
    QValueAxis* gop_axis_y_;

    // 码流明细子 TabWidget (bitstream_tab_ 底部, 4 个子页)
    QTabWidget* detail_sub_tabs_;
    QWidget* video_sub_;                // 子页 0: 视频帧
    QWidget* packet_sub_;               // 子页 1: 包
    QWidget* gop_summary_sub_;          // 子页 2: GOP 摘要
    QWidget* audio_frame_sub_;          // 子页 3: 音频帧

    QComboBox* frame_filter_combo_;
    QLabel* frame_summary_label_;
    QTableWidget* frame_table_;
    QPushButton* export_frame_csv_button_;
    QTableWidget* gop_table_;

    QLabel* audio_frame_summary_label_;
    QTableWidget* audio_frame_table_;
    QPushButton* export_audio_frame_csv_button_;

    QLabel* packet_summary_label_;
    QComboBox* packet_filter_combo_;
    QTableWidget* packet_table_;
    QPushButton* export_packet_csv_button_;
    int packet_filter_mode_ = -1;  // -1=全部, 0=视频流, 1=音频流, 2=其他流

    QWidget* event_analysis_tab_;
    QTabWidget* event_analysis_sub_tabs_;

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

    // 场景切换检测标签页
    QWidget* scene_change_tab_;
    QLabel* scene_change_summary_label_;
    QTableWidget* scene_change_table_;
    QChartView* scene_change_chart_;
    QChart* scene_change_chart_object_;
    QBarSeries* scene_change_series_;
    QBarSet* scene_change_bar_set_;
    QValueAxis* scene_change_axis_x_;
    QValueAxis* scene_change_axis_y_;
    std::vector<analyzer::SceneChangeResult> scene_change_records_;
    bool scene_change_table_dirty_ = false;
    size_t scene_change_table_synced_count_ = 0;

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
    
    // 码率与 GOP 深度分析标签页
    QWidget* bitrate_gop_tab_;
    QLabel* bitrate_gop_summary_label_;
    QProgressBar* bitrate_gop_progress_bar_;
    QPushButton* bitrate_gop_start_button_;
    QPushButton* bitrate_gop_cancel_button_;
    QComboBox* bitrate_window_combo_;
    QDoubleSpinBox* bitrate_target_peak_spin_;
    QDoubleSpinBox* bitrate_max_gop_seconds_spin_;
    QSpinBox* bitrate_max_gop_frames_spin_;
    QCheckBox* bitrate_decode_types_check_;
    QChartView* bitrate_gop_chart_;
    QChart* bitrate_gop_chart_object_;
    QLineSeries* bitrate_gop_series_;
    QLineSeries* bitrate_target_series_;
    QScatterSeries* bitrate_iframe_series_;
    QScatterSeries* bitrate_scene_series_;
    QScatterSeries* bitrate_anomaly_series_;
    QValueAxis* bitrate_gop_axis_x_;
    QValueAxis* bitrate_gop_axis_y_;
    QTabWidget* bitrate_gop_sub_tabs_;
    QTableWidget* bitrate_gop_table_;
    QTableWidget* bitrate_anomaly_table_;
    QListWidget* bitrate_suggestion_list_;
    analyzer::BitrateGopOptions bitrate_gop_options_;
    analyzer::AnalysisOptions diagnostics_options_;
    double bitrate_gop_display_window_ = 1.0;   // 当前图表显示的窗口长度

    // 音频 QC 标签页
    QWidget* audio_qc_tab_;
    QLabel* audio_qc_summary_label_;
    QProgressBar* audio_qc_progress_bar_;
    QPushButton* audio_qc_start_button_;
    QPushButton* audio_qc_cancel_button_;
    QDoubleSpinBox* audio_qc_target_lufs_spin_;
    QDoubleSpinBox* audio_qc_silence_spin_;
    QDoubleSpinBox* audio_qc_min_silence_spin_;
    QDoubleSpinBox* audio_qc_clip_spin_;
    QCheckBox* audio_qc_loudness_check_;
    QCheckBox* audio_qc_true_peak_check_;
    QCheckBox* audio_qc_correlation_check_;
    QTabWidget* audio_qc_sub_tabs_;
    // 响度曲线
    QChartView* audio_lufs_chart_;
    QChart* audio_lufs_chart_object_;
    QLineSeries* audio_momentary_series_;
    QLineSeries* audio_short_term_series_;
    QLineSeries* audio_integrated_series_;
    QLineSeries* audio_target_series_;
    QValueAxis* audio_lufs_axis_x_;
    QValueAxis* audio_lufs_axis_y_;
    // 电平曲线
    QChartView* audio_level_chart_;
    QChart* audio_level_chart_object_;
    QLineSeries* audio_rms_series_;
    QLineSeries* audio_peak_series_;
    QLineSeries* audio_true_peak_series_;
    QValueAxis* audio_level_axis_x_;
    QValueAxis* audio_level_axis_y_;
    // 静音段 / 削波点时间轴
    QChartView* audio_event_chart_;
    QChart* audio_event_chart_object_;
    QLineSeries* audio_silence_series_;
    QScatterSeries* audio_clip_series_;
    QValueAxis* audio_event_axis_x_;
    QValueAxis* audio_event_axis_y_;
    // 声道能量柱状图
    QChartView* audio_channel_chart_;
    QChart* audio_channel_chart_object_;
    QBarSeries* audio_channel_series_;
    QBarCategoryAxis* audio_channel_axis_x_;
    QValueAxis* audio_channel_axis_y_;
    // 声道相关性曲线
    QChartView* audio_corr_chart_;
    QChart* audio_corr_chart_object_;
    QLineSeries* audio_corr_series_;
    QValueAxis* audio_corr_axis_x_;
    QValueAxis* audio_corr_axis_y_;
    QTableWidget* audio_clip_table_;
    QTableWidget* audio_silence_table_;
    QTableWidget* audio_verdict_table_;
    QTableWidget* audio_metadata_table_;
    analyzer::AudioQcOptions audio_qc_options_;

    // 诊断与报告标签页
    QWidget* diagnostics_tab_;
    QPushButton* qc_start_button_;
    QPushButton* qc_cancel_button_;
    QPushButton* qc_export_button_;
    QProgressBar* qc_progress_bar_;
    QLabel* qc_summary_label_;
    QTabWidget* qc_sub_tabs_;
    QTableWidget* qc_issue_table_;
    QChartView* qc_chart_view_;
    QChart* qc_chart_object_;
    QLineSeries* qc_bitrate_series_;
    QLineSeries* qc_fps_series_;
    QValueAxis* qc_axis_x_;
    QValueAxis* qc_axis_bitrate_;
    QValueAxis* qc_axis_fps_;
    QTableWidget* qc_rule_table_;
    bool qc_rule_table_updating_ = false;

    // 时间轴与同步（诊断与报告页的子页）
    QWidget* timeline_sub_;
    QLabel* timeline_diag_summary_label_;
    QChartView* timeline_issue_chart_;
    QChart* timeline_issue_chart_object_;
    QLineSeries* timeline_interval_series_;
    QScatterSeries* timeline_marker_series_;
    QValueAxis* timeline_chart_axis_x_;
    QValueAxis* timeline_chart_axis_y_;
    QTableWidget* timeline_issue_table_;
    QVector<int> timeline_marker_issue_index_;   // 散点序号 -> 问题序号
    analyzer::TimelineAnalyzer timeline_analyzer_;
    model::TimelineAnalysisResult timeline_result_;
    bool timeline_dirty_ = false;
    bool timeline_offline_ = false;              // true = 数据来自全文件扫描

    analyzer::AnalysisCoordinator diagnostics_coordinator_;
    analyzer::QcRuleEngine qc_rule_engine_;
    analyzer::AnalysisResult diagnostics_result_;
    model::QcReport current_qc_report_;
    quint64 diagnostics_generation_ = 0;
    bool has_diagnostics_result_ = false;
    std::chrono::steady_clock::time_point diagnostics_start_time_;

    // 控制按钮
    QPushButton* export_button_;          // 流统计导出 (HTML/JSON/TXT)
    
    // 图表数据系列
    QLineSeries* sync_series_;
    QLineSeries* timeline_video_series_;
    QLineSeries* timeline_audio_series_;
    QLineSeries* timeline_event_series_;
    QValueAxis* sync_axis_x_;
    QValueAxis* sync_axis_y_;
    QValueAxis* timeline_axis_x_;
    QValueAxis* timeline_axis_y_;
    
    // 定时器
    QTimer* update_timer_;
    
    // 当前数据
    analyzer::StreamStats current_stats_;
    std::vector<VideoFrameRecord> frame_records_;
    std::vector<AudioFrameRecord> audio_frame_records_;
    std::vector<PacketRecord> packet_records_;
    std::vector<AnalysisEventRecord> analysis_event_records_;
    std::vector<SyncSampleRecord> sync_sample_records_;
    std::vector<TimelineEventRecord> timeline_event_records_;
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
    size_t frame_table_synced_record_count_ = 0;
    size_t gop_table_synced_count_ = 0;
    size_t audio_frame_table_synced_record_count_ = 0;
    size_t packet_table_synced_record_count_ = 0;
    size_t event_table_synced_record_count_ = 0;
    size_t sync_table_synced_record_count_ = 0;
    size_t timeline_table_synced_record_count_ = 0;
    std::deque<qreal> bitrate_chart_values_;
    std::deque<qreal> fps_chart_values_;
    std::deque<qreal> sync_chart_values_;

    bool scene_change_dirty_ = false;

    // 当前视频文件路径 (供导出报告使用)
    std::string current_video_path_;
};

} // namespace ui
} // namespace videoeye

// 注意: 跨线程信号/槽传递的元类型注册统一在 AnalysisPanel::SetupUI 中
// 通过 qRegisterMetaType<T>() 完成。Qt 6.8 的 QMetaType::fromType<T>() 会自动
// 为普通结构体生成元类型接口, 无需 (且不应) 在此处使用 Q_DECLARE_METATYPE,
// 否则会与 moc 触发的自动注册冲突 (C2908 "QMetaTypeId 已实例化")。
