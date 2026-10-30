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
// 覆盖面与已知局限（写清楚，别让后来者以为它万能）
// ------------------------------------------------
// 只喂空态：多数明细表（帧表/包表/规则表）在空态是隐藏的，所以第 1 条对它们
// 无覆盖 —— 漏 addWidget 的**容器类**控件（分组框/表格骨架/页头）才是本用例的主战场。
// 要覆盖"有数据时的叠字"，得先喂一份真实 AnalysisResult 再扫；那是下一步的事。
// 另一条路是像素级截图比对，但 offscreen 下字体度量跟真实平台有差异，判据不稳。

#include <gtest/gtest.h>

#include <QApplication>
#include <QGroupBox>
#include <QLayout>
#include <QRect>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <vector>

#include "core/domain/model/AnalysisFeature.h"
#include "core/domain/model/VisualDefectOptions.h"
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
void CollectSiblingOverlaps(QWidget* root, const std::string& page, std::vector<std::string>& out) {
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
                    out.push_back(page + ": 兄弟控件重叠 —— " + Describe(visible[i]) + "  X  " + Describe(visible[j]) +
                                  "（父 " + parent->metaObject()->className() + "）");
                }
            }
        }
    }
}

// 判据 2：默认几何 = 没进布局。
void CollectDefaultGeometry(QWidget* root, const std::string& page, std::vector<std::string>& out) {
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
                out.push_back(page + ": 可见控件停在默认几何（疑似漏 addWidget）—— " + Describe(w));
        }
    }
}

} // namespace

// 把所有分析页建出来，逐页按三条判据校验。
TEST(AnalysisPageLayout, EveryPageKeepsItsControlsInsideLayouts) {
    EnsureApp();

    QWidget host;
    host.resize(1000, 760);
    auto* host_layout = new QVBoxLayout(&host);
    host_layout->setContentsMargins(0, 0, 0, 0);

    std::vector<QWidget*> pages;
    std::vector<std::string> page_names;

    PageBuildDeps deps;
    deps.parent = &host;
    deps.signal_context = &host;
    deps.translate = [](const char* text) { return QString::fromUtf8(text); };
    deps.is_feature_on = [](AnalysisFeature) { return true; };
    deps.set_feature = [](AnalysisFeature, bool) {};
    deps.add_page = [&](QWidget* widget, const QString& title, const QString& page_id, const QString&, int,
                        const QString&) {
        pages.push_back(widget);
        page_names.push_back(page_id.toStdString() + "(" + title.toStdString() + ")");
    };
    deps.current_path = [] { return QString(); };
    deps.visual_defect_options = [] { return VisualDefectOptions{}; };

    BuildAnalysisPages(deps);

    // 建页数量必须与目录表一致：多一页少一页都说明建页循环跟目录脱钩了。
    ASSERT_EQ(pages.size(), AnalysisPageCatalog().size())
        << "建出的页面数与 AnalysisPageCatalog 对不上，先看 AnalysisPageFactory 的循环";

    std::vector<std::string> findings;
    for (size_t i = 0; i < pages.size(); ++i) {
        QWidget* page = pages[i];
        // 让滚动区自己去布局，别手工 resize 页面 —— 手工压到小于 minimumSizeHint
        // 会让 QVBoxLayout 把子控件挤成重叠，那是测量方式造出来的假象，不是产品的 bug。
        host_layout->addWidget(page);
        page->show();
        host.show();
        QApplication::processEvents();
        QApplication::processEvents();

        CollectSiblingOverlaps(page, page_names[i], findings);
        CollectDefaultGeometry(page, page_names[i], findings);

        // 判据 3：滚动区内容不该比自身最小尺寸还矮。
        if (auto* area = qobject_cast<QScrollArea*>(page)) {
            if (QWidget* content = area->widget()) {
                const int wanted = content->minimumSizeHint().height();
                if (content->height() > 0 && content->height() < wanted) {
                    findings.push_back(page_names[i] + ": 内容被压扁 —— 实际高 " + std::to_string(content->height()) +
                                       " < 最小高 " + std::to_string(wanted) + "（滚动层没生效？）");
                }
            }
        }

        host_layout->removeWidget(page);
        page->hide();
    }

    for (const std::string& finding : findings)
        ADD_FAILURE() << finding;
}
