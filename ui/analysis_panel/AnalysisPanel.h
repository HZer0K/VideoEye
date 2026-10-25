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
#include "core/domain/model/TimelineEvent.h"
#include "core/domain/model/VisualDefect.h"
#include "core/domain/model/VisualDefectOptions.h"

#include "ui/AnalysisFacade.h"
#include "ui/bitstream_panel/BitstreamPanel.h"
#include "ui/reporting_panel/ReportingPanel.h"
#include "ui/streaming_panel/StreamingPanel.h"

// 建页工厂: 页面目录（顺序 = stack 页序号）、建页、侧栏徽标口径都在这里。
// 面板自己只留"页面之间怎么接线"。
#include "ui/analysis_panel/AnalysisPageFactory.h"
#include "ui/analysis_panel/ScanClient.h"

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

    // 由协调层（MainWindow）下发"宏块分析的实际采集状态"：当 MV 叠加打开时会强制
    // 开启，关闭叠加后恢复成用户原先主动开启的状态。这里只更新面板内部 feature 表
    // 与宏块页勾选框，**不**发 AnalysisFeatureToggled —— 避免与协调层形成回环。
    void SetMacroblockAnalysisEnabled(bool enabled);

    // 设置当前视频文件路径 (供导出报告)
    void SetCurrentVideoPath(const QString& path) {
        current_video_path_ = path.toStdString();
        // 切换文件后重置时间轴累计状态 (避免上一文件的数据混入)；
        // 时间轴与 facade 都在诊断页里，由它自己清空
        if (pages_.diagnostics)
            pages_.diagnostics->ResetForNewFile();
        // 报告与批量 QC 页需要当前文件来"按模板重新分析"
        if (pages_.reporting)
            pages_.reporting->SetCurrentFile(path);
        // 拆出去的页面用当前路径拼导出 CSV 的默认文件名
        if (pages_.scene_change)
            pages_.scene_change->SetSourcePath(path);
        // 共用同一次扫描的页面统一走注册表（码率 GOP / 音频 QC / 色彩 HDR）
        for (ScanClient* page : scan_clients_)
            page->SetSourcePath(path);
        // 扫描总控在诊断页，换文件要同步它手里的当前路径
        if (pages_.diagnostics)
            pages_.diagnostics->SetSourcePath(path);
        // 场景切换记录是播放期的累计结果，换文件必须清零（与帧/包那几页一致）
        if (pages_.scene_change)
            pages_.scene_change->Reset();
        // 换文件 = 上一文件的分析结论全部作废：通知首页重置「分析状态」区
        emit DiagnosticsStateChanged();
    }

    // 重新发射所有启用状态的开关信号 (用于文件打开后同步播放器状态)
    void EmitInitialFeatureStates();

    // 用面板当前的诊断选项对 current_video_path_ 发起全文件诊断扫描。
    // 播放器打开失败进入"分析模式"时由 MainWindow 调用, 让诊断页展示具体错误原因。
    void StartDiagnosticsScanForCurrentFile();

    // ---- 诊断状态只读快照（供首页「分析状态」区拉取）----
    // 扫描起止 / 报告重算 / 换文件都会发 DiagnosticsStateChanged，MainWindow 收到后
    // 再调这几个取数刷新首页 —— 页面不互相持有指针，快照式读取与容器结构页等一致。
    bool IsScanRunning() const {
        return scan_running_;
    }
    bool HasDiagnosticsResult() const;
    const model::QcReport& DiagnosticsReport() const;
    // 最近一次全文件扫描的失败原因（无失败返回空串）
    QString DiagnosticsError() const;

    // ---- 侧栏导航标记 ----
    // 每个分析页的"异常数 + 级别 + 是否共用扫描中"。MainWindow 在
    // DiagnosticsStateChanged 后调用，把结果写进侧栏 item 的数据角色，
    // 由 SidebarItemDelegate 画成右侧徽标 / 扫描圆点。
    // 只有真有异常口径的页面才进表（issue_count>0 或 scanning），
    // 帧表/包表这类明细表故意不进 —— 几千行的徽标只是噪音。
    // 徽标的结构与口径都收在建页工厂（ui/analysis_panel/AnalysisPageFactory.h 的
    // PageBadge / CollectPageBadges）里，这里只留一个别名 —— 既有的
    // AnalysisPanel::PageBadgeInfo 写法（MainWindow 取标记用）一个字都不用改。
    using PageBadgeInfo = PageBadge;
    QMap<QString, PageBadgeInfo> PageBadges() const;

signals:
    // 分析功能开关变化信号 (供 MainWindow 连接 MediaPlayer)
    void AnalysisFeatureToggled(int feature, bool enabled);

    // 诊断扫描状态变化（开始 / 终态 / 报告重算 / 换文件）: MainWindow 转给首页刷新
    void DiagnosticsStateChanged();

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
    void AppendAudioFrameInfo(int index, qint64 pts, double timestamp_seconds, int sample_count, int sample_rate,
                              int channels, int byte_count);
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
    void OnVisualDefectStats(int analyzed_frames, int dropped_frames, const model::ActivePictureArea& effective_area);
    void OnVisualDefectOptionChanged();

    // 参数集页「开始分析」：复用同一次全文件扫描
    void OnBitstreamRefreshRequested();

    // 扫描生命周期由 DiagnosticsPage 编排，面板只把进度同步给其它几页
    void OnScanStarted();
    void OnDiagnosticsProgress(double percent, const QString& stage);
    // 唯一的扫描终态收尾: 恢复共用页的按钮/进度（三种终态一视同仁），
    // 非失败时再把同一份结果分发下去。不再有"成功/取消各走一半"的分裂路径。
    void OnScanEnded(DiagnosticsPage::ScanEndReason reason);

private:
    // 初始化UI: 建页交给工厂（AnalysisPageFactory，顺序见 AnalysisPageCatalog），
    // 本类只负责把页面之间的意图接起来。
    void SetupUI();
    // 建页所需的外部能力（feature 表 / 当前路径 / 登记页面的出口），喂给工厂。
    // 工厂不认识面板，本类也不认识工厂内部的摆放细节 —— 两边只通过这张依赖表说话。
    PageBuildDeps MakePageBuildDeps();
    // 页面之间的接线：页面发出的意图（跳转 / 扫描 / 状态冒泡）与扫描生命周期。
    // 这是协调层的本职，不随建页搬去工厂 —— 页面之间不互相持有指针这条规矩不能破。
    void WireAnalysisPages();
    // 将页面包裹 QScrollArea 并添加到页面列表。同时把导航元数据（稳定 pageId、
    // 分组 id、组内排序键、口径）写成控件属性，供 MainWindow 生成侧栏；见
    // ui/main_window/navigation_model.h。由 MakePageBuildDeps 作为 add_page 交给工厂。
    void AddPageWithScroll(QWidget* tab_widget, const QString& title, const QString& page_id, const QString& group_id,
                           int order, const QString& scope = QString());
    void FlushPendingUiUpdates();

    // 「关联场景切换」：页面不持有 facade，这一步由面板编排（重算 QC + 刷新问题表）
    void OnSceneLinkRequested(const std::vector<model::SceneChangeResult>& records,
                              const videoeye::BitrateGopOptions& options);

    void UpdateBitstreamUi(); // 参数集页：结构树 + 容器/码流对比 + 不一致表
    // 字幕的「过短 / 过长 / 阅读速度」阈值以「规则与阈值」那张可编辑的表为准，
    // 每次扫描前同步一次，避免选项与规则两处阈值各说各话。
    void SyncSubtitleThresholds(videoeye::AnalysisOptions& options);
    void OnStreamingRefreshRequested();
    void UpdateStreamingUi(); // 流媒体包页：结构树 + ladder + 分片时间轴 + 问题

    // 分析功能开关
    QMap<AnalysisFeature, bool> feature_enabled_;

    // 页面管理 (替代 QTabWidget)
    QList<QWidget*> page_widgets_;             // 各页面的 QScrollArea (含内容)
    QStringList page_titles_;                  // 各页面标题
    QStackedWidget* external_stack_ = nullptr; // 外部 QStackedWidget (由 MainWindow 提供)

    // 各分析页的指针登记表（由 AnalysisPageFactory 建好并整体交回来）。
    //
    // 以前是 15 个裸成员各自声明、各自在 SetupXxx 里赋值；现在合成一份结构体，
    // 于是"加了页但忘了转发数据"这类漏接在代码上更难发生 —— 转发处读的是同一张表。
    AnalysisPages pages_;

    // 面板仍持有这一份画面质量选项，作为新页面的初值与「重新发射开关」时的基准
    model::VisualDefectOptions visual_defect_options_;

    // 共用同一次全文件扫描的页面注册表。换文件、扫描起止、进度都按它循环转发。
    std::vector<ScanClient*> scan_clients_;

    // 只把页面登记进注册表（换文件 / 扫描起止 / 进度一起作用到它）。
    // 用它给"扫描意图信号已自行接线"的页面（流媒体包 / 编码参数集 / 字幕辅助）
    // 补齐共享扫描状态，不重复接请求信号。
    void AddScanClient(ScanClient* page);

    // 注册一个共用扫描的页面：入队 + 把 ScanRequested / CancelRequested 接到诊断页总控。
    // 适用于码率 GOP / 音频 QC / 色彩 HDR 这类自带 ScanRequested/CancelRequested 的页面。
    // 模板定义在 .cpp —— 只有面板自己实例化，不需要把诊断页细节带进这个头。
    template <typename PageT>
    void RegisterScanPage(PageT* page);

    // 定时器
    QTimer* update_timer_;

    // 当前视频文件路径 (供导出报告使用)
    std::string current_video_path_;

    // 全文件扫描是否进行中（OnScanStarted/OnScanEnded 维护，供首页取状态）
    bool scan_running_ = false;
};

} // namespace ui
} // namespace videoeye

// 注意: 跨线程信号/槽传递的元类型注册统一在 AnalysisPanel::SetupUI 中
// 通过 qRegisterMetaType<T>() 完成。Qt 6.8 的 QMetaType::fromType<T>() 会自动
// 为普通结构体生成元类型接口, 无需 (且不应) 在此处使用 Q_DECLARE_METATYPE,
// 否则会与 moc 触发的自动注册冲突 (C2908 "QMetaTypeId 已实例化")。
