#pragma once

#include <QWidget>
#include <QImage>
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
#include "ui/charts/MetricChartWidget.h"
#include "ui/reporting_panel/ReportingPanel.h"
#include "ui/AnalysisFacade.h"
#include "ui/analysis_panel/AnalysisPageSupport.h"
#include "ui/analysis_panel/ContainerStructurePage.h"
#include "ui/analysis_panel/SceneChangePage.h"
#include "ui/analysis_panel/BitrateGopPage.h"
#include "ui/analysis_panel/AudioQcPage.h"
#include "ui/analysis_panel/ColorHdrPage.h"
#include "ui/analysis_panel/SubtitleAuxPage.h"
#include "ui/analysis_panel/VisualDefectPage.h"
#include "ui/analysis_panel/DiagnosticsPage.h"
#include "ui/analysis_panel/EventTimelineView.h"
#include <chrono>
#include <deque>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <memory>

#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AudioVisualizationFrame.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineEvent.h"
#include "core/domain/model/Mp4BoxInfo.h"
#include "core/domain/model/EbmlInfo.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/MacroblockInfo.h"
#include "ui/bitstream_panel/BitstreamPanel.h"
#include "ui/streaming_panel/StreamingPanel.h"
#include "core/domain/model/FrameTimingInfo.h"
#include "core/domain/model/TimelineDiagnostic.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/AudioQcResult.h"
#include "core/domain/model/QualityMetric.h"
#include "core/domain/model/VisualDefect.h"
#include "core/domain/model/VisualDefectOptions.h"
#include "core/domain/model/StreamStats.h"
#include "core/domain/model/SubtitleCueInfo.h"
#include "core/domain/model/TimecodeInfo.h"
#include "core/domain/model/AuxiliaryDataInfo.h"

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
        Diagnostics,   // 诊断与报告 (全文件扫描 + QC 规则引擎)
        VisualDefect   // 画面质量 (黑场/冻结/马赛克/模糊/闪烁/曝光/色偏/梳齿/黑边)
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
        // 切换文件后重置时间轴累计状态 (避免上一文件的数据混入)；
        // 时间轴与 facade 都在诊断页里，由它自己清空
        if (diagnostics_page_) diagnostics_page_->ResetForNewFile();
        // 报告与批量 QC 页需要当前文件来"按模板重新分析"
        if (reporting_panel_) reporting_panel_->SetCurrentFile(path);
        // 拆出去的页面用当前路径拼导出 CSV 的默认文件名
        if (scene_change_page_) scene_change_page_->SetSourcePath(path);
        if (bitrate_gop_page_) bitrate_gop_page_->SetSourcePath(path);
        if (audio_qc_page_) audio_qc_page_->SetSourcePath(path);
        if (color_hdr_page_) color_hdr_page_->SetSourcePath(path);
        // 扫描总控在诊断页，换文件要同步它手里的当前路径
        if (diagnostics_page_) diagnostics_page_->SetSourcePath(path);
        // 场景切换记录是播放期的累计结果，换文件必须清零（与帧/包那几页一致）
        if (scene_change_page_) scene_change_page_->Reset();
    }
    
    // 重新发射所有启用状态的开关信号 (用于文件打开后同步播放器状态)
    void EmitInitialFeatureStates();

    // 用面板当前的诊断选项对 current_video_path_ 发起全文件诊断扫描。
    // 播放器打开失败进入"分析模式"时由 MainWindow 调用, 让诊断页展示具体错误原因。
    void StartDiagnosticsScanForCurrentFile();
    
signals:
    // 分析功能开关变化信号 (供 MainWindow 连接 MediaPlayer)
    void AnalysisFeatureToggled(int feature, bool enabled);

    // 画面质量分析的采样档位 / 阈值变化 (供 MainWindow 转发给 MediaPlayer)
    void VisualDefectOptionsChanged(const model::VisualDefectOptions& options);

    // 跳转到指定时间（秒）: 由"跳转到问题帧"触发, MainWindow 连接到播放器 Seek
    void SeekRequested(double seconds);

    // 全文件扫描拿到素材自带起始时码（tmcd 轨 / metadata timecode tag）时发出，
    // MainWindow 转给 PlayerPanel，让播放器时间轴旁显示真实 SMPTE 时码。
    void StartTimecodeReady(const QString& timecode, double fps);

    // 报告与批量 QC 页把状态（"已导出报告…""模板已加载"等）冒泡给主窗口状态栏
    void StatusMessage(const QString& text);
    
public slots:
    // 更新统计数据
    void UpdateStreamStats(const model::StreamStats& stats);

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
    void OnSceneChangeDetected(const model::SceneChangeResult& result);

    // 画面质量 / 视觉缺陷（播放时逐采样帧产出）
    void OnVisualDefectFrame(const model::FrameQualityMetric& metric);
    void OnVisualDefectDetected(const model::VisualDefect& defect);
    void OnVisualDefectReset();
    void OnVisualDefectStats(int analyzed_frames, int dropped_frames,
                             const model::ActivePictureArea& effective_area);
    void OnVisualDefectOptionChanged();

    // 参数集页「重新扫描」：复用同一次全文件扫描
    void OnBitstreamRefreshRequested();

    // 导出报告
    void OnExportReport();

    // 扫描生命周期由 DiagnosticsPage 编排，面板只把进度同步给其它几页
    void OnScanStarted();
    void OnScanCancelled();
    void OnDiagnosticsProgress(double percent, const QString& stage);
    void OnDiagnosticsFinished(bool completed);

    // 时间轴与同步诊断（播放实时数据 → 转交诊断页）
    void OnTimelinePacket(const model::PacketTiming& timing);
    void OnFrameTiming(const model::FrameTimingInfo& timing);

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

    // 初始化UI
    void SetupUI();
    // 将页面包裹 QScrollArea 并添加到页面列表
    void AddPageWithScroll(QWidget* tab_widget, const QString& title);
    void SetupStreamTab();
    void SetupFrameTab();
    void SetupPacketTab();
    void SetupBitstreamTab();
    void SetupMacroblockTab();
    // 编码参数集（SPS/PPS/VPS/Sequence Header）解析页，与「码流分析」页是两回事：
    // 那一页看的是包/帧/码率，这一页看的是 extradata 里的编码参数。
    void SetupParameterSetTab();
    // HLS / DASH 流媒体包页（manifest + segment + 多码率 ladder）
    void SetupStreamingPackageTab();
    // 诊断与报告页（全文件扫描 + QC 规则引擎 + 时间轴/同步诊断）：扫描总控已在页面内，
    // 面板这一层只负责建页、把页面意图接到全局，以及把进度同步给共用同一次扫描的几页。
    void SetupDiagnosticsPage();
    // 报告与批量 QC 页（功能 12）：模板选择 / 单文件报告 / 目录批量扫描 / 导出
    void SetupReportingPanelTab();
    // 已拆成独立页面组件的四个页，在这里建好并接上信号（本体是 QWidget，
    // 直接交给 AddPageWithScroll 变成外部 stack 的一页）。
    void SetupContainerStructurePage();
    void SetupSceneChangePage();
    void SetupBitrateGopPage();
    void SetupAudioQcPage();
    void SetupColorHdrPage();
    void SetupSubtitleAuxPage();
    // 事件 / 同步 / 时间轴三表聚合页：本体是 EventTimelineView，建页后注入 feature 钩子，
    // 诊断页指针在 SetupUI 末尾注入（AppendSyncSample 需要喂同步样本给诊断页）。
    void SetupEventTimelineView();
    void RebuildFrameTable();
    void RebuildGopTable();
    void RebuildAudioFrameTable();
    void RebuildPacketTable();
    void UpdateFrameSummary();
    void UpdateAudioFrameSummary();
    void UpdatePacketSummary();
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
    void RefreshStreamStatsUi(const model::StreamStats& stats);
    void SetTableItemText(QTableWidget* table, int row, int column, const QString& text);
    void AppendFrameRowToTable(const VideoFrameRecord& record);
    void AppendAudioFrameRowToTable(const AudioFrameRecord& record);
    void AppendPacketRowToTable(const PacketRecord& record);
    void UpdateGopRowInTable(int row, const GopSummary& summary);
    void OnExportFrameCsv();
    void OnExportAudioFrameCsv();
    void OnExportGopCsv();
    void OnExportPacketCsv();
    void OnExportMp4Box();
    void OnFrameFilterChanged();
    void RefreshMacroblockUi();
    void OnExportMacroblockCsv();

    // 画面质量 / 视觉缺陷（已拆成独立的 VisualDefectPage）
    void SetupVisualDefectPage();
    // 「关联场景切换」：页面不持有 facade，这一步由面板编排（重算 QC + 刷新问题表）
    void OnSceneLinkRequested(const std::vector<model::SceneChangeResult>& records,
                              const analyzer::BitrateGopOptions& options);

    void UpdateBitstreamUi();         // 参数集页：结构树 + 容器/码流对比 + 不一致表
    // 字幕的「过短 / 过长 / 阅读速度」阈值以「规则与阈值」那张可编辑的表为准，
    // 每次扫描前同步一次，避免选项与规则两处阈值各说各话。
    void SyncSubtitleThresholds(analyzer::AnalysisOptions& options);
    void OnStreamingRefreshRequested();
    void UpdateStreamingUi();         // 流媒体包页：结构树 + ladder + 分片时间轴 + 问题

    // 更新图表
    void UpdateBitrateChart(const model::StreamStats& stats);
    void UpdateFPSChart(const model::StreamStats& stats);
    // GOP 曲线数据源为 UI 侧 gop_summaries_ (统一口径, 见 RebuildGopTable)
    void UpdateGOPChart();
    void ResetStreamCharts();
    
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

    // 场景切换检测页（已拆成独立的 SceneChangePage）
    SceneChangePage* scene_change_page_ = nullptr;

    // 画面质量 / 视觉缺陷页（已拆成独立的 VisualDefectPage）
    VisualDefectPage* visual_defect_page_ = nullptr;
    // 面板仍持有这一份选项，作为新页面的初值与「重新发射开关」时的基准
    model::VisualDefectOptions visual_defect_options_;

    // 统一文件结构分析标签页（已拆成独立的 ContainerStructurePage）
    ContainerStructurePage* container_page_ = nullptr;

    // 码率与 GOP 深度分析页（已拆成独立的 BitrateGopPage）
    BitrateGopPage* bitrate_gop_page_ = nullptr;
    // 诊断与报告页（已拆成独立的 DiagnosticsPage：扫描总控 + 问题表 + 规则表 + 时间轴）
    DiagnosticsPage* diagnostics_page_ = nullptr;

    // 音频 QC 标签页（已拆成独立的 AudioQcPage）
    AudioQcPage* audio_qc_page_ = nullptr;

    // 色彩与 HDR 标签页（已拆成独立的 ColorHdrPage）
    ColorHdrPage* color_hdr_page_ = nullptr;

    // 编码参数集解析页（SPS / PPS / VPS / AV1 Sequence Header）
    BitstreamPanel* bitstream_params_panel_ = nullptr;

    // 流媒体包页（HLS / DASH 清单 + 分片 + 码率阶梯）
    StreamingPanel* streaming_panel_ = nullptr;

    // 字幕 / 时码 / 辅助数据页（已拆成独立的 SubtitleAuxPage）
    SubtitleAuxPage* subtitle_aux_page_ = nullptr;

    // 事件 / 同步 / 时间轴三表聚合页（从 AnalysisPanel 拆出的 EventTimelineView）
    EventTimelineView* event_timeline_view_ = nullptr;

    // 报告与批量 QC 页（功能 12）：模板选择 / 单文件报告 / 目录批量扫描 / 导出
    ui::ReportingPanel* reporting_panel_ = nullptr;

    // 控制按钮
    QPushButton* export_button_;          // 流统计导出 (HTML/JSON/TXT)
    
    // 定时器
    QTimer* update_timer_;
    
    // 当前数据
    model::StreamStats current_stats_;
    std::vector<VideoFrameRecord> frame_records_;
    std::vector<AudioFrameRecord> audio_frame_records_;
    std::vector<PacketRecord> packet_records_;
    std::vector<GopSummary> gop_summaries_;
    model::StreamStats pending_stream_stats_;
    bool has_pending_stream_stats_ = false;
    bool frame_table_dirty_ = false;
    bool gop_table_dirty_ = false;
    bool frame_summary_dirty_ = false;
    bool audio_frame_table_dirty_ = false;
    bool audio_frame_summary_dirty_ = false;
    bool packet_table_dirty_ = false;
    bool packet_summary_dirty_ = false;
    size_t frame_table_synced_record_count_ = 0;
    size_t gop_table_synced_count_ = 0;
    size_t audio_frame_table_synced_record_count_ = 0;
    size_t packet_table_synced_record_count_ = 0;
    std::deque<qreal> bitrate_chart_values_;
    std::deque<qreal> fps_chart_values_;

    // 当前视频文件路径 (供导出报告使用)
    std::string current_video_path_;
};

} // namespace ui
} // namespace videoeye

// 注意: 跨线程信号/槽传递的元类型注册统一在 AnalysisPanel::SetupUI 中
// 通过 qRegisterMetaType<T>() 完成。Qt 6.8 的 QMetaType::fromType<T>() 会自动
// 为普通结构体生成元类型接口, 无需 (且不应) 在此处使用 Q_DECLARE_METATYPE,
// 否则会与 moc 触发的自动注册冲突 (C2908 "QMetaTypeId 已实例化")。
