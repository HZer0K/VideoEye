#pragma once

// 侧栏导航的元数据模型（从 MainWindow 抽出，便于单测）。
//
// 页面在创建时登记稳定的 pageId、显示标题、分组 id、组内排序键与口径（播放期/全文件）；
// 侧栏由这些元数据生成，而不是依赖固定页序与硬编码分组下标 —— 以后增减或重排页面时，
// 只需改页面自己的登记，不必再动「预期页数」和分组范围（那种写法一旦漏改就会静默错位）。
//
// 与 report_path.h / result_stamp.h / macroblock_coordination.h 同一思路：
// 能被测试直接调用的逻辑抽成不依赖 QWidget 的纯结构。

#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace videoeye {
namespace ui {

// 一个分组：id 稳定（用于页面登记与排序），title 本地化显示。
struct NavGroup {
    QString id;
    QString title;
};

// 单个页面的导航元数据。
struct PageNavigation {
    QString page_id;    // 稳定标识（可空表示未登记，仅用于排错）
    QString title;      // 显示标题（已本地化）
    QString group_id;   // 分组 id；空表示不归入任何分组，排到最后
    QString scope;      // 口径标注："播放期" / "全文件"；空表示不标
    int order = 0;      // 组内排序键（小的在前）
    int stack_index = -1;
};

// 侧栏一行：分组标题（is_header = true，不可选中）或页面项（带 stack_index）。
struct NavRow {
    QString text;
    int stack_index = -1;
    bool is_header = false;
};

// 把页面元数据排成侧栏行：
//   * 分组按 groups 的顺序出现；组内按 order 升序，order 相同按 stack_index；
//   * 「多于一个页面」的分组前加一条分组标题；只有一个页面的分组不加标题 ——
//     标题不可点击，紧跟唯一页面时两者样式接近、容易被当成两条重复项
//     （曾出现「概览」与「媒体信息」长得一样、却只有后者可点）；
//   * 页面标题只保留标题本身，不再拼「（播放期）/（全文件）」后缀；口径上提到分组标题，
//     由组内已声明口径的页面汇总（见 group_scope）：若组内口径一致就把「（口径）」并入
//     分组标题（如「播放监看（播放期）」「全片质量（全文件）」），既区分了两种 GOP 统计
//     （播放期摘要 vs 全文件「码率与 GOP」），又不必在每个页面标题里重复；
//   * 未登记分组的页面排到最后，保持 stack 顺序，不加标题头。
inline QVector<NavRow> BuildNavigationRows(const QVector<PageNavigation>& pages,
                                           const QVector<NavGroup>& groups) {
    QVector<PageNavigation> sorted = pages;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const PageNavigation& a, const PageNavigation& b) {
                         if (a.order != b.order) return a.order < b.order;
                         return a.stack_index < b.stack_index;
                     });

    // 页面项只显示标题本身，口径统一上提到分组标题，不再逐页重复后缀。
    auto append_items = [](QVector<NavRow>* rows, const QVector<PageNavigation>& items) {
        for (const PageNavigation& p : items) {
            NavRow row;
            row.stack_index = p.stack_index;
            row.text = p.title;
            rows->append(row);
        }
    };

    // 分组口径：只看组内已声明口径的页面。全部一致 -> 返回该口径并标到分组标题上；
    // 不一致（未来出现混合口径的组）-> 返回空，宁可不标也不误导；组内都没标 -> 空。
    auto group_scope = [](const QVector<PageNavigation>& items) -> QString {
        QString scope;
        for (const PageNavigation& p : items) {
            if (p.scope.isEmpty()) continue;
            if (scope.isEmpty()) {
                scope = p.scope;
            } else if (scope != p.scope) {
                return QString();
            }
        }
        return scope;
    };

    QVector<NavRow> rows;
    QVector<bool> used(sorted.size(), false);
    for (const NavGroup& group : groups) {
        QVector<PageNavigation> in_group;
        for (int i = 0; i < sorted.size(); ++i) {
            if (sorted[i].group_id == group.id) {
                in_group.append(sorted[i]);
                used[i] = true;
            }
        }
        if (in_group.isEmpty()) continue;

        // 单页分组不显示标题：标题不可点击，紧跟唯一页面时会被误认成重复页面项。
        if (in_group.size() > 1) {
            const QString scope = group_scope(in_group);
            NavRow header;
            header.text = scope.isEmpty()
                              ? group.title
                              : QStringLiteral("%1（%2）").arg(group.title, scope);
            header.is_header = true;
            rows.append(header);
        }
        append_items(&rows, in_group);
    }

    QVector<PageNavigation> leftovers;
    for (int i = 0; i < sorted.size(); ++i) {
        if (!used[i]) leftovers.append(sorted[i]);
    }
    std::stable_sort(leftovers.begin(), leftovers.end(),
                     [](const PageNavigation& a, const PageNavigation& b) {
                         return a.stack_index < b.stack_index;
                     });
    append_items(&rows, leftovers);
    return rows;
}

}  // namespace ui
}  // namespace videoeye
