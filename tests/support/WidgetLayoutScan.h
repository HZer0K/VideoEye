#pragma once

// 控件布局回归的公共判据：**同一个手误可能在任意一页重犯**，判据只该有一份。
//
// 起点是 2026-10-29 的现场：OverviewPage::BuildUi 里 new 了「分析状态」分组框、
// 也给它建了布局、塞了内容，唯独漏了 `root->addWidget(analysis_group_)` ——
// 控件不进布局就永远停在默认几何 100x30 @ (0,0)，被画在页头上面与标题叠字。
// 数据链路完全正确，纯逻辑测试全绿，只有比对实际几何才抓得住。
//
// 三个判据（任一条失败即红）
// ------------------------
//   1) 同一父下的可见兄弟不得几何重叠（>2px 才算，容忍圆角/边框）；
//   2) 不得出现"停在构造默认几何"的可见控件 —— 没进布局的强特征
//      （默认几何不是死板的 100x30，见 CollectDefaultGeometry 上的说明）；
//   3) （调用方自己判）套在 QScrollArea 里的内容高度不得小于自身 minimumSizeHint。
//
// 必须逐 tab 扫（量过，不是凭感觉）
// -------------------------------
// 分包页大量用 QTabWidget，而 QTabWidget 只给当前页几何，其余页压根没排过。
// 分析页只扫默认 tab 能看到 365 个控件，逐 tab 全切能看到 529 个（+45%）——
// 换句话说，漏 addWidget 的手误藏在非首个 tab 里时，只扫默认 tab 一辈子发现不了。
// （「报告与批量 QC」页不分包，逐 tab 这一趟在那儿不产出覆盖，只当通用保险。）
//
// 已知局限
// --------
//   * 嵌套 QTabWidget 只保证外层被激活到（内层在外层隐藏时切不出几何）。
//   * 像素级截图比对是另一条路，但 offscreen 下字体度量跟真实平台有差异，判据不稳。
//   * 判据 1 有天然盲区：**两个都漏了 addWidget 的控件**重叠在同一个默认几何上时，
//     判据 2 会先报出来，所以不影响召回。

#include <QApplication>
#include <QRect>
#include <QTabWidget>
#include <QWidget>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace videoeye_test {

inline std::string DescribeWidget(const QWidget* w) {
    return std::string(w->metaObject()->className()) + " geo=" + std::to_string(w->x()) + "," + std::to_string(w->y()) +
           " " + std::to_string(w->width()) + "x" + std::to_string(w->height());
}

// 判据 1：同一父下可见兄弟两两不重叠。
inline void CollectSiblingOverlaps(QWidget* root, const std::string& where, std::vector<std::string>& out) {
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
                    out.push_back(where + ": 兄弟控件重叠 —— " + DescribeWidget(visible[i]) + "  X  " +
                                  DescribeWidget(visible[j]) + "（父 " + parent->metaObject()->className() + "）");
                }
            }
        }
    }
}

// 判据 2：默认几何 = 没进布局。
//
// ❗ 默认几何**不是**死板的 100x30。QWidget 构造时是 100x30，但 setMinimumWidth/Height
// 会把当前尺寸顶上去（setMinimumSize 内部会 resize 到至少最小值），于是同一个"漏
// addWidget"的手误，长出来可能是 100x80（详情标签设了最小高 80）、100x220（结果表设了
// 最小高 220）。2026-10-30 实测：把 BatchScanView 的 `addWidget(batch_detail_)` 注释掉，
// 控件停在 **100x80**，只认 100x30 的写法完全放过它 —— 判据得跟着最小尺寸走。
inline bool LooksLikeUnlaidOutGeometry(const QWidget* w) {
    if (w->x() != 0 || w->y() != 0)
        return false;
    return w->width() == std::max(100, w->minimumWidth()) && w->height() == std::max(30, w->minimumHeight());
}

inline void CollectDefaultGeometry(QWidget* root, const std::string& where, std::vector<std::string>& out) {
    std::vector<QWidget*> pending{root};
    while (!pending.empty()) {
        QWidget* parent = pending.back();
        pending.pop_back();
        for (QObject* child : parent->children()) {
            auto* w = qobject_cast<QWidget*>(child);
            if (!w || w->isWindow())
                continue;
            pending.push_back(w);
            // 停在 (0,0) 且尺寸正好是构造默认值的控件必然压在父容器左上角的页头/
            // 分组标题上。真有一个天生就这么大的控件也认了：先让它红，再决定是放宽
            // 判据还是修布局，总比静默叠字强。
            if (w->isVisible() && LooksLikeUnlaidOutGeometry(w))
                out.push_back(where + ": 可见控件停在默认几何（疑似漏 addWidget）—— " + DescribeWidget(w));
        }
    }
}

// 扫一遍当前可见树：前两条判据 + 把这次看到的可见控件记进 seen（用于并集统计）。
inline void ScanVisibleState(QWidget* root, const std::string& where, std::vector<std::string>& findings,
                             std::set<const QWidget*>& seen) {
    CollectSiblingOverlaps(root, where, findings);
    CollectDefaultGeometry(root, where, findings);

    std::vector<QWidget*> pending{root};
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

// 一次"逐 tab 扫"的结果。两个数都要：first_state 是只扫默认 tab 时的覆盖，
// all_states 是逐 tab 全切之后的覆盖 —— 两者相等就说明逐 tab 这一趟白跑了。
struct TabScanResult {
    size_t first_state = 0;
    size_t all_states = 0;
};

// 扫默认状态，再把每个 QTabWidget 的每个 tab 都激活一次并重扫。
// 调用方负责在此之前 show() + processEvents()，扫完负责切回第 0 页（这里已切回）。
inline TabScanResult ScanEveryTabState(QWidget* page, const std::string& where, std::vector<std::string>& findings) {
    std::set<const QWidget*> seen;
    ScanVisibleState(page, where, findings, seen);
    const size_t first_state = seen.size();

    for (QTabWidget* tabs : page->findChildren<QTabWidget*>()) {
        for (int k = 1; k < tabs->count(); ++k) { // 第 0 页上面已经扫过
            tabs->setCurrentIndex(k);
            QApplication::processEvents();
            QApplication::processEvents();
            ScanVisibleState(page, where + " tab" + std::to_string(k) + "/" + std::to_string(tabs->count()), findings,
                             seen);
        }
        tabs->setCurrentIndex(0);
    }
    QApplication::processEvents();

    return TabScanResult{first_state, seen.size()};
}

} // namespace videoeye_test
