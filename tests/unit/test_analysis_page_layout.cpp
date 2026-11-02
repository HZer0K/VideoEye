// 全部分析页的布局回归：每一页的控件都必须真的进布局、互不压字。
//
// 为什么要有这个（而不只是 test_overview_page_layout.cpp）
// ------------------------------------------------------
// 2026-10-29 的现场：OverviewPage::BuildUi 里 new 了「分析状态」分组框、也给它建了
// 布局、塞了内容，唯独漏了 `root->addWidget(analysis_group_)` —— 控件不进布局就永远
// 停在默认几何 100x30 @ (0,0)，被画在页头上面与标题叠字。数据链路完全正确，
// 纯逻辑测试（OverviewAnalysisTests）全绿，只有比对实际几何才抓得住。
//
// 那次只补了总览页这一页的守卫。可是分析页有 14 个，建页方式是同一套
// （AnalysisPageFactory 按目录循环 + 各页自己的 SetupUi），同一个手误在任何一页
// 都可能重犯。这里按 AnalysisPageCatalog 把所有页都建出来，逐页校验几何。
//
// 判据（任一条失败即红）
// ----------------------
//   1) 页面的可见控件里，**同一父下**的两个兄弟不得几何重叠（>2px 才算，容忍圆角/边框）；
//   2) 不得出现"停在构造默认几何"的可见控件 —— 这是没进布局的强特征；
//   3) 套在 QScrollArea 里的页面内容高度不得小于自身 minimumSizeHint 的高度
//      （widgetResizable 下内容至少该拿到最小尺寸，不够说明被压扁了）。
//
// 判据 1/2 的实现在 tests/support/WidgetLayoutScan.h，与「报告与批量 QC」页
// （test_reporting_panel_layout.cpp）共用同一份 —— 那个手误在那一页同样会犯。
//
// 两件容易被忽略的事（都量过，不是凭感觉加的）
// --------------------------------------------
// A) **必须逐 tab 扫**。分析页大量用 QTabWidget 分包（视频帧 / 音频帧 / 数据包、
//    事件 / 同步 / 时间轴、结构树 / 元数据…），QTabWidget 只给当前页几何，其余页
//    压根没排过。只扫默认 tab 能看到 365 个控件，逐 tab 全切能看到 529 个（+45%）——
//    换句话说，漏 addWidget 的手误藏在非首个 tab 里时，只扫默认 tab 一辈子发现不了。
//
// B) **必须有一轮有数据**。空态下容器树、流表、元数据表是隐藏的；喂一份
//    ContainerStructureResult 之后「文件结构」页的可见控件从 34 涨到 90。数据只喂
//    这一页：其余各页的"有数据 vs 空态"差分都在 ±2 以内，造那些数据换不来覆盖。
//    详见 tests/support/AnalysisPageDataFixture.h 的说明。
//
// 为了让上面两条不会悄悄退化成空操作，用例末尾有一条**覆盖守护**断言（见 kMin*）：
// tab 遍历或数据处理一旦失效，它会红并直接说明是哪一条失效了。
//
// 已知局限
// --------
//   * 质量诊断页只有空态覆盖 —— 它要 AnalysisFacade 出内容，那会把纯布局用例变成
//     集成用例（起线程 / 读文件）。报告页由 test_reporting_panel_layout.cpp 单独守
//     （它不在 AnalysisPageCatalog 里，是 MainWindow 直接 new 的）。
//   * 嵌套 QTabWidget 只保证外层被激活到（内层在外层隐藏时切不出几何）。
//   * 像素级截图比对是另一条路，但 offscreen 下字体度量跟真实平台有差异，判据不稳。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <string>
#include <vector>

#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/VisualDefectOptions.h"
#include "tests/support/AnalysisPageDataFixture.h"
#include "tests/support/WidgetLayoutScan.h"
#include "ui/analysis_panel/AnalysisPageCatalog.h"
#include "ui/analysis_panel/AnalysisPageFactory.h"

namespace {

using videoeye::model::AnalysisFeature;
using videoeye::model::VisualDefectOptions;
using namespace videoeye::ui;

// 判据本身在 tests/support/WidgetLayoutScan.h（报告与批量 QC 页也用同一份）。
using videoeye_test::ScanEveryTabState;
using videoeye_test::TabScanResult;

// 与 test_overview_page_layout.cpp 同款：只构造、不渲染，跑 offscreen 平台。
QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_analysis_page_layout";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 一套建好的页面 + 承载它们的宿主。页面轮流进宿主的布局里量几何。
struct PageSet {
    QWidget host;
    QVBoxLayout* layout = nullptr;
    std::vector<QWidget*> pages;
    std::vector<std::string> names;
    AnalysisPages handles;
};

// 按目录建出全部页面。返回 false 表示建页数与目录表对不上（断言已写好，调用方直接 return）。
bool BuildAllPages(PageSet& set) {
    set.host.resize(1000, 760);
    set.layout = new QVBoxLayout(&set.host);
    set.layout->setContentsMargins(0, 0, 0, 0);

    PageBuildDeps deps;
    deps.parent = &set.host;
    deps.signal_context = &set.host;
    deps.translate = [](const char* text) { return QString::fromUtf8(text); };
    deps.is_feature_on = [](AnalysisFeature) { return true; };
    deps.set_feature = [](AnalysisFeature, bool) {};
    deps.add_page = [&set](QWidget* widget, const QString& title, const QString& page_id, const QString&, int,
                           const QString&) {
        auto* scroll = new QScrollArea(&set.host);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(widget);
        set.pages.push_back(scroll);
        set.names.push_back(page_id.toStdString() + "(" + title.toStdString() + ")");
    };
    deps.current_path = [] { return QString(); };
    deps.visual_defect_options = [] { return VisualDefectOptions{}; };

    set.handles = BuildAnalysisPages(deps);

    // 建页数量必须与目录表一致：多一页少一页都说明建页循环跟目录脱钩了。
    EXPECT_EQ(set.pages.size(), AnalysisPageCatalog().size())
        << "建出的页面数与 AnalysisPageCatalog 对不上，先看 AnalysisPageFactory 的循环";
    return set.pages.size() == AnalysisPageCatalog().size();
}

// 一次完整扫描的统计，供后面的覆盖守护断言用。
struct ScanOutcome {
    std::vector<std::string> findings;
    int visible_union = 0;  // 所有 tab 状态下出现过的不同控件数
    int improved_pages = 0; // 切换 tab 后可见控件变多的页数
};

// 逐页扫：每个 tab 都激活一次并重扫，量的是"每种排布"而不是"默认排布"。
ScanOutcome ScanAllPages(PageSet& set) {
    ScanOutcome outcome;
    for (size_t i = 0; i < set.pages.size(); ++i) {
        QWidget* page = set.pages[i];
        // 让滚动区自己去布局，别手工 resize 页面 —— 手工压到小于 minimumSizeHint
        // 会让 QVBoxLayout 把子控件挤成重叠，那是测量方式造出来的假象，不是产品的 bug。
        set.layout->addWidget(page);
        page->show();
        set.host.show();
        QApplication::processEvents();
        QApplication::processEvents();

        const TabScanResult scanned = ScanEveryTabState(page, set.names[i], outcome.findings);

        outcome.visible_union += static_cast<int>(scanned.all_states);
        if (scanned.all_states > scanned.first_state)
            ++outcome.improved_pages;

        // 判据 3：滚动区内容不该比自身最小尺寸还矮（在默认 tab 状态下量）。
        if (auto* area = qobject_cast<QScrollArea*>(page)) {
            if (QWidget* content = area->widget()) {
                const int wanted = content->minimumSizeHint().height();
                if (content->height() > 0 && content->height() < wanted) {
                    outcome.findings.push_back(set.names[i] + ": 内容被压扁 —— 实际高 " +
                                               std::to_string(content->height()) + " < 最小高 " +
                                               std::to_string(wanted) + "（滚动层没生效？）");
                }
            }
        }

        set.layout->removeWidget(page);
        page->hide();
    }
    return outcome;
}

// 覆盖守护的阈值。取得很松，是为了容忍正常改版：多一页少一页 tab、多一列明细
// 都不该让它红；但"机制失效"会让它大幅掉下去。
constexpr int kMinImprovedPages = 6;
constexpr int kMinVisibleUnion = 400;

} // namespace

TEST(AnalysisPageLayout, PagesRemainScrollableAfterShrinkingAndGrowing) {
    EnsureApp();
    PageSet set;
    ASSERT_TRUE(BuildAllPages(set));
    videoeye_test::FeedContainerData(set.handles, videoeye_test::MakeRichContainerStructure());

    for (const QSize& size : {QSize(900, 300), QSize(1200, 760), QSize(900, 300)}) {
        set.host.resize(size);
        const ScanOutcome outcome = ScanAllPages(set);
        for (const std::string& finding : outcome.findings)
            ADD_FAILURE() << size.width() << "x" << size.height() << ": " << finding;
        EXPECT_EQ(set.host.size(), size);
    }
}

// 一、空态；二、喂一份容器结构再来一遍。两轮都要干净，且第二轮必须真的看到更多控件。
TEST(AnalysisPageLayout, EveryPageKeepsItsControlsInsideLayouts) {
    EnsureApp();

    PageSet set;
    ASSERT_TRUE(BuildAllPages(set));

    // ---- 第一轮：空态 ----
    const ScanOutcome empty = ScanAllPages(set);
    for (const std::string& finding : empty.findings)
        ADD_FAILURE() << finding;

    // ---- 第二轮：喂「文件结构」页一份容器结构 ----
    // 容器树 / 流表 / 元数据表在空态是隐藏的，只有这一轮才参与几何比对。
    videoeye_test::FeedContainerData(set.handles, videoeye_test::MakeRichContainerStructure());
    for (int i = 0; i < 3; ++i)
        QApplication::processEvents();

    const ScanOutcome with_data = ScanAllPages(set);
    for (const std::string& finding : with_data.findings)
        ADD_FAILURE() << finding;

    // ---- 覆盖守护 ----
    // 这两件事都是"做了才有用、坏了看不出来"：tab 被换成 QStackedWidget、或者
    // SetResult 不再渲染旧字段，用例都会继续全绿，但它守的控件会从 529 掉回 365。
    EXPECT_GE(empty.improved_pages, kMinImprovedPages)
        << "切换 tab 后几乎没多看到控件（" << empty.improved_pages
        << " 页）—— QTabWidget 可能被换掉了，"
           "逐 tab 扫描已失效。覆盖会悄悄退化成只看默认 tab，这条断言就是为了让它红。";
    EXPECT_GE(empty.visible_union, kMinVisibleUnion)
        << "扫到的可见控件总数异常地少（" << empty.visible_union << "），先确认页面真的建出来了";
    EXPECT_GT(with_data.visible_union, empty.visible_union)
        << "喂了容器结构却没多出任何可见控件（" << empty.visible_union << " -> " << with_data.visible_union
        << "）：要么 ContainerStructurePage::SetResult 不再渲染旧字段，要么 fixture 的字段被清空了。"
           "这一轮的覆盖已经退化成和空态一样，没有意义。";
}

// ---------------------------------------------------------------------------
// 页头一致性：分析入口与「导出」在每一页的位置、个数都必须一样
//
// 现场（2026-11-02）：同样是"启用分析"，
//   * 播放统计与帧包：页头一个 + 视频帧/包/音频帧子页各一个（同一页 4 个同名勾选框），
//     而且页头那个其实只管流统计，看着却像整页总闸；
//   * 事件与时间轴：页头只有「导出」，三个开关散在子页工具栏最右侧；
//   * 编码参数集 / 字幕与辅助：页头是「开始分析」按钮，没有开关。
// 于是用户每换一页都要重新找"分析开关在哪、导出在哪"。
//
// 规范（本用例钉住）：
//   1) 播放期页（实时采集）页头**恰好一个**「启用分析」勾选框（多采集项页用聚合开关）；
//   2) 全文件页（一次性扫描）页头**恰好一个**「开始分析」按钮，且没有「启用分析」；
//   3) 任何页都不能同时出现两种入口，也不能一个都没有（工具页除外）；
//   4) 每页恰好一个「导出」，且与分析入口同一行、位于其右侧。
// 这几条都是"改了不报错、只是界面又乱了"的那类约束，只能靠断言守。
// ---------------------------------------------------------------------------
TEST(AnalysisPageHeader, AnalysisEntryAndExportSitOnTheSameHeaderRow) {
    EnsureApp();
    PageSet set;
    ASSERT_TRUE(BuildAllPages(set));

    // 按页面 id 声明口径（AnalysisPageCatalog 的 scope 字段对 event_timeline 是空的，
    // 不能拿它当判据 —— 这里显式列，新增页面时也必须来这边选一边）。
    const QSet<QString> playback_pages = {
        QStringLiteral("playback_stats_packets"), QStringLiteral("event_timeline"), QStringLiteral("file_structure"),
        QStringLiteral("motion_vectors"),         QStringLiteral("scene_change"),   QStringLiteral("visual_quality"),
    };
    const QSet<QString> full_file_pages = {
        QStringLiteral("bitrate_gop"),         QStringLiteral("audio_qc"),          QStringLiteral("color_hdr"),
        QStringLiteral("codec_parameters"),    QStringLiteral("streaming_packets"), QStringLiteral("subtitle_aux"),
        QStringLiteral("quality_diagnostics"),
    };

    const auto& catalog = AnalysisPageCatalog();
    ASSERT_EQ(catalog.size(), set.pages.size());

    int playback_seen = 0;
    int full_file_seen = 0;
    int tool_seen = 0;

    for (size_t i = 0; i < catalog.size(); ++i) {
        const QString id = QString::fromUtf8(catalog[i].id);
        auto* scroll = qobject_cast<QScrollArea*>(set.pages[i]);
        ASSERT_TRUE(scroll != nullptr);
        QWidget* page = scroll->widget();
        ASSERT_TRUE(page != nullptr);

        // 让布局真正算过一次，才能量同一行的坐标
        set.layout->addWidget(set.pages[i]);
        set.host.show();
        set.pages[i]->show();
        QApplication::processEvents();
        QApplication::processEvents();

        QList<QWidget*> entries;
        for (QCheckBox* box : page->findChildren<QCheckBox*>()) {
            if (box->text() == QStringLiteral("启用分析"))
                entries.append(box);
        }
        for (QPushButton* button : page->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("开始分析"))
                entries.append(button);
        }
        const auto exports = page->findChildren<QPushButton*>(QStringLiteral("ExportMenuButton"));

        const std::string where = id.toStdString() + "（" + QString::fromUtf8(catalog[i].title).toStdString() + "）";

        EXPECT_EQ(exports.size(), 1) << where << " 的「导出」入口不是恰好一个";

        if (playback_pages.contains(id)) {
            ++playback_seen;
            EXPECT_EQ(entries.size(), 1) << where << " 应恰好一个分析入口（播放期页 = 一个「启用分析」），实际 "
                                         << entries.size();
            if (entries.size() == 1) {
                EXPECT_TRUE(qobject_cast<QCheckBox*>(entries.front()) != nullptr)
                    << where << " 的播放期分析入口应是「启用分析」勾选框";
            }
            // 注意：objectName 不做统一要求 —— 画面质量页的勾选框叫 VisualDefectEnable，
            // 已有测试按它定位。一致性靠"文本 + 数量 + 位置"判定。
        } else if (full_file_pages.contains(id)) {
            ++full_file_seen;
            EXPECT_EQ(entries.size(), 1) << where << " 应恰好一个分析入口（全文件页 = 一个「开始分析」按钮），实际 "
                                         << entries.size();
            if (entries.size() == 1) {
                EXPECT_TRUE(qobject_cast<QPushButton*>(entries.front()) != nullptr)
                    << where << " 的全文件分析入口应是「开始分析」按钮";
            }
            for (QCheckBox* box : page->findChildren<QCheckBox*>()) {
                EXPECT_NE(box->text(), QStringLiteral("启用分析"))
                    << where << " 是全文件扫描页，不该出现常驻「启用分析」开关";
            }
        } else {
            ++tool_seen;
            EXPECT_TRUE(entries.isEmpty()) << where << " 是工具页，不该挂分析入口";
        }

        // 同一行 + 分析入口在导出左侧
        if (entries.size() == 1 && exports.size() == 1) {
            const QPoint entry_pos = entries.front()->mapTo(page, QPoint(0, 0));
            const QPoint export_pos = exports.front()->mapTo(page, QPoint(0, 0));
            EXPECT_LE(std::abs(entry_pos.y() - export_pos.y()), 6)
                << where << " 的分析入口与「导出」不在同一行（y=" << entry_pos.y() << " / " << export_pos.y()
                << "）—— 页头应当只有一行：[标题] … [分析入口] [导出]";
            EXPECT_LT(entry_pos.x(), export_pos.x())
                << where << " 的分析入口应位于「导出」左侧（x=" << entry_pos.x() << " / " << export_pos.x() << "）";
        }

        set.layout->removeWidget(set.pages[i]);
        set.pages[i]->hide();
    }

    // 覆盖守护：三个集合一旦与实际页面脱钩（新增页没归类 / id 写错），上面的循环会
    // 悄悄少查几页而不报错 —— 这三条把"少查了"变成失败。
    EXPECT_EQ(playback_seen, playback_pages.size()) << "有播放期页没被检查到（id 与目录对不上？）";
    EXPECT_EQ(full_file_seen, full_file_pages.size()) << "有全文件页没被检查到（id 与目录对不上？）";
    EXPECT_EQ(playback_seen + full_file_seen + tool_seen, static_cast<int>(catalog.size()))
        << "有页面既不属于播放期也不属于全文件，且没被当作工具页统计";
}
