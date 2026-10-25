#pragma once

// 分析页的建页工厂 + 页面注册表。
//
// 以前 AnalysisPanel 有十几个 SetupXxx()，每个都混着做四件事：
//   1. new 一个页面组件（外加 setMinimumHeight / SetSourcePath 这类摆放细节）；
//   2. 注入 feature 钩子 —— 把页面内部的开关编号映射到全局 AnalysisFeature；
//   3. 调 AddPageWithScroll 把页面连同导航元数据交给外部 stack；
//   4. （个别页）把页面发出的意图信号接到面板。
//
// 1~3 是**建页**，是本文件的事；4 是**跨页接线**，属于协调层，仍留在面板
// （WireAnalysisPages）—— 页面之间不互相持有指针这条规矩不能因为搬代码而破掉。
//
// 建页按 AnalysisPageCatalog 的条目循环（见 AnalysisPageCatalog.h）：顺序由数据表
// 决定，不再由调用次序决定。工厂不认识面板 —— 需要的一切由 PageBuildDeps 注入，
// 于是"页面组件只认识自己的开关编号"这件事也跟着收口到这里。

#include <QMap>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/VisualDefectOptions.h"

#include "ui/analysis_panel/AnalysisPageCatalog.h"
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
#include "ui/bitstream_panel/BitstreamPanel.h"
#include "ui/reporting_panel/ReportingPanel.h"
#include "ui/streaming_panel/StreamingPanel.h"

namespace videoeye {
namespace ui {

// 建页所需的外部能力（面板注入）。全部是惰性求值 —— 建页顺序不保证诊断页已经建好，
// 而若干页面钩子里要用它，所以一律以回调形式在**调用时**取，不在建页时取快照。
struct PageBuildDeps {
    // 页面控件的宿主（Qt 父子关系用）
    QWidget* parent = nullptr;
    // 工厂内部那条唯一的跨页连接（GOP 摘要桥）的 connect 上下文 = 面板对象
    QObject* signal_context = nullptr;

    // 过 tr()。以回调注入而不是在工厂里写 tr()，是为了保留 AnalysisPanel 这个
    // 翻译上下文（工厂是自由函数，没有 QObject 的 tr 可用）。
    std::function<QString(const char*)> translate;

    // 读 / 写全局功能开关。写的一侧要顺带把开关变化发出去（面板发
    // AnalysisFeatureToggled），页面组件自己不认识这个枚举。
    std::function<bool(model::AnalysisFeature)> is_feature_on;
    std::function<void(model::AnalysisFeature, bool)> set_feature;

    // 把页面连同导航元数据交给外部 stack（面板的 AddPageWithScroll）
    std::function<void(QWidget* widget, const QString& title, const QString& page_id, const QString& group_id,
                       int order, const QString& scope)>
        add_page;

    // 当前文件路径（导出 CSV 的默认名 / 扫描输入），以及画面质量页的选项初值
    std::function<QString()> current_path;
    std::function<model::VisualDefectOptions()> visual_defect_options;
};

// 建出来的页面指针。面板持有它，播放期转发数据、扫描终态分发结果都从这里取。
struct AnalysisPages {
    // 「播放统计与帧包」一页由两个组件拼成（顶部流概览 + 底部帧/包明细）
    StreamOverviewView* stream_overview = nullptr;
    FramePacketView* frame_packet = nullptr;

    MacroblockView* macroblock = nullptr;
    EventTimelineView* event_timeline = nullptr;
    ContainerStructurePage* container = nullptr;
    SceneChangePage* scene_change = nullptr;
    VisualDefectPage* visual_defect = nullptr;
    BitrateGopPage* bitrate_gop = nullptr;
    AudioQcPage* audio_qc = nullptr;
    ColorHdrPage* color_hdr = nullptr;
    BitstreamPanel* bitstream_params = nullptr;
    StreamingPanel* streaming = nullptr;
    SubtitleAuxPage* subtitle_aux = nullptr;
    DiagnosticsPage* diagnostics = nullptr;
    ReportingPanel* reporting = nullptr;
};

// 按目录顺序建页。返回的每一页都已经过 add_page 登记（顺序 == stack 页序号）。
AnalysisPages BuildAnalysisPages(const PageBuildDeps& deps);

// ---- 侧栏导航标记 ----
// 每个分析页的"异常数 + 级别 + 是否共用扫描中"。MainWindow 在 DiagnosticsStateChanged
// 后拿它写进侧栏 item 的数据角色，由 SidebarItemDelegate 画成右侧徽标 / 扫描圆点。
// 只有真有异常口径的页面才进表（issue_count>0 或 scanning），帧表/包表这类明细表
// 故意不进 —— 几千行的徽标只是噪音。
struct PageBadge {
    int issue_count = 0;   // 异常条数（0 = 不显示数字徽标）
    int level = 0;         // 0 无 / 1 警告 / 2 错误 / 3 致命（决定徽标配色）
    bool scanning = false; // 该页共用的一次全文件扫描正在进行
};

QMap<QString, PageBadge> CollectPageBadges(const AnalysisPages& pages, bool scanning);

} // namespace ui
} // namespace videoeye
