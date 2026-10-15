#pragma once

// 侧栏的「建条目 / 画徽标」机制（从 MainWindow 搬出）。
//
// 侧栏是主窗口里唯一一处**自己画**的控件：分组标题与页面项在同一列表里，QSS 的
// ::item 只能给整列统一样式，无法按行区分，所以必须由委托按行绘制。这块代码原先
// 住在 MainWindow.cpp 顶部的匿名命名空间里（约 100 行），跟着一起搬过来的还有
// 三条徽标数据角色 —— 它们被两处共享（这里是绘制方，MainWindow 的
// UpdateNavigationBadges 是写入方），分散在两个文件里改一处漏一处就会画不出来。
//
// 纯口径（怎么排、怎么分组）在 navigation_model.h，那里不依赖 QWidget 且有单测；
// 这里只管 Qt 侧的读写，两者靠 NavRow / PageNavigation 对接。

#include <QListWidget>
#include <QMap>
#include <QStackedWidget>
#include <QString>
#include <QVector>
#include <functional>

#include "ui/main_window/navigation_model.h"

namespace videoeye {
namespace ui {

// 侧栏 item 的自定义数据角色。
// Qt::UserRole 存 stack 下标（见 ApplyNavigationRows），这里三个是导航标记三件套：
// 由 ApplySidebarBadges 写入、SidebarItemDelegate 画出。
constexpr int kBadgeCountRole = Qt::UserRole + 1;  // 异常条数（int）
constexpr int kBadgeLevelRole = Qt::UserRole + 2;  // 级别 0/1/2/3（int）
constexpr int kScanningRole = Qt::UserRole + 3;    // 是否共用扫描中（bool）

// 一条侧栏项此刻该显示什么徽标。
struct SidebarBadge {
    int count = 0;        // 异常条数（0 = 不画数字徽标）
    int level = 0;        // 0 无 / 1 警告 / 2 错误 / 3 致命（决定配色）
    bool scanning = false;  // 该页共用的一次全文件扫描正在进行
};

// 给侧栏装上按行绘制的委托（分组标题 vs 页面项 + 徽标）。
// 必须在铺条目之前调用 —— 委托是列表级的，不是逐 item 的。
void InstallSidebarDelegate(QListWidget* sidebar);

// 从页面控件的动态属性里收集导航元数据。
// 页面在登记时把自己的 pageId / 标题 / 分组 / 组内排序 / 口径写进属性，侧栏据此生成，
// 于是增减或重排分析页都不必再动侧栏代码。
QVector<PageNavigation> CollectPageNavigation(QStackedWidget* stack);

// 侧栏分组。id 与页面登记对应、标题本地化；顺序即侧栏从上到下的分组顺序。
QVector<NavGroup> DefaultNavGroups();

// 把排好的行铺进侧栏：分组标题置 NoItemFlags（不可选中、无 UserRole），页面项带
// UserRole = stack 下标。铺完后默认选中第一个页面项。
// 全程 blockSignals —— 逐条 addItem 会反复触发 currentRowChanged，而此时
// content_stack_ 与侧栏正处在半重建状态，按中间态切页会闪。
void ApplyNavigationRows(QListWidget* sidebar, const QVector<NavRow>& rows);

// 按 pageId 查这一页的徽标；查不到返回默认构造（什么都不画）。
using SidebarBadgeLookup = std::function<SidebarBadge(const QString& page_id)>;

// 把所有页面项的徽标刷新一遍：先由 stack 下标反查 pageId，再问 lookup。
// 只在值真的变了时才写回并 update()（这个列表重绘不算便宜，而扫描期每帧都可能调）。
void ApplySidebarBadges(QListWidget* sidebar, QStackedWidget* stack,
                        const SidebarBadgeLookup& lookup);

// 按 stack 下标选中对应侧栏行（首页「跳转到分析页」用）。
// 行号因分组标题会与下标错位，不能当下标用，只能逐行反查 UserRole。
void SelectSidebarRowForStackIndex(QListWidget* sidebar, int stack_index);

}  // namespace ui
}  // namespace videoeye
