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
//   2) 不得出现"默认几何 100x30 @ (0,0)"的可见控件 —— 这是没进布局的强特征；
//   3) 套在 QScrollArea 里的页面内容高度不得小于自身 minimumSizeHint 的高度
//      （widgetResizable 下内容至少该拿到最小尺寸，不够说明被压扁了）。
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
//   * 质量诊断页与报告页只有空态覆盖 —— 它们要 AnalysisFacade 出内容，那会把
//     纯布局用例变成集成用例（起线程 / 读文件）。
//   * 嵌套 QTabWidget 只保证外层被激活到（内层在外层隐藏时切不出几何）。
//   * 像素级截图比对是另一条路，但 offscreen 下字体度量跟真实平台有差异，判据不稳。

#include <gtest/gtest.h>

#include <QApplication>
#include <QGroupBox>
#include <QLayout>
#include <QRect>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <set>
#include <string>
#include <vector>

#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/VisualDefectOptions.h"
#include "tests/support/AnalysisPageDataFixture.h"
#include "ui/analysis_panel/AnalysisPageCatalog.h"
#include "ui/analysis_panel/AnalysisPageFactory.h"

namespace {

using videoeye::model::AnalysisFeature;
using videoeye::model::VisualDefectOptions;
using namespace videoeye::ui;

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

std::string Describe(const QWidget* w) {
    return std::string(w->metaObject()->className()) + " geo=" + std::to_string(w->x()) + "," + std::to_string(w->y()) +
           " " + std::to_string(w->width()) + "x" + std::to_string(w->height());
}

// 判据 1：同一父下可见兄弟两两不重叠。
void CollectSiblingOverlaps(QWidget* root, const std::string& where, std::vector<std::string>& out) {
    std::vector<QWidget*> pending{root};
    while (!pending.empty()) {
        QWidget* parent = pending.back();
        pending.pop_back();

        std::vector<QWidget*> visible;
        for (QObject* child : parent->children()) {
            auto* w = qobject_cast<QWidget*>(child);
            if (!w || w->isWindow())
                continue;
            pending.push_back(w);
            if (!w->isVisible() || w->width() <= 0 || w->height() <= 0)
                continue;
            visible.push_back(w);
        }

        for (size_t i = 0; i < visible.size(); ++i) {
            for (size_t j = i + 1; j < visible.size(); ++j) {
                const QRect overlap = visible[i]->geometry().intersected(visible[j]->geometry());
                // 2px 以内不算：边框、圆角、焦点框会有一两像素的合法交叠。
                if (overlap.width() > 2 && overlap.height() > 2) {
                    out.push_back(where + ": 兄弟控件重叠 —— " + Describe(visible[i]) + "  X  " + Describe(visible[j]) +
                                  "（父 " + parent->metaObject()->className() + "）");
                }
            }
        }
    }
}

// 判据 2：默认几何 = 没进布局。
void CollectDefaultGeometry(QWidget* root, const std::string& where, std::vector<std::string>& out) {
    std::vector<QWidget*> pending{root};
    while (!pending.empty()) {
        QWidget* parent = pending.back();
        pending.pop_back();
        for (QObject* child : parent->children()) {
            auto* w = qobject_cast<QWidget*>(child);
            if (!w || w->isWindow())
                continue;
            pending.push_back(w);
            // 100x30 @ (0,0) 是 QWidget 未进任何布局时的默认几何 —— 它必然压在页面
            // 左上角的页头/分组标题上。真有一个天生就这么大的控件也认了：先让它红，
            // 再决定是放宽判据还是修布局，总比静默叠字强。
            if (w->isVisible() && w->x() == 0 && w->y() == 0 && w->width() == 100 && w->height() == 30)
                out.push_back(where + ": 可见控件停在默认几何（疑似漏 addWidget）—— " + Describe(w));
        }
    }
}

// 扫一遍当前可见树：前两条判据 + 把这次看到的可见控件记进 seen（用于并集统计）。
void ScanCurrentState(QWidget* page, const std::string& where, std::vector<std::string>& findings,
                      std::set<const QWidget*>& seen) {
    CollectSiblingOverlaps(page, where, findings);
    CollectDefaultGeometry(page, where, findings);

    std::vector<QWidget*> pending{page};
    while (!pending.empty()) {
        QWidget* parent = pending.back();
        pending.pop_back();
        for (QObject* child : parent->children()) {
            auto* w = qobject_cast<QWidget*>(child);
            if (!w || w->isWindow())
                continue;
            pending.push_back(w);
            if (w->isVisible())
                seen.insert(w);
        }
    }
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
        set.pages.push_back(widget);
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

        std::set<const QWidget*> seen;
        ScanCurrentState(page, set.names[i], outcome.findings, seen);
        const size_t first_state = seen.size();

        for (QTabWidget* tabs : page->findChildren<QTabWidget*>()) {
            for (int k = 1; k < tabs->count(); ++k) { // 第 0 页上面已经扫过
                tabs->setCurrentIndex(k);
                QApplication::processEvents();
                QApplication::processEvents();
                ScanCurrentState(page, set.names[i] + " tab" + std::to_string(k) + "/" + std::to_string(tabs->count()),
                                 outcome.findings, seen);
            }
            tabs->setCurrentIndex(0);
        }
        QApplication::processEvents();

        outcome.visible_union += static_cast<int>(seen.size());
        if (seen.size() > first_state)
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
