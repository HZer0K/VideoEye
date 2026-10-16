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
void AnalysisPanel::AddScanClient(ScanClient* page) {
    scan_clients_.push_back(page);
}

template <typename PageT>
void AnalysisPanel::RegisterScanPage(PageT* page) {
    AddScanClient(page);
    connect(page, &PageT::ScanRequested, this, [this, page]() {
        if (!pages_.diagnostics) return;
        videoeye::AnalysisOptions options = pages_.diagnostics->options();
        page->FillScanOptions(options);
        pages_.diagnostics->StartScan(options);
    });
    connect(page, &PageT::CancelRequested, this, [this]() {
        if (pages_.diagnostics) pages_.diagnostics->CancelScan();
    });
}

AnalysisPanel::AnalysisPanel(QWidget* parent)
    : QWidget(parent) {
    
    // 播放期/打开期开关的默认值统一按「附加开销」分层（与界面文案、tooltip 一致）：
    //   默认开 = 复用播放/打开本来就有的数据，几乎无额外开销（流统计、视频帧、容器结构）；
    //   默认关 = 需要额外的解码、降采样或数据留存（音频帧、数据包、事件、同步、时间轴、
    //            宏块、场景切换、画面质量），界面统一标注「按需开启」。
    // 全文件扫描页（码率 GOP / 音频 QC / 色彩 HDR / 参数集 / 流媒体包 / 字幕 / 质量诊断）
    // 不用常驻开关，统一由「开始分析」按钮触发共享扫描。
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

void AnalysisPanel::SetMacroblockAnalysisEnabled(bool enabled) {
    feature_enabled_[AnalysisFeature::Macroblock] = enabled;
    // 同步宏块页勾选框（钩子读的是 feature_enabled_）。SyncToggleFromHooks 内部屏蔽
    // 信号，不会回调 set_enabled_，因此不会反过来又发一次 AnalysisFeatureToggled。
    if (pages_.macroblock) pages_.macroblock->SyncToggleFromHooks();
}

void AnalysisPanel::EmitInitialFeatureStates() {
    // 对每个启用状态的 feature 重新发射信号 (除了 Master 和 Mp4Box，以及 Macroblock)。
    // Macroblock 不在这里重发：它的实际采集状态由协调层把"用户启用"与"叠加需求"取或
    // 后通过 SetMacroblockAnalysisEnabled + SetAnalysisFeature 统一下发（见 MainWindow::
    // ApplyMacroblockAnalysisState）。若在换文件时按 feature_enabled_ 重发，会在
    // "仅因叠加而开启"的情况下把用户意图误标成开启，导致关闭叠加后回不到原状态。
    static const AnalysisFeature kFeatures[] = {
        AnalysisFeature::StreamStats,
        AnalysisFeature::VideoFrame,
        AnalysisFeature::AudioFrame,
        AnalysisFeature::Packet,
        AnalysisFeature::Event,
        AnalysisFeature::SyncSample,
        AnalysisFeature::Timeline,
        AnalysisFeature::VisualDefect,
    };
    for (auto feat : kFeatures) {
        bool enabled = feature_enabled_.value(feat, true);
        emit AnalysisFeatureToggled(static_cast<int>(feat), enabled);
    }
}


void AnalysisPanel::AddPageWithScroll(QWidget* tab_widget, const QString& title,
                                      const QString& page_id, const QString& group_id, int order,
                                      const QString& scope) {
    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(tab_widget);
    // 导航元数据（MainWindow 用它生成侧栏条目与分组，见 navigation_model.h）
    scroll->setProperty("pageTitle", title);
    scroll->setProperty("pageId", page_id);
    scroll->setProperty("pageGroup", group_id);
    scroll->setProperty("pageOrder", order);
    scroll->setProperty("pageScope", scope);
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
// 播放期数据回吐：面板只做「开关过滤 + 转发」，数据本身归各页面组件
// ===========================================================================

void AnalysisPanel::UpdateStreamStats(const model::StreamStats& stats) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::StreamStats, true)) return;
    if (pages_.stream_overview) pages_.stream_overview->SetStreamStats(stats);
}

void AnalysisPanel::ResetVideoFrameList() {
    // 手动清空不受 feature 开关约束：以前被 Master/VideoFrame 挡着，关掉开关后
    // 用户点「清空」没反应（数据还在，看起来像坏了）。开关只管"要不要收新数据"，
    // 不兼管"用户想不想清"。
    if (pages_.frame_packet) pages_.frame_packet->ResetVideoFrames();
    // 换文件时曲线也要清零：原先由面板自己 reset，现在曲线归流概览区。
    // （GOP 相关的那一份数据由 FramePacketView::ResetVideoFrames 清空后
    //   经 GopSummariesChanged 通知，两边不会各清一半。）
    if (pages_.stream_overview) pages_.stream_overview->ResetCharts();
}

void AnalysisPanel::AppendVideoFrameInfo(int index, int frame_type, bool is_key_frame,
                                         qint64 pts, double timestamp_seconds) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::VideoFrame, true)) return;
    if (pages_.frame_packet) {
        pages_.frame_packet->AppendVideoFrame(index, frame_type, is_key_frame, pts, timestamp_seconds);
    }
}

void AnalysisPanel::ResetAudioFrameList() {
    // 同 ResetVideoFrameList：清空不收 feature 开关的约束
    if (pages_.frame_packet) pages_.frame_packet->ResetAudioFrames();
}

void AnalysisPanel::AppendAudioFrameInfo(int index, qint64 pts, double timestamp_seconds,
                                         int sample_count, int sample_rate, int channels, int byte_count) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::AudioFrame, true)) return;
    if (pages_.frame_packet) {
        pages_.frame_packet->AppendAudioFrame(index, pts, timestamp_seconds,
                                             sample_count, sample_rate, channels, byte_count);
    }
}

void AnalysisPanel::ResetPacketList() {
    // 同 ResetVideoFrameList：清空不收 feature 开关的约束
    if (pages_.frame_packet) pages_.frame_packet->ResetPackets();
}

void AnalysisPanel::AppendPacketInfo(const model::PacketInfo& packet_info) {
    if (!feature_enabled_.value(AnalysisFeature::Master, true) ||
        !feature_enabled_.value(AnalysisFeature::Packet, true)) return;
    if (pages_.frame_packet) pages_.frame_packet->AppendPacket(packet_info);
}

void AnalysisPanel::UpdateMacroblockInfo(const model::MacroblockFrameAnalysis& analysis) {
    if (pages_.macroblock) pages_.macroblock->SetAnalysis(analysis);
}

void AnalysisPanel::ResetAnalysisEventList() {
    if (pages_.event_timeline) pages_.event_timeline->ResetAnalysisEventList();
}


void AnalysisPanel::ResetSyncSampleList() {
    if (pages_.event_timeline) pages_.event_timeline->ResetSyncSampleList();
}


void AnalysisPanel::ResetTimelineEventList() {
    if (pages_.event_timeline) pages_.event_timeline->ResetTimelineEventList();
}


void AnalysisPanel::AppendAnalysisEvent(const model::AnalysisEvent& event_info) {
    if (pages_.event_timeline) pages_.event_timeline->AppendAnalysisEvent(event_info);
}


void AnalysisPanel::AppendSyncSample(const model::SyncSample& sample) {
    // 同步样本只喂给时间轴视图（同步表 / 曲线）。以前还直发一份给诊断页，让它维护
    // 第二套音视频偏移曲线 —— 同一份样本绕两页、两处展示，已收敛到本视图一处。
    if (pages_.event_timeline) pages_.event_timeline->AppendSyncSample(sample);
}


void AnalysisPanel::AppendTimelineEvent(const model::TimelineEvent& event) {
    if (pages_.event_timeline) pages_.event_timeline->AppendTimelineEvent(event);
}


void AnalysisPanel::OnStreamingRefreshRequested() {
    // 清单解析是纯本地文件读取（第一阶段不联网），跟着全文件扫描一起跑。
    // 扫描总控在诊断页，这里只改一个开关再交给它跑
    pages_.diagnostics->options().analyze_streaming_package = true;
    pages_.diagnostics->StartScan(pages_.diagnostics->options());
}

void AnalysisPanel::UpdateStreamingUi() {
    if (!pages_.streaming) return;
    // 优先用容器结构分析的结果（打开清单文件时由 ContainerStructureAnalyzer 直接产出）；
    // 没有的话再退到全文件扫描的 streaming_package。
    if (pages_.container && pages_.container->result().streaming_package.valid) {
        pages_.streaming->SetResult(pages_.container->result().streaming_package);
        return;
    }
    if (pages_.diagnostics && pages_.diagnostics->hasResult() &&
        pages_.diagnostics->result().streaming_analyzed) {
        pages_.streaming->SetResult(pages_.diagnostics->result().streaming_package);
        return;
    }
    pages_.streaming->Clear();
}


void AnalysisPanel::OnBitstreamRefreshRequested() {
    // 码流解析只读 extradata（KB 级），成本可忽略，跟着全文件扫描一起跑。
    pages_.diagnostics->options().analyze_bitstream = true;
    pages_.diagnostics->StartScan(pages_.diagnostics->options());
}

void AnalysisPanel::UpdateBitstreamUi() {
    if (!pages_.bitstream_params) return;
    if (!pages_.diagnostics || !pages_.diagnostics->hasResult() ||
        !pages_.diagnostics->result().bitstream_analyzed) {
        pages_.bitstream_params->Clear();
        return;
    }
    pages_.bitstream_params->SetResult(pages_.diagnostics->result().bitstream_analysis);
}

// 播放器打开失败进入「分析模式」时的自动扫描入口：用诊断页的默认选项跑一次。
// 静默失败路径 —— 不弹"请先打开文件"提示框，原因写进诊断页自己的汇总标签。
void AnalysisPanel::StartDiagnosticsScanForCurrentFile() {
    if (!pages_.diagnostics || current_video_path_.empty()) return;
    // 静默路径：打开失败后自动补扫一次，不能弹窗打断"打开即分析"的流程
    pages_.diagnostics->StartScan(pages_.diagnostics->options(), /*silent=*/true);
}

// ===========================================================================
// 建页 / 接线
//
// 建页（new 组件 + 导航元数据 + 注入 feature 钩子）在 AnalysisPageFactory 里，
// 按 AnalysisPageCatalog 的顺序循环 —— 顺序即外部 stack 的页序号，是硬契约。
// 本类只留接线：把页面发出的意图接到全局、把扫描生命周期同步给共用扫描的几页。
// 页面之间不互相持有指针，跨页数据一律经本类转发。
// ===========================================================================

void AnalysisPanel::SetupUI() {
    // 不再创建内部 QTabWidget，页面由 AddPageWithScroll 收集
    // PopulateStackedWidget 时添加到外部 QStackedWidget
    pages_ = BuildAnalysisPages(MakePageBuildDeps());
    WireAnalysisPages();

    qRegisterMetaType<model::SceneChangeResult>();
    qRegisterMetaType<model::AnalysisResult>();
    qRegisterMetaType<model::FrameQualityMetric>();
    qRegisterMetaType<model::VisualDefect>();
    qRegisterMetaType<model::VisualDefectOptions>();
}

PageBuildDeps AnalysisPanel::MakePageBuildDeps() {
    PageBuildDeps deps;
    deps.parent = this;
    deps.signal_context = this;
    // 以回调注入 tr()，保留 AnalysisPanel 这个翻译上下文（工厂是自由函数，没有 tr 可用）
    deps.translate = [](const char* source) { return AnalysisPanel::tr(source); };
    deps.is_feature_on = [this](model::AnalysisFeature feature) {
        // feature_enabled_ 在构造函数里给每个开关都显式置了初值，后面的兜底值
        // 实际永远用不上 —— 留 true 只是为了不在这里凭空造第二套默认值。
        return feature_enabled_.value(feature, true);
    };
    deps.set_feature = [this](model::AnalysisFeature feature, bool enabled) {
        feature_enabled_[feature] = enabled;
        emit AnalysisFeatureToggled(static_cast<int>(feature), enabled);
    };
    deps.add_page = [this](QWidget* widget, const QString& title, const QString& page_id,
                           const QString& group_id, int order, const QString& scope) {
        AddPageWithScroll(widget, title, page_id, group_id, order, scope);
    };
    deps.current_path = [this]() { return QString::fromStdString(current_video_path_); };
    deps.visual_defect_options = [this]() { return visual_defect_options_; };
    return deps;
}

void AnalysisPanel::WireAnalysisPages() {
    // ---- 文件结构 ----
    connect(pages_.container, &ContainerStructurePage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::ContainerStructure] = enabled;
                emit AnalysisFeatureToggled(
                    static_cast<int>(AnalysisFeature::ContainerStructure), enabled);
            });

    // ---- 场景切换 ----
    connect(pages_.scene_change, &SceneChangePage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::SceneChange] = enabled;
                emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::SceneChange), enabled);
            });

    // ---- 画面质量 / 视觉缺陷 ----
    connect(pages_.visual_defect, &VisualDefectPage::FeatureToggled, this,
            [this](bool enabled) {
                feature_enabled_[AnalysisFeature::VisualDefect] = enabled;
                emit AnalysisFeatureToggled(static_cast<int>(AnalysisFeature::VisualDefect), enabled);
            });
    connect(pages_.visual_defect, &VisualDefectPage::OptionsChanged,
            this, &AnalysisPanel::VisualDefectOptionsChanged);
    connect(pages_.visual_defect, &VisualDefectPage::SeekRequested,
            this, &AnalysisPanel::SeekRequested);

    // ---- 码率与 GOP ----
    // 扫描请求统一由诊断页编排（页面才持有 facade 与进度条总控）；
    // 信号接线与其它共用页一样收进 RegisterScanPage。
    RegisterScanPage(pages_.bitrate_gop);
    connect(pages_.bitrate_gop, &BitrateGopPage::SeekRequested, this, &AnalysisPanel::SeekRequested);
    connect(pages_.bitrate_gop, &BitrateGopPage::SceneLinkRequested,
            this, &AnalysisPanel::OnSceneLinkRequested);

    // ---- 音频 QC ----
    RegisterScanPage(pages_.audio_qc);
    connect(pages_.audio_qc, &AudioQcPage::SeekRequested, this, &AnalysisPanel::SeekRequested);

    // ---- 色彩与 HDR ----
    RegisterScanPage(pages_.color_hdr);

    // ---- 编码参数集 ----
    connect(pages_.bitstream_params, &BitstreamPanel::RefreshRequested,
            this, &AnalysisPanel::OnBitstreamRefreshRequested);
    // 与流媒体包页一致：登记进共享扫描注册表，扫描中按钮一起禁用。
    AddScanClient(pages_.bitstream_params);

    // ---- 流媒体包 ----
    connect(pages_.streaming, &StreamingPanel::RefreshRequested,
            this, &AnalysisPanel::OnStreamingRefreshRequested);
    // 「开始分析」由本页自己发出、面板接线；这里只把它登记进共享扫描注册表，
    // 让扫描中按钮跟着一起禁用、换文件同步，和码率/音频/HDR 表现一致。
    AddScanClient(pages_.streaming);

    // ---- 字幕与辅助数据 ----
    connect(pages_.subtitle_aux, &SubtitleAuxPage::ScanRequested, this, [this]() {
        // 字幕页自己不发 demux：字幕包本来就在同一次扫描里过了一遍
        pages_.diagnostics->StartScan(pages_.diagnostics->options());
    });
    connect(pages_.subtitle_aux, &SubtitleAuxPage::StartTimecodeReady,
            this, &AnalysisPanel::StartTimecodeReady);
    connect(pages_.subtitle_aux, &SubtitleAuxPage::SeekRequested,
            this, &AnalysisPanel::SeekRequested);
    // 字幕页同样共用同一次扫描：登记进注册表，扫描中「开始分析」跟着禁用、换文件同步。
    AddScanClient(pages_.subtitle_aux);

    // ---- 质量诊断（扫描总控）----
    // 字幕的「过短 / 过长 / 阅读速度」阈值以「规则与阈值」那张可编辑的表为准。
    // 装成扫描前的钩子，任何入口发起的扫描（本页按钮 / 其它几页 / 流媒体包 /
    // 参数集）都会先同步一次，免得选项与规则两处阈值各说各话。
    pages_.diagnostics->SetBeforeScanHook(
        [this](videoeye::AnalysisOptions& options) { SyncSubtitleThresholds(options); });

    // 扫描生命周期 -> 面板同步其它几页的按钮 / 进度条
    connect(pages_.diagnostics, &DiagnosticsPage::ScanStarted, this, &AnalysisPanel::OnScanStarted);
    connect(pages_.diagnostics, &DiagnosticsPage::ProgressChanged,
            this, &AnalysisPanel::OnDiagnosticsProgress);
    // 终态只有一个入口: 成功 / 取消 / 失败都从这里收尾共用同一次扫描的几页
    connect(pages_.diagnostics, &DiagnosticsPage::ScanEnded, this, &AnalysisPanel::OnScanEnded);
    // 报告重算（改规则 / 关联场景切换）后，音频 QC 与色彩 HDR 的判定表要跟着换
    connect(pages_.diagnostics, &DiagnosticsPage::QcReportChanged,
            this, [this](const model::QcReport& report) {
                if (pages_.audio_qc) pages_.audio_qc->SetQcReport(report);
                if (pages_.color_hdr) pages_.color_hdr->SetQcReport(report);
                // 首页的问题计数 / 最重要的异常同样跟着重算
                emit DiagnosticsStateChanged();
            });
    connect(pages_.diagnostics, &DiagnosticsPage::SeekRequested,
            this, &AnalysisPanel::SeekRequested);

    // ---- 报告与批量 QC ----
    // 该页自带后台线程，状态冒泡给主窗口状态栏。
    connect(pages_.reporting, &ui::ReportingPanel::StatusMessage,
            this, &AnalysisPanel::StatusMessage);
}


// ---------------------------------------------------------------------------
// 播放器回调：只转发给页面，面板不再自己攒这些状态
// ---------------------------------------------------------------------------

void AnalysisPanel::OnContainerStructureReady(const model::ContainerStructureResult& result) {
    VE_PERF("OnContainerStructureReady 总计");
    if (!feature_enabled_.value(AnalysisFeature::Master, true)) return;
    if (pages_.container) pages_.container->SetResult(result);
    // 流媒体清单：顺手把「流媒体包」页也喂上（同一份结果，只是换了个视图）
    if (pages_.streaming) UpdateStreamingUi();
}

void AnalysisPanel::OnContainerStructureFailed(const QString& message) {
    // feature 关掉时页面本来就不画，这里也不该冒出错误提示
    if (!feature_enabled_.value(AnalysisFeature::Master, true)) return;
    if (pages_.container) pages_.container->SetError(message);
}

void AnalysisPanel::OnSceneChangeDetected(const model::SceneChangeResult& result) {
    if (pages_.scene_change) pages_.scene_change->AppendResult(result);
}

void AnalysisPanel::OnVisualDefectFrame(const model::FrameQualityMetric& metric) {
    if (pages_.visual_defect) pages_.visual_defect->AppendFrame(metric);
}

void AnalysisPanel::OnVisualDefectDetected(const model::VisualDefect& defect) {
    if (pages_.visual_defect) pages_.visual_defect->AppendDefect(defect);
}

void AnalysisPanel::OnVisualDefectReset() {
    if (pages_.visual_defect) pages_.visual_defect->ResetAll();
}

void AnalysisPanel::OnVisualDefectStats(int analyzed_frames, int dropped_frames,
                                        const model::ActivePictureArea& effective_area) {
    if (pages_.visual_defect) {
        pages_.visual_defect->ApplyStats(analyzed_frames, dropped_frames, effective_area);
    }
}

void AnalysisPanel::OnVisualDefectOptionChanged() {
    if (pages_.visual_defect) emit VisualDefectOptionsChanged(pages_.visual_defect->options());
}

// 「关联场景切换」：判定表归码率页，报告重算留在诊断页（那里才有 facade 与问题表），
// 面板这一层只做转发 + 让码率页弹出自己的汇总。
void AnalysisPanel::OnSceneLinkRequested(const std::vector<model::SceneChangeResult>& records,
                                         const videoeye::BitrateGopOptions& options) {
    if (pages_.diagnostics) {
        pages_.diagnostics->ApplySceneLink(records, options);
    }
    if (pages_.bitrate_gop) pages_.bitrate_gop->ShowSceneLinkSummary();
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
    if (pages_.frame_packet && pages_.frame_packet->HasPending()) {
        pages_.frame_packet->FlushPending();
    }
    if (pages_.stream_overview && pages_.stream_overview->HasPending()) {
        pages_.stream_overview->FlushPending();
    }
    if (pages_.macroblock && pages_.macroblock->HasPending()) {
        pages_.macroblock->FlushPending();
    }
    if (pages_.scene_change && pages_.scene_change->HasPending()) {
        pages_.scene_change->FlushPending();
    }
    if (pages_.visual_defect && pages_.visual_defect->HasPending()) {
        pages_.visual_defect->FlushPending();
    }
    if (pages_.event_timeline) pages_.event_timeline->FlushPendingUiUpdates();

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


void AnalysisPanel::SyncSubtitleThresholds(videoeye::AnalysisOptions& options) {
    if (!pages_.subtitle_aux || !pages_.diagnostics) return;
    pages_.subtitle_aux->ApplyRuleThresholds(pages_.diagnostics->rules(), options.subtitle_options);
}


void AnalysisPanel::OnScanStarted() {
    // 与诊断页共用同一次扫描的页面：先把它们的开始/取消状态对齐
    scan_running_ = true;
    for (ScanClient* page : scan_clients_) page->SetScanActive(true);
    // 首页的「分析状态」区要立刻切到"分析中"
    emit DiagnosticsStateChanged();
}

void AnalysisPanel::OnScanEnded(DiagnosticsPage::ScanEndReason reason) {
    const bool failed = (reason == DiagnosticsPage::ScanEndReason::Failed);
    const bool completed = (reason == DiagnosticsPage::ScanEndReason::Completed);

    scan_running_ = false;

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
    if (!pages_.diagnostics) return;
    VE_PERF("AnalysisPanel 分发扫描结果");

    // 「关联场景切换」要用场景切换页的记录，先喂给码率页
    if (pages_.bitrate_gop) {
        pages_.bitrate_gop->SetSceneChanges(pages_.scene_change ? pages_.scene_change->records()
                                                              : std::vector<model::SceneChangeResult>());
        pages_.bitrate_gop->SetResult(pages_.diagnostics->result());
    }
    // 音频 QC / 色彩 HDR 的异常表来自 QC 规则引擎，所以连报告一起给
    if (pages_.audio_qc) {
        pages_.audio_qc->SetResult(pages_.diagnostics->result(), pages_.diagnostics->qcReport());
    }
    if (pages_.color_hdr) {
        pages_.color_hdr->SetResult(pages_.diagnostics->result(), pages_.diagnostics->qcReport());
    }

    {
        VE_PERF("UpdateBitstreamUi");
        UpdateBitstreamUi();  // 参数集页（SPS/PPS/Sequence Header）同样共用同一次扫描结果
    }
    {
        VE_PERF("UpdateStreamingUi");
        UpdateStreamingUi();  // 流媒体包页：清单文件的 QC 结果来自同一次扫描
    }
    if (pages_.subtitle_aux) {
        VE_PERF("SubtitleAuxPage::SetResult");
        pages_.subtitle_aux->SetResult(pages_.diagnostics->result());
    }

    // MP4 样本表：扫描跑过就顺带刷新容器页（与打开文件时那次解析结果一致）
    const model::AnalysisResult& result = pages_.diagnostics->result();
    if (result.mp4_samples_analyzed && result.mp4_samples.valid) {
        VE_PERF("诊断后刷新 MP4 样本表");
        if (pages_.container) pages_.container->ApplySampleTable(result.mp4_samples);
    }

    // 终态回到首页：评分/结论/异常清单都该刷新（失败的路径上面已 return，没有新结果）
    emit DiagnosticsStateChanged();
}

// ---------------------------------------------------------------------------
// 诊断状态只读快照（首页「分析状态」区经 DiagnosticsStateChanged 通知后拉取）
// ---------------------------------------------------------------------------

bool AnalysisPanel::HasDiagnosticsResult() const {
    return pages_.diagnostics && pages_.diagnostics->hasResult();
}

const model::QcReport& AnalysisPanel::DiagnosticsReport() const {
    // 诊断页的 qcReport() 返回成员引用，没有结果时是一份默认构造的空报告，
    // 调用方先问 HasDiagnosticsResult() 再取数。
    static const model::QcReport kEmpty;
    return pages_.diagnostics ? pages_.diagnostics->qcReport() : kEmpty;
}

QString AnalysisPanel::DiagnosticsError() const {
    return pages_.diagnostics ? pages_.diagnostics->lastError() : QString();
}

// ---------------------------------------------------------------------------
// 侧栏导航标记
// ---------------------------------------------------------------------------

// 返回类型写成完整形式（而不是 AnalysisPanel::PageBadges）：MSVC 会把
// "类名::嵌套类型 类名::同名函数" 误判成构造函数声明，报 C2143。
QMap<QString, AnalysisPanel::PageBadgeInfo> AnalysisPanel::PageBadges() const {
    QMap<QString, PageBadgeInfo> badges;
    const bool scanning = scan_running_;

    // 有结果或扫描中的页面才进表
    auto add = [&badges, scanning](const QString& page_id, int count, int level) {
        if (count <= 0 && !scanning) return;
        PageBadgeInfo info;
        info.issue_count = count;
        info.level = count > 0 ? level : 0;
        info.scanning = scanning;
        badges.insert(page_id, info);
    };

    // 质量诊断: QC 报告问题总数 + 最高严重度（致命>错误>警告）
    if (pages_.diagnostics && pages_.diagnostics->hasResult()) {
        const model::QcReport& report = pages_.diagnostics->qcReport();
        const int count = static_cast<int>(report.issues.size());
        int level = 0;
        if (report.CountBySeverity(model::IssueSeverity::Critical) > 0) {
            level = 3;
        } else if (report.CountBySeverity(model::IssueSeverity::Error) > 0) {
            level = 2;
        } else if (count > 0) {
            level = 1;
        }
        add(QStringLiteral("quality_diagnostics"), count, level);
    } else {
        add(QStringLiteral("quality_diagnostics"), 0, 0);
    }

    // 共用同一次扫描的页面: 各自的异常表行数（没有异常表的页面只标扫描态）
    if (pages_.bitrate_gop) add(QStringLiteral("bitrate_gop"), pages_.bitrate_gop->IssueCount(), 1);
    if (pages_.audio_qc) add(QStringLiteral("audio_qc"), pages_.audio_qc->IssueCount(), 1);
    if (pages_.color_hdr) add(QStringLiteral("color_hdr"), pages_.color_hdr->IssueCount(), 1);
    if (pages_.streaming) add(QStringLiteral("streaming_packets"), pages_.streaming->IssueCount(), 1);
    if (pages_.bitstream_params) {
        add(QStringLiteral("codec_parameters"), pages_.bitstream_params->IssueCount(), 1);
    }
    if (pages_.subtitle_aux) add(QStringLiteral("subtitle_aux"), 0, 0);

    // 画面质量是播放期累积的缺陷记录，不随全文件扫描走，但同样值得一个计数徽标
    if (pages_.visual_defect) {
        const int count = static_cast<int>(pages_.visual_defect->DefectRecords().size());
        if (count > 0) {
            add(QStringLiteral("visual_quality"), count, 1);
        }
    }
    return badges;
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
