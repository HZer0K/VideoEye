#include "ui/main_window/sidebar_navigation.h"

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QListWidgetItem>
#include <QObject>
#include <QPainter>
#include <QPen>
#include <QSignalBlocker>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVariant>

#include <algorithm>

namespace videoeye {
namespace ui {
namespace {

// 徽标配色（按级别: 1 警告 / 2 错误 / 3 致命）
QColor BadgeColor(int level) {
    switch (level) {
        case 3: return QColor(0xF8, 0x51, 0x49);  // 致命红
        case 2: return QColor(0xFF, 0x7B, 0x72);  // 错误橙红
        case 1: return QColor(0xD2, 0x99, 0x22);  // 警告橙
        default: return QColor(0x8B, 0x94, 0x9E);  // 无级别灰
    }
}

// 侧栏一行分两类：可点击的页面项，与不可点击的分组标题。
// 把 QLabel 当作 item widget 挂在不可选中的标题行上也画不出来（只剩一条空行），
// 所以用委托按行绘制：页面项交给基类（保留 QSS 的悬停/选中样式），分组标题用更小、
// 更暗、加粗的字体画成小节标签。
class SidebarItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        // 分组标题登记时置了 Qt::NoItemFlags，没有 ItemIsSelectable，以此区分两类行。
        const bool is_header = !(index.flags() & Qt::ItemIsSelectable);
        if (!is_header) {
            QStyledItemDelegate::paint(painter, option, index);
            DrawBadge(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();  // 文本自己画，不交给默认样式（否则又会拿到页面项的颜色）
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        painter->save();
        QFont font = opt.font;
        font.setPointSizeF(std::max(7.5, font.pointSizeF() - 2.0));
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(QColor(0x6E, 0x76, 0x81));
        painter->drawText(opt.rect.adjusted(16, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
        painter->restore();
    }

private:
    // 页面项右侧的导航标记: 扫描态画强调色小圆点，异常数画彩色数字徽标。
    // 数据来自 ApplySidebarBadges 写进 item 的角色；两者都没有就不画。
    static void DrawBadge(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) {
        const int count = index.data(kBadgeCountRole).toInt();
        const bool scanning = index.data(kScanningRole).toBool();
        if (count <= 0 && !scanning) return;

        const QRect rect = option.rect;
        const int right = rect.right() - 10;
        painter->save();
        QFont font = option.font;
        font.setPointSizeF(std::max(8.0, font.pointSizeF() - 3.0));
        font.setBold(true);
        painter->setFont(font);

        if (scanning) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0x58, 0xA6, 0xFF));
            painter->drawEllipse(QRect(right - 6, rect.center().y() - 3, 6, 6));
        }
        if (count > 0) {
            const QColor color = BadgeColor(index.data(kBadgeLevelRole).toInt());
            const QString text = count > 99 ? QStringLiteral("99+") : QString::number(count);
            const QFontMetrics fm(font);
            const int pill_width = fm.horizontalAdvance(text) + 10;
            const int pill_right = scanning ? right - 12 : right;
            const QRect pill(pill_right - pill_width, rect.center().y() - 8, pill_width, 16);
            QColor background = color;
            background.setAlpha(38);  // ~15% 透明度底
            painter->setPen(QPen(color, 1.0));
            painter->setBrush(background);
            painter->drawRoundedRect(pill, 8, 8);
            painter->setPen(color);
            painter->drawText(pill, Qt::AlignCenter, text);
        }
        painter->restore();
    }
};

}  // namespace

void InstallSidebarDelegate(QListWidget* sidebar) {
    if (!sidebar) return;
    sidebar->setItemDelegate(new SidebarItemDelegate(sidebar));
}

QVector<PageNavigation> CollectPageNavigation(QStackedWidget* stack) {
    QVector<PageNavigation> pages;
    if (!stack) return pages;
    pages.reserve(stack->count());
    for (int i = 0; i < stack->count(); ++i) {
        QWidget* page = stack->widget(i);
        if (!page) continue;
        PageNavigation nav;
        nav.stack_index = i;
        nav.page_id = page->property("pageId").toString();
        nav.title = page->property("pageTitle").toString();
        nav.group_id = page->property("pageGroup").toString();
        nav.scope = page->property("pageScope").toString();
        nav.order = page->property("pageOrder").toInt();
        // 未登记标题的页面（新增页忘了写属性）也要能在侧栏里点得到，兜一个占位名
        if (nav.title.isEmpty()) nav.title = QObject::tr("页面 %1").arg(i);
        pages.append(nav);
    }
    return pages;
}

QVector<NavGroup> DefaultNavGroups() {
    return {
        {QStringLiteral("overview"), QObject::tr("概览")},
        {QStringLiteral("playback"), QObject::tr("播放监看")},
        {QStringLiteral("file_parse"), QObject::tr("文件解析")},
        {QStringLiteral("full_quality"), QObject::tr("全片质量")},
        {QStringLiteral("report_tools"), QObject::tr("报告与工具")},
    };
}

void ApplyNavigationRows(QListWidget* sidebar, const QVector<NavRow>& rows) {
    if (!sidebar) return;

    // 逐条 addItem 会反复触发 currentRowChanged，而此刻 content_stack_ 与侧栏正处在
    // 半重建状态（条目已铺、页面可能还没同步），按中间态切页会闪 —— 全程静音。
    sidebar->blockSignals(true);
    sidebar->clear();
    for (const NavRow& row : rows) {
        QListWidgetItem* item = new QListWidgetItem(row.text);
        if (row.is_header) {
            // 小节标签：不可选中，委托据此把它画成更小更暗的标题（见 SidebarItemDelegate）。
            item->setFlags(Qt::NoItemFlags);
            item->setSizeHint(QSize(200, 28));
        } else {
            item->setSizeHint(QSize(200, 34));
            item->setData(Qt::UserRole, row.stack_index);
        }
        sidebar->addItem(item);
    }
    sidebar->blockSignals(false);

    // 默认选中第一个页面项（分组标题没有 UserRole，落在它上面不会切换）。
    // 这一次**必须**发信号（信号已在上面恢复）：调用方（MainWindow）正是靠
    // currentRowChanged 才把 content_stack_ 同步到第 0 页；静音掉的话界面会停在
    // 「侧栏选中了第一项、内容区还停在旧页」的不一致状态。
    for (int row = 0; row < sidebar->count(); ++row) {
        if (sidebar->item(row)->data(Qt::UserRole).isValid()) {
            sidebar->setCurrentRow(row);
            break;
        }
    }
}

void ApplySidebarBadges(QListWidget* sidebar, QStackedWidget* stack,
                        const SidebarBadgeLookup& lookup) {
    if (!sidebar || !stack || !lookup) return;

    bool changed = false;
    for (int row = 0; row < sidebar->count(); ++row) {
        QListWidgetItem* item = sidebar->item(row);
        if (!item) continue;
        const QVariant stack_index = item->data(Qt::UserRole);
        if (!stack_index.isValid()) continue;  // 分组标题没有徽标
        QWidget* page = stack->widget(stack_index.toInt());
        if (!page) continue;  // 侧栏项与 stack 不同步（换页/重建的窗口期）时别解引用

        const SidebarBadge badge = lookup(page->property("pageId").toString());
        if (item->data(kBadgeCountRole).toInt() != badge.count ||
            item->data(kBadgeLevelRole).toInt() != badge.level ||
            item->data(kScanningRole).toBool() != badge.scanning) {
            item->setData(kBadgeCountRole, badge.count);
            item->setData(kBadgeLevelRole, badge.level);
            item->setData(kScanningRole, badge.scanning);
            changed = true;
        }
    }
    if (changed) sidebar->update();
}

void SelectSidebarRowForStackIndex(QListWidget* sidebar, int stack_index) {
    if (!sidebar) return;
    for (int row = 0; row < sidebar->count(); ++row) {
        QListWidgetItem* item = sidebar->item(row);
        if (!item || item->data(Qt::UserRole).toInt() != stack_index) continue;
        // 静音：调用方（首页「跳转到分析页」）已经把 stack 切过去了，这里只是让侧栏
        // 的选中态跟上；再触发一次 currentRowChanged 会让同一次跳转走两遍切页路径。
        const QSignalBlocker blocker(sidebar);
        sidebar->setCurrentRow(row);
        return;
    }
}

}  // namespace ui
}  // namespace videoeye
