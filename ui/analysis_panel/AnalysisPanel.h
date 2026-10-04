#pragma once

// 分析面板 = 各分析页的协调层。
//
// 面板自己不再持有明细数据与表格控件：各页（含「码流分析」拆出的
// StreamOverviewView / FramePacketView、「宏块分析」拆出的 MacroblockView）都是
// 独立的 QWidget 组件，面板只负责
//   1. 建页并把它们交给外部 QStackedWidget（AddPageWithScroll / PopulateStackedWidget）；
//   2. 注入 feature 钩子，把页面内部的开关编号映射到全局 AnalysisFeature 并转发信号；
//   3. 播放期把播放器回吐的数据按开关过滤后转发给对应页；
//   4. 全文件扫描结束后把同一份结果分发给共用该次扫描的几页。
// 页面之间不互相持有指针，跨页数据一律经面板接线（如 FramePacketView 的 GOP 摘要
// → StreamOverviewView）。

#include <QList>
#include <QMap>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <string>
#include <vector>

#include "core/domain/model/AnalysisEvent.h"
#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/ContainerStructureInfo.h"
#include "core/domain/model/FrameTimingInfo.h"
#include "core/domain/model/MacroblockInfo.h"
#include "core/domain/model/PacketInfo.h"
#include "core/domain/model/QualityMetric.h"
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/StreamStats.h"
#include "core/domain/model/SyncSample.h"
#include "core/domain/model/TimelineDiagnostic.h"
#include "core/domain/model/TimelineEvent.h"
#include "core/domain/model/VisualDefect.h"
#include "core/domain/model/VisualDefectOptions.h"

#include "ui/AnalysisFacade.h"
#include "ui/bitstream_panel/BitstreamPanel.h"
#include "ui/reporting_panel/ReportingPanel.h"
#include "ui/streaming_panel/StreamingPanel.h"

#include "ui/analysis_panel/AudioQcPage.h"
#include "ui/analysis_panel/BitrateGopPage.h"
#include "ui/analysis_panel/ColorHdrPage.h"
#include "ui/analysis_panel/ContainerStructurePage.h"
#include "ui/analysis_panel/DiagnosticsPage.h"
#include "ui/analysis_panel/EventTimelineView.h"
#include "ui/analysis_panel/FramePacketView.h"
#include "ui/analysis_panel/MacroblockView.h"
#include "ui/analysis_panel/SceneChangePage.h"
#include "ui/analysis_panel/StreamOverviewView.h"
#include "ui/analysis_panel/SubtitleAuxPage.h"
#include "ui/analysis_panel/VisualDefectPage.h"

namespace videoeye {
namespace ui {

// 分析面板类
class AnalysisPanel : public QWidget {
    Q_OBJECT
    
public:
    // 分析功能开关枚举
    //
    // 已下放到 core/domain/model/AnalysisFeature.h：MediaPlayer 现在直接认这个枚举
    // （SetAnalysisFeature），面板与播放器说的是同一种语言，MainWindow 不再需要
    // 一个 10 分支的 switch 做翻译。
    // 这里是类内别名，为了让既有代码里的 AnalysisPanel::AnalysisFeature 继续可用
    // （面板内部与页面组件共 70+ 处引用），不用挨个改。
    using AnalysisFeature = model::AnalysisFeature;

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
    // 以下播放期回吐的槽都只做两件事：套用 Master/功能开关的过滤，再转发给
    // 对应的页面组件。数据本身不再由面板持有（评审 P2：面板退化为协调层）。
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
    // 结构分析没出结果（解析失败 / 分析器抛异常）
    void OnContainerStructureFailed(const QString& message);
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

    // 扫描生命周期由 DiagnosticsPage 编排，面板只把进度同步给其它几页
    void OnScanStarted();
    void OnDiagnosticsProgress(double percent, const QString& stage);
    // 唯一的扫描终态收尾: 恢复共用页的按钮/进度（三种终态一视同仁），
    // 非失败时再把同一份结果分发下去。不再有"成功/取消各走一半"的分裂路径。
    void OnScanEnded(DiagnosticsPage::ScanEndReason reason);

    // 时间轴与同步诊断（播放实时数据 → 转交诊断页）
    void OnTimelinePacket(const model::PacketTiming& timing);
    void OnFrameTiming(const model::FrameTimingInfo& timing);

private:
    // 初始化UI
    void SetupUI();
    // 将页面包裹 QScrollArea 并添加到页面列表
    void AddPageWithScroll(QWidget* tab_widget, const QString& title);
    // 「码流分析」页：把流概览（StreamOverviewView）与帧/包明细（FramePacketView）
    // 两个组件拼成一页（顶部固定 + 分隔线 + 底部可伸展），并把 GOP 摘要从产出方
    // 桥接到消费方。
    void SetupBitstreamTab();
    // 宏块分析页：本体是 MacroblockView，建页后注入 feature 钩子。
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
    // 已拆成独立页面组件的页，在这里建好并接上信号（本体是 QWidget，
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
    void FlushPendingUiUpdates();

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
    
    // 分析功能开关
    QMap<AnalysisFeature, bool> feature_enabled_;
    
    // 页面管理 (替代 QTabWidget)
    QList<QWidget*> page_widgets_;      // 各页面的 QScrollArea (含内容)
    QStringList page_titles_;           // 各页面标题
    QStackedWidget* external_stack_ = nullptr;  // 外部 QStackedWidget (由 MainWindow 提供)

    // 「码流分析」页拆成的两个组件：顶部流概览 + 底部帧/包明细。
    // 两者在 SetupBitstreamTab 里拼进同一页；GOP 摘要在后者产出、前者消费，
    // 由面板在 SetupBitstreamTab 末尾把信号接起来（两个组件互不认识）。
    StreamOverviewView* stream_overview_view_ = nullptr;
    FramePacketView* frame_packet_view_ = nullptr;

    // 宏块分析页（已拆成独立的 MacroblockView）
    MacroblockView* macroblock_view_ = nullptr;

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

    // 定时器
    QTimer* update_timer_;

    // 当前视频文件路径 (供导出报告使用)
    std::string current_video_path_;
};

} // namespace ui
} // namespace videoeye

// 注意: 跨线程信号/槽传递的元类型注册统一在 AnalysisPanel::SetupUI 中
// 通过 qRegisterMetaType<T>() 完成。Qt 6.8 的 QMetaType::fromType<T>() 会自动
// 为普通结构体生成元类型接口, 无需 (且不应) 在此处使用 Q_DECLARE_METATYPE,
// 否则会与 moc 触发的自动注册冲突 (C2908 "QMetaTypeId 已实例化")。
