#include "ui/analysis_panel/AnalysisPanel.h"

#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"
// OnSceneLinkRequested 的形参用到 videoeye::BitrateGopOptions（关联场景切换的判定表），
// 面板只把这份记录转交给诊断页，不自己算。
// 这个结构体住在 AnalysisOptions.h —— 以前这里 include 的是具体实现它的
// BitrateGopAnalyzer.h，等于为了一个参数把整个码率分析器拖进面板的编译图。
#include "core/analysis/AnalysisOptions.h"

#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>

#include <chrono>
#include <string>
#include <vector>

namespace videoeye {
namespace ui {

namespace {
constexpr int kUiFlushIntervalMs = 120;
}  // namespace

// 共用同一次全文件扫描的页面接线：入注册表 + 把「开始/取消」意图接到诊断页总控。
// 以前三页各写一份（连信号 + FillScanOptions + StartScan），差异只有 FillScanOptions
// 的具体内容 —— 那由页面自己在 ScanClient 实现里决定。
template <typename PageT>
void AnalysisPanel::RegisterScanPage(PageT* page) {
    scan_clients_.push_back(page);
    connect(page, &PageT::ScanRequested, this, [this, page]() {
        if (!diagnostics_page_) return;
        videoeye::AnalysisOptions options = diagnostics_page_->options();
        page->FillScanOptions(options);
        diagnostics_page_->StartScan(options);
    });
    connect(page, &PageT::CancelRequested, this, [this]() {
        if (diagnostics_page_) diagnostics_page_->CancelScan();
    });
}

AnalysisPanel::AnalysisPanel(QWidget* parent)
    : QWidget(parent) {
    
    // 默认启用: 基础功能, 关闭: 高性能分析
    feature_enabled_[AnalysisFeature::Master] = true;
    feature_enabled_[AnalysisFeature::StreamStats] = true;
    feature_enabled_[AnalysisFeature::VideoFrame] = true;
    feature_enabled_[AnalysisFeature::AudioFrame] = false;
    feature_enabled_[AnalysisFeature::Packet] = false;
    feature_enabled_[AnalysisFeature::Event] = false;
    feature_enabled_[AnalysisFeature::SyncSample] = false;
    feature_enabled_[AnalysisFeature::Timeline] = false;
    feature_enabled_[AnalysisFeature::ContainerStructure] = true;
    feature_enabled_[AnalysisFeature::Macroblock] = false;
    feature_enabled_[AnalysisFeature::SceneChange] = false;
    feature_enabled_[AnalysisFeature::Diagnostics] = true;
    // 画面质量默认关: 播放时要额外做降采样与边缘统计，属于按需开启的重活
    feature_enabled_[AnalysisFeature::VisualDefect] = false;

    SetupUI();
    
    update_timer_ = new QTimer(this);
    connect(update_timer_, &QTimer::timeout, this, &AnalysisPanel::FlushPendingUiUpdates);
    update_timer_->start(kUiFlushIntervalMs);
    
    LOG_INFO("分析面板已初始化");
}

AnalysisPanel::~AnalysisPanel() {
    LOG_INFO("分析面板已销毁");
}


bool AnalysisPanel::IsFeatureEnabled(AnalysisFeature feature) const {
    return feature_enabled_.value(feature, true);
}

void AnalysisPanel::EmitInitialFeatureStates() {
    // 对每个启用状态的 feature 重新发射信号 (除了 Master 和 Mp4Box)
    static const AnalysisFeature kFeatures[] = {
        AnalysisFeature::StreamStats,
        AnalysisFeature::VideoFrame,
        AnalysisFeature::AudioFrame,
        AnalysisFeature::Packet,
        AnalysisFeature::Event,
        AnalysisFeature::SyncSample,
        AnalysisFeature::Timeline,
        AnalysisFeature::Macroblock,
        AnalysisFeature::VisualDefect,
    };
    for (auto feat : kFeatures) {
        bool enabled = feature_enabled_.value(feat, true);
        emit AnalysisFeatureToggled(static_cast<int>(feat), enabled);
    }
}


void AnalysisPanel::AddPageWithScroll(QWidget* tab_widget, const QString& title) {
    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(tab_widget);
    scroll->setProperty("pageTitle", title);  // MainWindow 用它生成侧边栏条目
    page_widgets_.append(scroll);
    page_titles_.append(title);
}

int AnalysisPanel::PopulateStackedWidget(QStackedWidget* stack) {
    external_stack_ = stack;
    for (int i = 0; i < page_widgets_.size(); ++i) {
        stack->addWidget(page_widgets_[i]);
    }
    return page_widgets_.size();
}

void AnalysisPanel::SetCurrentPageIndex(int index) {
    if (external_stack_ && index >= 0 && index < page_widgets_.size()) {
        // 外部 stack 的 page 0 是媒体信息，分析页从 page 1 开始
        external_stack_->setCurrentIndex(index + 1);
    }
}

// ===========================================================================
// 「码流分析」页：流概览 + 帧/包明细
//
// 原先这一页（含 5 张表、3 条曲线、4 个子 Tab、记录缓存与增量刷新）全塞在面板里。
// 现在拆成两个组件，面板只负责三件事：
//   1. 把两块内容拼成一页（顶部固定、分隔条、底部可伸展）；
//   2. 注入 feature 钩子（视图内只有自己的编号，映射与信号转发在面板）；
//   3. 把 GOP 摘要从产出方（FramePacketView）桥到消费方（StreamOverviewView）。
// ===========================================================================

void AnalysisPanel::SetupBitstreamTab() {
    stream_overview_view_ = new StreamOverviewView(this);
    frame_packet_view_ = new FramePacketView(this);
    frame_packet_view_->setMinimumHeight(360);

    // 流统计开关（视图内编号 0）-> AnalysisFeature::StreamStats
    stream_overview_view_->SetFeatureHooks(
        [this](int) {
            return feature_enabled_.value(AnalysisFeature::StreamStats, true);
        },
        [this](int, bool enabled) {
            feature_enabled_[AnalysisFeature::StreamStats] = enabled;
            emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::StreamStats), enabled);
        });

    // 视频帧 / 音频帧 / 数据包开关（视图内编号 0/1/2）-> AnalysisFeature
    frame_packet_view_->SetFeatureHooks(
        [this](int view_feature) {
            switch (view_feature) {
                case 0: return feature_enabled_.value(AnalysisFeature::VideoFrame, true);
                case 1: return feature_enabled_.value(AnalysisFeature::AudioFrame, true);
                case 2: return feature_enabled_.value(AnalysisFeature::Packet, true);
                default: return true;
            }
        },
        [this](int view_feature, bool enabled) {
            AnalysisFeature feat = AnalysisFeature::VideoFrame;
            switch (view_feature) {
                case 0: feat = AnalysisFeature::VideoFrame; break;
                case 1: feat = AnalysisFeature::AudioFrame; break;
                case 2: feat = AnalysisFeature::Packet; break;
                default: return;
            }
            feature_enabled_[feat] = enabled;
            emit AnalysisFeatureToggled(static_cast<int>(feat), enabled);
        });

    // GOP 摘要由「帧/包明细」从解码帧 pict_type 推导产出，「流概览」区拿它填
    // 「最大GOP大小」并画「GOP 帧数分布」曲线。两个视图互不认识，只在面板连一次
    // —— 直接互相持有指针会让两者无法单独构造 / 单独测试。
    connect(frame_packet_view_, &FramePacketView::GopSummariesChanged, this, [this]() {
        if (stream_overview_view_ && frame_packet_view_) {
            stream_overview_view_->SetGopSummaries(frame_packet_view_->GopSummaries());
        }
    });

    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 顶部流概览区 (固定高度不可滚动, 让曲线一直可见)
    layout->addWidget(stream_overview_view_);

    // 分隔条
    QFrame* sep = new QFrame(page);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    // 底部 4 子 Tab 区 (可伸展, 占剩余高度)
    layout->addWidget(frame_packet_view_, 1);

    AddPageWithScroll(page, tr("码流分析"));
}

void AnalysisPanel::SetupMacroblockTab() {
    macroblock_view_ = new MacroblockView(this);
    // 宏块开关（视图内编号 0）-> AnalysisFeature::Macroblock
    macroblock_view_->SetFeatureHooks(
        [this](int) {
            return feature_enabled_.value(AnalysisFeature::Macroblock, false);
        },
        [this](int, bool enabled) {
            feature_enabled_[AnalysisFeature::Macroblock] = enabled;
            emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::Macroblock), enabled);
        });

    AddPageWithScroll(macroblock_view_, tr("宏块分析"));
}

// ===========================================================================
// 播放期数据回吐：面板只做「开关过滤 + 转发」，数据本身归各页面组件
// ===========================================================================

void AnalysisPanel::UpdateStreamStats(const model::StreamStats& stats) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::StreamStats, true)) return;
    if (stream_overview_view_) stream_overview_view_->SetStreamStats(stats);
}

void AnalysisPanel::ResetVideoFrameList() {
    // 手动清空不受 feature 开关约束：以前被 Master/VideoFrame 挡着，关掉开关后
    // 用户点「清空」没反应（数据还在，看起来像坏了）。开关只管"要不要收新数据"，
    // 不兼管"用户想不想清"。
    if (frame_packet_view_) frame_packet_view_->ResetVideoFrames();
    // 换文件时曲线也要清零：原先由面板自己 reset，现在曲线归流概览区。
    // （GOP 相关的那一份数据由 FramePacketView::ResetVideoFrames 清空后
    //   经 GopSummariesChanged 通知，两边不会各清一半。）
    if (stream_overview_view_) stream_overview_view_->ResetCharts();
}

void AnalysisPanel::AppendVideoFrameInfo(int index, int frame_type, bool is_key_frame,
                                         qint64 pts, double timestamp_seconds) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::VideoFrame, true)) return;
    if (frame_packet_view_) {
        frame_packet_view_->AppendVideoFrame(index, frame_type, is_key_frame, pts, timestamp_seconds);
    }
}

void AnalysisPanel::ResetAudioFrameList() {
    // 同 ResetVideoFrameList：清空不收 feature 开关的约束
    if (frame_packet_view_) frame_packet_view_->ResetAudioFrames();
}

void AnalysisPanel::AppendAudioFrameInfo(int index, qint64 pts, double timestamp_seconds,
                                         int sample_count, int sample_rate, int channels, int byte_count) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::AudioFrame, true)) return;
    if (frame_packet_view_) {
        frame_packet_view_->AppendAudioFrame(index, pts, timestamp_seconds,
                                             sample_count, sample_rate, channels, byte_count);
    }
}

void AnalysisPanel::ResetPacketList() {
    // 同 ResetVideoFrameList：清空不收 feature 开关的约束
    if (frame_packet_view_) frame_packet_view_->ResetPackets();
}

void AnalysisPanel::AppendPacketInfo(const model::PacketInfo& packet_info) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Packet, true)) return;
    if (frame_packet_view_) frame_packet_view_->AppendPacket(packet_info);
}

void AnalysisPanel::UpdateMacroblockInfo(const model::MacroblockFrameAnalysis& analysis) {
    if (macroblock_view_) macroblock_view_->SetAnalysis(analysis);
}

void AnalysisPanel::ResetAnalysisEventList() {
    if (event_timeline_view_) event_timeline_view_->ResetAnalysisEventList();
}


void AnalysisPanel::ResetSyncSampleList() {
    // 只复位时间轴视图内的同步表/曲线；诊断页的音视频偏移累积随换文件由
    // SetCurrentVideoPath -> DiagnosticsPage::ResetForNewFile 统一清零，这里不重复复位。
    if (event_timeline_view_) event_timeline_view_->ResetSyncSampleList();
}


void AnalysisPanel::ResetTimelineEventList() {
    if (event_timeline_view_) event_timeline_view_->ResetTimelineEventList();
}


void AnalysisPanel::AppendAnalysisEvent(const model::AnalysisEvent& event_info) {
    if (event_timeline_view_) event_timeline_view_->AppendAnalysisEvent(event_info);
}


void AnalysisPanel::AppendSyncSample(const model::SyncSample& sample) {
    // 同步样本统一在这里直发给两个消费方：视图建同步表/曲线，诊断页建音视频偏移曲线。
    // 视图不再经信号中继（评审 P1：收敛同步链，避免一次样本绕两跳）。
    if (event_timeline_view_) event_timeline_view_->AppendSyncSample(sample);
    if (diagnostics_page_) {
        diagnostics_page_->OnSyncSample(sample.audio_timestamp_seconds * 1000.0,
                                        sample.video_timestamp_seconds * 1000.0);
    }
}


void AnalysisPanel::AppendTimelineEvent(const model::TimelineEvent& event) {
    if (event_timeline_view_) event_timeline_view_->AppendTimelineEvent(event);
}

// ===========================================================================
// 建页：参数集 / 流媒体包 / 字幕辅助 / 诊断与报告 / 报告与批量 QC
// ===========================================================================

void AnalysisPanel::SetupStreamingPackageTab() {
    // 页面本体就是 StreamingPanel：manifest 结构树 + 码率阶梯表 +
    // 分片时间轴对齐表 + 问题表都封装在里面。
    streaming_panel_ = new StreamingPanel(this);
    streaming_panel_->setMinimumHeight(560);

    connect(streaming_panel_, &StreamingPanel::RefreshRequested,
            this, &AnalysisPanel::OnStreamingRefreshRequested);

    AddPageWithScroll(streaming_panel_, tr("流媒体包"));
}

void AnalysisPanel::OnStreamingRefreshRequested() {
    // 清单解析是纯本地文件读取（第一阶段不联网），跟着全文件扫描一起跑。
    // 扫描总控在诊断页，这里只改一个开关再交给它跑
    diagnostics_page_->options().analyze_streaming_package = true;
    diagnostics_page_->StartScan(diagnostics_page_->options());
}

void AnalysisPanel::UpdateStreamingUi() {
    if (!streaming_panel_) return;
    // 优先用容器结构分析的结果（打开清单文件时由 ContainerStructureAnalyzer 直接产出）；
    // 没有的话再退到全文件扫描的 streaming_package。
    if (container_page_ && container_page_->result().streaming_package.valid) {
        streaming_panel_->SetResult(container_page_->result().streaming_package);
        return;
    }
    if (diagnostics_page_ && diagnostics_page_->hasResult() &&
        diagnostics_page_->result().streaming_analyzed) {
        streaming_panel_->SetResult(diagnostics_page_->result().streaming_package);
        return;
    }
    streaming_panel_->Clear();
}

void AnalysisPanel::SetupParameterSetTab() {
    // 页面本体就是 BitstreamPanel：结构树 + 容器/码流对比 + 不一致表都封装在里面。
    // 外面套的是 AddPageWithScroll 的 QScrollArea，给个最小高度免得被压扁。
    bitstream_params_panel_ = new BitstreamPanel(this);
    bitstream_params_panel_->setMinimumHeight(560);

    connect(bitstream_params_panel_, &BitstreamPanel::RefreshRequested,
            this, &AnalysisPanel::OnBitstreamRefreshRequested);

    AddPageWithScroll(bitstream_params_panel_, tr("参数集解析"));
}

void AnalysisPanel::OnBitstreamRefreshRequested() {
    // 码流解析只读 extradata（KB 级），成本可忽略，跟着全文件扫描一起跑。
    diagnostics_page_->options().analyze_bitstream = true;
    diagnostics_page_->StartScan(diagnostics_page_->options());
}

void AnalysisPanel::UpdateBitstreamUi() {
    if (!bitstream_params_panel_) return;
    if (!diagnostics_page_ || !diagnostics_page_->hasResult() ||
        !diagnostics_page_->result().bitstream_analyzed) {
        bitstream_params_panel_->Clear();
        return;
    }
    bitstream_params_panel_->SetResult(diagnostics_page_->result().bitstream_analysis);
}

// 播放器打开失败进入「分析模式」时的自动扫描入口：用诊断页的默认选项跑一次。
// 静默失败路径 —— 不弹"请先打开文件"提示框，原因写进诊断页自己的汇总标签。
void AnalysisPanel::StartDiagnosticsScanForCurrentFile() {
    if (!diagnostics_page_ || current_video_path_.empty()) return;
    // 静默路径：打开失败后自动补扫一次，不能弹窗打断"打开即分析"的流程
    diagnostics_page_->StartScan(diagnostics_page_->options(), /*silent=*/true);
}

// ---------------------------------------------------------------------------
// 播放期时间轴统计：数据归诊断页（TimelineAnalyzer 也在那里），面板只转发
// ---------------------------------------------------------------------------

void AnalysisPanel::OnTimelinePacket(const model::PacketTiming& timing) {
    if (diagnostics_page_) diagnostics_page_->OnPacketTiming(timing);
}

void AnalysisPanel::OnFrameTiming(const model::FrameTimingInfo& timing) {
    if (diagnostics_page_) diagnostics_page_->OnFrameTiming(timing);
}

// ===========================================================================
// 已拆成独立页面组件的部分
//
// 这一节只做三件事：建页 + 把页面发出的意图接到面板（面板才知道当前文件路径 /
// 扫描编排 / 全局 feature 表），把播放器回来的数据喂给页面，以及全文件扫描结束
// 时把结果分发给共用同一次扫描的几页。页面本身不持有 facade、不认识面板成员。
// ===========================================================================

void AnalysisPanel::SetupUI() {
    // 不再创建内部 QTabWidget，页面由 AddPageWithScroll 收集
    // PopulateStackedWidget 时添加到外部 QStackedWidget

    SetupBitstreamTab();
    SetupEventTimelineView();
    SetupContainerStructurePage();
    SetupMacroblockTab();
    SetupSceneChangePage();
    SetupVisualDefectPage();
    SetupBitrateGopPage();
    SetupAudioQcPage();
    SetupColorHdrPage();
    SetupParameterSetTab();
    SetupStreamingPackageTab();
    SetupSubtitleAuxPage();
    SetupDiagnosticsPage();
    SetupReportingPanelTab();

    qRegisterMetaType<model::SceneChangeResult>();
    qRegisterMetaType<model::AnalysisResult>();
    qRegisterMetaType<model::FrameQualityMetric>();
    qRegisterMetaType<model::VisualDefect>();
    qRegisterMetaType<model::VisualDefectOptions>();
}

void AnalysisPanel::SetupEventTimelineView() {
    event_timeline_view_ = new EventTimelineView(this);

    // 视图内部用 0/1/2 表示 Event/Sync/Timeline，这里映射到面板的 AnalysisFeature 并回写
    // feature_enabled_，同时转发 AnalysisFeatureToggled，保证 MainWindow 的连接行为不变。
    event_timeline_view_->SetFeatureHooks(
        [this](int view_feature) {
            switch (view_feature) {
                case 0: return feature_enabled_.value(AnalysisFeature::Event, true);
                case 1: return feature_enabled_.value(AnalysisFeature::SyncSample, true);
                case 2: return feature_enabled_.value(AnalysisFeature::Timeline, true);
                default: return true;
            }
        },
        [this](int view_feature, bool enabled) {
            AnalysisFeature feat = AnalysisFeature::Event;
            switch (view_feature) {
                case 0: feat = AnalysisFeature::Event; break;
                case 1: feat = AnalysisFeature::SyncSample; break;
                case 2: feat = AnalysisFeature::Timeline; break;
                default: return;
            }
            feature_enabled_[feat] = enabled;
            emit AnalysisFeatureToggled(static_cast<int>(feat), enabled);
        });

    AddPageWithScroll(event_timeline_view_, tr("事件与时间轴"));
}

void AnalysisPanel::SetupContainerStructurePage() {
    container_page_ = new ContainerStructurePage(
        feature_enabled_.value(AnalysisFeature::ContainerStructure, true), this);
    connect(container_page_, &ContainerStructurePage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::ContainerStructure] = enabled;
                emit AnalysisFeatureToggled(
                    static_cast<int>(AnalysisFeature::ContainerStructure), enabled);
            });
    AddPageWithScroll(container_page_, tr("文件结构"));
}

void AnalysisPanel::SetupSceneChangePage() {
    scene_change_page_ = new SceneChangePage(
        feature_enabled_.value(AnalysisFeature::SceneChange, false), this);
    connect(scene_change_page_, &SceneChangePage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::SceneChange] = enabled;
                emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::SceneChange), enabled);
            });

    AddPageWithScroll(scene_change_page_, tr("场景切换"));
}

void AnalysisPanel::SetupVisualDefectPage() {
    visual_defect_page_ = new VisualDefectPage(
        feature_enabled_.value(AnalysisFeature::VisualDefect, false), visual_defect_options_, this);
    connect(visual_defect_page_, &VisualDefectPage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::VisualDefect] = enabled;
                emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::VisualDefect), enabled);
            });
    connect(visual_defect_page_, &VisualDefectPage::OptionsChanged,
            this, &AnalysisPanel::VisualDefectOptionsChanged);
    connect(visual_defect_page_, &VisualDefectPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    AddPageWithScroll(visual_defect_page_, tr("画面质量"));
}

void AnalysisPanel::SetupBitrateGopPage() {
    bitrate_gop_page_ = new BitrateGopPage(this);
    bitrate_gop_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    // 扫描请求统一由诊断页编排（页面才持有 facade 与进度条总控）；
    // 信号接线与其它共用页一样收进 RegisterScanPage。
    RegisterScanPage(bitrate_gop_page_);
    connect(bitrate_gop_page_, &BitrateGopPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    connect(bitrate_gop_page_, &BitrateGopPage::SceneLinkRequested,
            this, &AnalysisPanel::OnSceneLinkRequested);
    AddPageWithScroll(bitrate_gop_page_, tr("码率与 GOP"));
}

void AnalysisPanel::SetupAudioQcPage() {
    audio_qc_page_ = new AudioQcPage(this);
    audio_qc_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    RegisterScanPage(audio_qc_page_);
    connect(audio_qc_page_, &AudioQcPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    AddPageWithScroll(audio_qc_page_, tr("音频 QC"));
}

void AnalysisPanel::SetupColorHdrPage() {
    color_hdr_page_ = new ColorHdrPage(this);
    color_hdr_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    RegisterScanPage(color_hdr_page_);
    AddPageWithScroll(color_hdr_page_, tr("色彩与 HDR"));
}

void AnalysisPanel::SetupSubtitleAuxPage() {
    subtitle_aux_page_ = new SubtitleAuxPage(this);
    connect(subtitle_aux_page_, &SubtitleAuxPage::ScanRequested, this, [this]() {
        // 字幕页自己不发 demux：字幕包本来就在同一次扫描里过了一遍
        videoeye::AnalysisOptions options = diagnostics_page_->options();
        diagnostics_page_->StartScan(options);
    });
    connect(subtitle_aux_page_, &SubtitleAuxPage::StartTimecodeReady,
            this, &AnalysisPanel::StartTimecodeReady);
    connect(subtitle_aux_page_, &SubtitleAuxPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    AddPageWithScroll(subtitle_aux_page_, tr("字幕 / 辅助数据"));
}

// ---------------------------------------------------------------------------
// 播放器回调：只转发给页面，面板不再自己攒这些状态
// ---------------------------------------------------------------------------

void AnalysisPanel::OnContainerStructureReady(const model::ContainerStructureResult& result) {
    VE_PERF("OnContainerStructureReady 总计");
    if (!feature_enabled_.value(AnalysisFeature::Master, true)) return;
    if (container_page_) container_page_->SetResult(result);
    // 流媒体清单：顺手把「流媒体包」页也喂上（同一份结果，只是换了个视图）
    if (streaming_panel_) UpdateStreamingUi();
}

void AnalysisPanel::OnContainerStructureFailed(const QString& message) {
    // feature 关掉时页面本来就不画，这里也不该冒出错误提示
    if (!feature_enabled_.value(AnalysisFeature::Master, true)) return;
    if (container_page_) container_page_->SetError(message);
}

void AnalysisPanel::OnSceneChangeDetected(const model::SceneChangeResult& result) {
    if (scene_change_page_) scene_change_page_->AppendResult(result);
}

void AnalysisPanel::OnVisualDefectFrame(const model::FrameQualityMetric& metric) {
    if (visual_defect_page_) visual_defect_page_->AppendFrame(metric);
}

void AnalysisPanel::OnVisualDefectDetected(const model::VisualDefect& defect) {
    if (visual_defect_page_) visual_defect_page_->AppendDefect(defect);
}

void AnalysisPanel::OnVisualDefectReset() {
    if (visual_defect_page_) visual_defect_page_->ResetAll();
}

void AnalysisPanel::OnVisualDefectStats(int analyzed_frames, int dropped_frames,
                                        const model::ActivePictureArea& effective_area) {
    if (visual_defect_page_) {
        visual_defect_page_->ApplyStats(analyzed_frames, dropped_frames, effective_area);
    }
}

void AnalysisPanel::OnVisualDefectOptionChanged() {
    if (visual_defect_page_) emit VisualDefectOptionsChanged(visual_defect_page_->options());
}

// 「关联场景切换」：判定表归码率页，报告重算留在诊断页（那里才有 facade 与问题表），
// 面板这一层只做转发 + 让码率页弹出自己的汇总。
void AnalysisPanel::OnSceneLinkRequested(const std::vector<model::SceneChangeResult>& records,
                                         const videoeye::BitrateGopOptions& options) {
    if (diagnostics_page_) {
        diagnostics_page_->ApplySceneLink(records, options);
    }
    if (bitrate_gop_page_) bitrate_gop_page_->ShowSceneLinkSummary();
}

// ---------------------------------------------------------------------------
// 批量刷新定时器：拆出去的页面同样只置脏，重绘等这一拍
// ---------------------------------------------------------------------------

void AnalysisPanel::FlushPendingUiUpdates() {
    // 面板不可见时跳过UI刷新以节省CPU
    if (!isVisible()) return;

    // 批量刷新跑在主线程: 单次超过 50ms 就会被用户感知为卡顿, 打点记录便于定位
    const auto flush_begin = std::chrono::steady_clock::now();

    // 先刷帧/包明细，再刷流概览：前者可能在本拍产出新的 GOP 摘要，后者同一拍就能
    // 用上（「最大GOP大小」指标与 GOP 分布曲线都取自它）。反过来会慢一拍。
    if (frame_packet_view_ && frame_packet_view_->HasPending()) {
        frame_packet_view_->FlushPending();
    }
    if (stream_overview_view_ && stream_overview_view_->HasPending()) {
        stream_overview_view_->FlushPending();
    }
    if (macroblock_view_ && macroblock_view_->HasPending()) {
        macroblock_view_->FlushPending();
    }
    if (scene_change_page_ && scene_change_page_->HasPending()) {
        scene_change_page_->FlushPending();
    }
    if (visual_defect_page_ && visual_defect_page_->HasPending()) {
        visual_defect_page_->FlushPending();
    }
    if (event_timeline_view_) event_timeline_view_->FlushPendingUiUpdates();

    const double flush_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - flush_begin).count();
    if (flush_ms >= 50.0) {
        LOG_WARN("[perf] 分析面板批量 UI 刷新耗时 " +
                 std::to_string(static_cast<long long>(flush_ms)) + " ms");
    }
}

// ---------------------------------------------------------------------------
// 诊断与报告：一次 demux 供诊断页 / 码率 GOP / 音频 QC / 色彩 HDR / 字幕辅助
// 五页共用，所以扫描生命周期由诊断页自己编排，面板只做两件跨页的事 ——
// 把进度同步给共用扫描的其它页、把扫描结果分发给它们。
// ---------------------------------------------------------------------------

void AnalysisPanel::SetupDiagnosticsPage() {
    diagnostics_page_ = new DiagnosticsPage(this);
    diagnostics_page_->SetSourcePath(QString::fromStdString(current_video_path_));
    // 字幕的「过短 / 过长 / 阅读速度」阈值以「规则与阈值」那张可编辑的表为准。
    // 装成扫描前的钩子，任何入口发起的扫描（本页按钮 / 其它几页 / 流媒体包 /
    // 参数集）都会先同步一次，免得选项与规则两处阈值各说各话。
    diagnostics_page_->SetBeforeScanHook(
        [this](videoeye::AnalysisOptions& options) { SyncSubtitleThresholds(options); });

    // 扫描生命周期 -> 面板同步其它几页的按钮 / 进度条
    connect(diagnostics_page_, &DiagnosticsPage::ScanStarted,
            this, &AnalysisPanel::OnScanStarted);
    connect(diagnostics_page_, &DiagnosticsPage::ProgressChanged,
            this, &AnalysisPanel::OnDiagnosticsProgress);
    // 终态只有一个入口: 成功 / 取消 / 失败都从这里收尾共用同一次扫描的几页
    connect(diagnostics_page_, &DiagnosticsPage::ScanEnded,
            this, &AnalysisPanel::OnScanEnded);
    // 报告重算（改规则 / 关联场景切换）后，音频 QC 与色彩 HDR 的判定表要跟着换
    connect(diagnostics_page_, &DiagnosticsPage::QcReportChanged,
            this, [this](const model::QcReport& report) {
                if (audio_qc_page_) audio_qc_page_->SetQcReport(report);
                if (color_hdr_page_) color_hdr_page_->SetQcReport(report);
            });
    connect(diagnostics_page_, &DiagnosticsPage::SeekRequested, this, &AnalysisPanel::SeekRequested);

    AddPageWithScroll(diagnostics_page_, tr("诊断与报告"));
}

void AnalysisPanel::SyncSubtitleThresholds(videoeye::AnalysisOptions& options) {
    if (!subtitle_aux_page_ || !diagnostics_page_) return;
    subtitle_aux_page_->ApplyRuleThresholds(diagnostics_page_->rules(), options.subtitle_options);
}

void AnalysisPanel::SetupReportingPanelTab() {
    // 报告与批量 QC（功能 12）：模板选择 + 单文件报告 + 目录批量扫描 + 导出。
    // 该页自带后台线程，状态冒泡给主窗口状态栏。
    reporting_panel_ = new ui::ReportingPanel(this);
    connect(reporting_panel_, &ui::ReportingPanel::StatusMessage,
            this, &AnalysisPanel::StatusMessage);
    AddPageWithScroll(reporting_panel_, tr("报告与批量 QC"));
}

void AnalysisPanel::OnScanStarted() {
    // 与诊断页共用同一次扫描的页面：先把它们的开始/取消状态对齐
    for (ScanClient* page : scan_clients_) page->SetScanActive(true);
}

void AnalysisPanel::OnScanEnded(DiagnosticsPage::ScanEndReason reason) {
    const bool failed = (reason == DiagnosticsPage::ScanEndReason::Failed);
    const bool completed = (reason == DiagnosticsPage::ScanEndReason::Completed);

    // 共用同一次扫描的几页在这里**一起**回到 Idle。成功 / 取消 / 失败走同一条路径 ——
    // 以前失败时一个信号都不发，这几页就永远停在扫描态（取消按钮还亮着、"开始分析"
    // 永久禁用），与诊断页显示的"失败"互相矛盾，用户只能重开文件才恢复。
    const QString final_format = failed ? tr("扫描失败")
                                        : (completed ? tr("分析完成") : tr("已取消（结果不完整）"));
    const int final_progress = failed ? 0 : 100;
    for (ScanClient* page : scan_clients_) {
        page->SetScanActive(false);
        page->SetProgress(final_progress);
        page->SetProgressFormat(final_format);
    }

    // 失败没有可用结果: 状态恢复完就结束, 不要用空结果盖掉页面上的旧内容。
    if (failed) return;

    // ---- 以下把同一次扫描的结果分发给各页（失败的路径已在上面返回）----
    if (!diagnostics_page_) return;
    VE_PERF("AnalysisPanel 分发扫描结果");

    // 「关联场景切换」要用场景切换页的记录，先喂给码率页
    if (bitrate_gop_page_) {
        bitrate_gop_page_->SetSceneChanges(scene_change_page_ ? scene_change_page_->records()
                                                              : std::vector<model::SceneChangeResult>());
        bitrate_gop_page_->SetResult(diagnostics_page_->result());
    }
    // 音频 QC / 色彩 HDR 的异常表来自 QC 规则引擎，所以连报告一起给
    if (audio_qc_page_) {
        audio_qc_page_->SetResult(diagnostics_page_->result(), diagnostics_page_->qcReport());
    }
    if (color_hdr_page_) {
        color_hdr_page_->SetResult(diagnostics_page_->result(), diagnostics_page_->qcReport());
    }

    {
        VE_PERF("UpdateBitstreamUi");
        UpdateBitstreamUi();  // 参数集页（SPS/PPS/Sequence Header）同样共用同一次扫描结果
    }
    {
        VE_PERF("UpdateStreamingUi");
        UpdateStreamingUi();  // 流媒体包页：清单文件的 QC 结果来自同一次扫描
    }
    if (subtitle_aux_page_) {
        VE_PERF("SubtitleAuxPage::SetResult");
        subtitle_aux_page_->SetResult(diagnostics_page_->result());
    }

    // MP4 样本表：扫描跑过就顺带刷新容器页（与打开文件时那次解析结果一致）
    const model::AnalysisResult& result = diagnostics_page_->result();
    if (result.mp4_samples_analyzed && result.mp4_samples.valid) {
        VE_PERF("诊断后刷新 MP4 样本表");
        if (container_page_) container_page_->ApplySampleTable(result.mp4_samples);
    }
}

// 扫描进行中的进度同步（只在扫描中才有意义；终态收口统一在 OnScanEnded）
void AnalysisPanel::OnDiagnosticsProgress(double percent, const QString& stage) {
    for (ScanClient* page : scan_clients_) {
        page->SetProgress(static_cast<int>(percent));
        page->SetProgressFormat(stage + " %p%");
    }
}

} // namespace ui
} // namespace videoeye
