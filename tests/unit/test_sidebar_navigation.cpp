// 侧栏导航的 Qt 侧机制回归（ui/main_window/sidebar_navigation.{h,cpp}）。
//
// 这块原先住在 MainWindow.cpp 顶部的匿名命名空间里，跟着主窗口一起没人测得到。
// 它守的是三条「错了就是静默错位」的不变量：
//   1. 页面项带 UserRole = stack 下标，分组标题不带 —— 这是切换与徽标唯一的寻址
//      依据（行号因标题占位必然与下标错位，历史上「码率与 GOP」显示成「诊断与
//      报告」就是这么来的）；
//   2. 分组标题必须不可选中（NoItemFlags），否则点一下分组名会切到错误页面；
//   3. 铺条目全程静音 —— 逐条 addItem 触发的 currentRowChanged 会在「侧栏已铺、
//      页面还没同步」的半重建窗口里切页。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QListWidget>
#include <QListWidgetItem>
#include <QStackedWidget>
#include <QWidget>

#include "ui/main_window/sidebar_navigation.h"

namespace {

using videoeye::ui::ApplyNavigationRows;
using videoeye::ui::ApplySidebarBadges;
using videoeye::ui::CollectPageNavigation;
using videoeye::ui::DefaultNavGroups;
using videoeye::ui::InstallSidebarDelegate;
using videoeye::ui::kBadgeCountRole;
using videoeye::ui::kBadgeLevelRole;
using videoeye::ui::kScanningRole;
using videoeye::ui::NavRow;
using videoeye::ui::SelectSidebarRowForStackIndex;
using videoeye::ui::SidebarBadge;

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_sidebar_navigation";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 造一个登记了导航属性的页面（与真实页面登记方式一致：setProperty）
QWidget* MakePage(QStackedWidget* stack, const QString& page_id, const QString& title, const QString& group, int order,
                  const QString& scope = QString()) {
    QWidget* page = new QWidget(stack);
    page->setProperty("pageId", page_id);
    page->setProperty("pageTitle", title);
    page->setProperty("pageGroup", group);
    page->setProperty("pageOrder", order);
    page->setProperty("pageScope", scope);
    return page;
}

bool HasStackIndex(QListWidget* sidebar, int row) {
    return sidebar->item(row)->data(Qt::UserRole).isValid();
}

} // namespace

TEST(SidebarNavigationTests, CollectPageNavigationReadsWidgetProperties) {
    EnsureApp();
    QStackedWidget stack;
    stack.addWidget(
        MakePage(&stack, QStringLiteral("overview"), QStringLiteral("总览"), QStringLiteral("overview"), 0));
    stack.addWidget(MakePage(&stack, QStringLiteral("bitrate"), QStringLiteral("码率与 GOP"),
                             QStringLiteral("full_quality"), 2, QStringLiteral("全文件")));

    const auto pages = CollectPageNavigation(&stack);
    ASSERT_EQ(pages.size(), 2);
    EXPECT_EQ(pages[0].page_id, QStringLiteral("overview"));
    EXPECT_EQ(pages[0].stack_index, 0);
    EXPECT_EQ(pages[1].page_id, QStringLiteral("bitrate"));
    EXPECT_EQ(pages[1].stack_index, 1);
    EXPECT_EQ(pages[1].scope, QStringLiteral("全文件"));
}

TEST(SidebarNavigationTests, UnregisteredPageStillGetsAPlaceholderTitle) {
    // 新增页忘了写 pageTitle 也要能在侧栏里点得到 —— 不然就是一个没有名字、
    // 也没有下标的空行，谁都不知道那里有一页。
    EnsureApp();
    QStackedWidget stack;
    stack.addWidget(new QWidget(&stack));
    const auto pages = CollectPageNavigation(&stack);
    ASSERT_EQ(pages.size(), 1);
    EXPECT_FALSE(pages[0].title.isEmpty());
    EXPECT_EQ(pages[0].stack_index, 0);
}

TEST(SidebarNavigationTests, ApplyNavigationRowsMarksHeadersUnselectable) {
    EnsureApp();
    QListWidget sidebar;
    InstallSidebarDelegate(&sidebar);

    NavRow header;
    header.text = QStringLiteral("全片质量");
    header.is_header = true;
    NavRow page;
    page.text = QStringLiteral("码率与 GOP");
    page.stack_index = 3;

    ApplyNavigationRows(&sidebar, {header, page});
    ASSERT_EQ(sidebar.count(), 2);
    // 分组标题：不可选中、且**没有** UserRole（OnSidebarChanged 据此直接返回）
    EXPECT_EQ(sidebar.item(0)->flags(), Qt::NoItemFlags);
    EXPECT_FALSE(HasStackIndex(&sidebar, 0));
    // 页面项：可选中、带 stack 下标
    EXPECT_TRUE(sidebar.item(1)->flags() & Qt::ItemIsSelectable);
    EXPECT_EQ(sidebar.item(1)->data(Qt::UserRole).toInt(), 3);
}

TEST(SidebarNavigationTests, ApplyNavigationRowsSelectsTheFirstPageItem) {
    // 铺完之后必须落在第一个**页面项**上（分组标题没有 UserRole，落在它上面不会切页）
    EnsureApp();
    QListWidget sidebar;
    InstallSidebarDelegate(&sidebar);

    NavRow header;
    header.text = QStringLiteral("概览");
    header.is_header = true;
    NavRow first;
    first.stack_index = 0;
    NavRow second;
    second.stack_index = 1;

    ApplyNavigationRows(&sidebar, {header, first, second});
    EXPECT_EQ(sidebar.currentRow(), 1); // 跳过标题行
}

TEST(SidebarNavigationTests, ApplyNavigationRowsEmitsOnlyForTheFinalSelection) {
    // 逐条 addItem 会反复触发 currentRowChanged；此刻侧栏与页面栈正处在半重建状态，
    // 按中间态切页会闪 —— 所以铺的过程必须静音。
    //
    // 但**末尾那一次默认选中必须发**：调用方（MainWindow）正是靠 currentRowChanged
    // 才把 content_stack_ 同步到第 0 页。静音掉会出现「侧栏选中第一项、内容区还停在
    // 旧页」的不一致。所以期望是恰好 1 次，不是 0 次。
    EnsureApp();
    QListWidget sidebar;
    InstallSidebarDelegate(&sidebar);
    ASSERT_TRUE(sidebar.itemDelegate() != nullptr);

    int emitted = 0;
    QVector<int> rows;
    QObject::connect(&sidebar, &QListWidget::currentRowChanged, [&emitted, &rows](int row) {
        ++emitted;
        rows.append(row);
    });
    NavRow row;
    row.stack_index = 0;
    ApplyNavigationRows(&sidebar, {row, row});
    EXPECT_EQ(emitted, 1);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.front(), 0); // 落在第一个页面项上
}

TEST(SidebarNavigationTests, ApplySidebarBadgesWritesAllThreeRoles) {
    EnsureApp();
    QStackedWidget stack;
    stack.addWidget(
        MakePage(&stack, QStringLiteral("visual"), QStringLiteral("画面质检"), QStringLiteral("full_quality"), 0));
    QListWidget sidebar;
    NavRow row;
    row.stack_index = 0;
    ApplyNavigationRows(&sidebar, {row});

    SidebarBadge badge;
    badge.count = 7;
    badge.level = 3;
    badge.scanning = true;
    ApplySidebarBadges(&sidebar, &stack, [&badge](const QString&) { return badge; });

    EXPECT_EQ(sidebar.item(0)->data(kBadgeCountRole).toInt(), 7);
    EXPECT_EQ(sidebar.item(0)->data(kBadgeLevelRole).toInt(), 3);
    EXPECT_TRUE(sidebar.item(0)->data(kScanningRole).toBool());
}

TEST(SidebarNavigationTests, ApplySidebarBadgesSkipsHeaderRows) {
    // 分组标题没有 UserRole，也不该去问 lookup（它的 pageId 无从谈起）
    EnsureApp();
    QStackedWidget stack;
    stack.addWidget(
        MakePage(&stack, QStringLiteral("visual"), QStringLiteral("画面质检"), QStringLiteral("full_quality"), 0));
    QListWidget sidebar;
    NavRow header;
    header.text = QStringLiteral("全片质量");
    header.is_header = true;
    ApplyNavigationRows(&sidebar, {header});

    int asked = 0;
    ApplySidebarBadges(&sidebar, &stack, [&asked](const QString&) {
        ++asked;
        return SidebarBadge{};
    });
    EXPECT_EQ(asked, 0);
    EXPECT_FALSE(sidebar.item(0)->data(kBadgeCountRole).isValid());
}

TEST(SidebarNavigationTests, ApplySidebarBadgesClearsBadgeWhenPageHasNone) {
    // 扫描结束后 count 要回到 0 —— 写了就画，不写就留着上一次的数字。
    EnsureApp();
    QStackedWidget stack;
    stack.addWidget(
        MakePage(&stack, QStringLiteral("visual"), QStringLiteral("画面质检"), QStringLiteral("full_quality"), 0));
    QListWidget sidebar;
    NavRow row;
    row.stack_index = 0;
    ApplyNavigationRows(&sidebar, {row});

    SidebarBadge badge;
    badge.count = 9;
    ApplySidebarBadges(&sidebar, &stack, [&badge](const QString&) { return badge; });
    ASSERT_EQ(sidebar.item(0)->data(kBadgeCountRole).toInt(), 9);

    badge = SidebarBadge{};
    ApplySidebarBadges(&sidebar, &stack, [&badge](const QString&) { return badge; });
    EXPECT_EQ(sidebar.item(0)->data(kBadgeCountRole).toInt(), 0);
    EXPECT_FALSE(sidebar.item(0)->data(kScanningRole).toBool());
}

TEST(SidebarNavigationTests, ApplySidebarBadgesSurvivesDesynchronizedStack) {
    // 侧栏项与 stack 不同步（换页 / 重建的窗口期）时不能解引用空页指针
    EnsureApp();
    QStackedWidget stack; // 空栈
    QListWidget sidebar;
    NavRow row;
    row.stack_index = 5; // 越界
    ApplyNavigationRows(&sidebar, {row});
    ApplySidebarBadges(&sidebar, &stack, [](const QString&) { return SidebarBadge{}; });
    EXPECT_FALSE(sidebar.item(0)->data(kBadgeCountRole).isValid());
}

TEST(SidebarNavigationTests, SelectSidebarRowForStackIndexSkipsHeaderRows) {
    EnsureApp();
    QListWidget sidebar;
    NavRow header;
    header.text = QStringLiteral("报告与工具");
    header.is_header = true;
    NavRow a;
    a.stack_index = 0;
    NavRow b;
    b.stack_index = 1;
    ApplyNavigationRows(&sidebar, {header, a, b});

    SelectSidebarRowForStackIndex(&sidebar, 1);
    EXPECT_EQ(sidebar.currentRow(), 2); // 行号 != 下标，只能反查
}

TEST(SidebarNavigationTests, SelectSidebarRowForStackIndexDoesNotRetriggerNavigation) {
    // 调用方（首页跳转）已经把 stack 切过去了，这里只是让选中态跟上；
    // 再触发一次 currentRowChanged 会让同一次跳转走两遍切页路径。
    EnsureApp();
    QListWidget sidebar;
    NavRow header;
    header.text = QStringLiteral("概览");
    header.is_header = true;
    NavRow a;
    a.stack_index = 0;
    NavRow b;
    b.stack_index = 1;
    ApplyNavigationRows(&sidebar, {header, a, b});

    int emitted = 0;
    QObject::connect(&sidebar, &QListWidget::currentRowChanged, [&emitted](int) { ++emitted; });
    SelectSidebarRowForStackIndex(&sidebar, 1);
    EXPECT_EQ(sidebar.currentRow(), 2);
    EXPECT_EQ(emitted, 0);
}

TEST(SidebarNavigationTests, DefaultNavGroupsCoverEveryRegisteredGroupId) {
    // 分组 id 与页面登记对应；这里只保证概览 / 播放 / 解析 / 全片 / 工具五类都在，
    // 少一个的话那一组页面会被排到侧栏最末且没有标题。
    const auto groups = DefaultNavGroups();
    ASSERT_EQ(groups.size(), 5);
    QVector<QString> ids;
    for (const auto& g : groups)
        ids.append(g.id);
    EXPECT_TRUE(ids.contains(QStringLiteral("overview")));
    EXPECT_TRUE(ids.contains(QStringLiteral("playback")));
    EXPECT_TRUE(ids.contains(QStringLiteral("file_parse")));
    EXPECT_TRUE(ids.contains(QStringLiteral("full_quality")));
    EXPECT_TRUE(ids.contains(QStringLiteral("report_tools")));
}
