// 主窗口两条尺寸/布局口径的回归（ui/main_window/window_layout.h）。
//
// 这两条原先埋在 UpdateMinimumWindowSize / showEvent 里，属于典型的「改错了不崩、
// 只是窗口变得难用」，所以以前没人发现它们错了没人管：
//   * 最小高度算漏一条栏 -> 窗口能被拖到比内容还矮，底部分析页被裁掉，而用户只会
//     以为"这个软件排版就这样"；
//   * 小屏阈值判反 -> 1366x768 的笔记本上一打开就超出可用高度（固定 480px 播放区
//     + 250px 内容区 + 菜单栏），窗口放不下 —— 这条以前真出过。

#include "ui/main_window/window_layout.h"

#include <gtest/gtest.h>

namespace {

using videoeye::ui::InitialLayoutPreset;
using videoeye::ui::IsBelowMinimum;
using videoeye::ui::kBaseMinimumWidth;
using videoeye::ui::kSmallScreenHeightThreshold;
using videoeye::ui::MinimumWindowSize;
using videoeye::ui::WindowLayoutMetrics;
using videoeye::ui::WindowLayoutPreset;

WindowLayoutMetrics Metrics(int bars, int content, int player) {
    WindowLayoutMetrics m;
    m.menu_bar_height = bars;
    m.content_min_height = content;
    m.player_min_height = player;
    return m;
}

} // namespace

TEST(WindowLayoutTests, MinimumHeightIsTheSumOfEveryBarAndBothPanes) {
    // 三条栏 + 内容区 + 播放区，一条都不能漏：漏一条就允许窗口比内容矮。
    WindowLayoutMetrics m;
    m.app_bar_height = 48;
    m.menu_bar_height = 24;
    m.status_bar_height = 22;
    m.content_min_height = 250;
    m.player_min_height = 300;
    EXPECT_EQ(MinimumWindowSize(m).height(), 48 + 24 + 22 + 250 + 300);
}

TEST(WindowLayoutTests, CollapsedPlayerAreaLetsTheWindowShrink) {
    // 播放区收起时 MinimumHeightHint 返回 0，这是刻意的：分析为主的场景下
    // 用户应该能把窗口压到只剩分析区。
    EXPECT_EQ(MinimumWindowSize(Metrics(24, 250, 0)).height(), 274);
    EXPECT_LT(MinimumWindowSize(Metrics(24, 250, 0)).height(), MinimumWindowSize(Metrics(24, 250, 300)).height());
}

TEST(WindowLayoutTests, MinimumWidthAddsSidebarOnTopOfTheBase) {
    // 侧栏宽度必须计入：它是固定宽度，不计就会出现"横向缩到刚好裁掉侧栏"的尺寸。
    WindowLayoutMetrics m = Metrics(24, 250, 300);
    EXPECT_EQ(MinimumWindowSize(m).width(), kBaseMinimumWidth);
    m.sidebar_width = 200;
    EXPECT_EQ(MinimumWindowSize(m).width(), kBaseMinimumWidth + 200);
}

TEST(WindowLayoutTests, EmptyMetricsStillProduceAUsableSize) {
    // 还没有侧栏 / 播放区（构造早期）时不能算出 0 —— setMinimumSize(0,0) 等于没约束。
    const QSize min = MinimumWindowSize(WindowLayoutMetrics{});
    EXPECT_GE(min.width(), kBaseMinimumWidth);
    EXPECT_GE(min.height(), 0);
}

TEST(WindowLayoutTests, SmallScreensStartInTheAnalysisPreset) {
    // 860 是 1366x768 笔记本减掉任务栏后的典型可用高度，阈值以下必须走「分析」。
    EXPECT_EQ(InitialLayoutPreset(kSmallScreenHeightThreshold - 1), WindowLayoutPreset::Analysis);
    EXPECT_EQ(InitialLayoutPreset(700), WindowLayoutPreset::Analysis);
}

TEST(WindowLayoutTests, RoomyScreensStartInTheMonitorPreset) {
    EXPECT_EQ(InitialLayoutPreset(kSmallScreenHeightThreshold), WindowLayoutPreset::Monitor);
    EXPECT_EQ(InitialLayoutPreset(1200), WindowLayoutPreset::Monitor);
}

TEST(WindowLayoutTests, UnknownScreenHeightFallsBackToMonitor) {
    // 拿不到屏幕信息（离屏 / 无屏）时 available_height 是 0。
    // 不能因为 0 < 860 就走「分析」—— 那会把播放区压成一条。
    EXPECT_EQ(InitialLayoutPreset(0), WindowLayoutPreset::Monitor);
    EXPECT_EQ(InitialLayoutPreset(-1), WindowLayoutPreset::Monitor);
}

TEST(WindowLayoutTests, BelowMinimumTriggersOnEitherAxis) {
    const QSize min(1000, 700);
    EXPECT_TRUE(IsBelowMinimum(QSize(999, 700), min));   // 只窄了
    EXPECT_TRUE(IsBelowMinimum(QSize(1000, 699), min));  // 只矮了
    EXPECT_FALSE(IsBelowMinimum(QSize(1000, 700), min)); // 正好卡在最小值上不算
    EXPECT_FALSE(IsBelowMinimum(QSize(1200, 900), min));
}
