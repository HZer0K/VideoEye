// ControlBarWidget（播放控制栏）的接缝测试。
//
// 为什么值得单独测：控制栏是从 PlayerPanel 抽出的独立视图组件，它自己只保留两个
// **纯本地 handler**（音量 / 静音），其余用户意图都靠外露的子控件访问器交给面板接线。
// 这两个 handler 的状态全压在组件内部（last_volume_），一旦改错不会崩，只会让
// 「静音后恢复」恢复成错误的音量，或者静音图标/提示与实际音量分家 —— 属于评审
// P3「组件可单独测试只兑现了一部分」里该补的一块。
//
// 具体盯三件事：
//   1. 控件初值：音量滑块 0..100 且默认 100、进度条初始上下界都是 0、逐帧按钮默认隐藏、
//      MV 开关可勾选。这些是布局契约，改了界面会静默错位。
//   2. 静音/恢复的状态迁移：点一次记住当前音量并归零，再点一次恢复；期间音量、提示
//      文案、滑块值三者必须同步。
//   3. 依赖注入：注入 MediaPlayer 后，音量变化必须真正转发到 SetVolume（未注入时
//      只更新本地提示，绝不能空指针解引用）。
//
// 用 offscreen 平台跑，不需要显示器；只构造控件、不渲染。

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

#include "core/player/MediaPlayer.h"
#include "ui/player/ControlBarWidget.h"

using videoeye::player::MediaPlayer;
using videoeye::ui::ControlBarWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_control_bar_widget";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 控件初值：布局契约（范围 / 默认值 / 隐藏状态 / objectName）
// ---------------------------------------------------------------------------

TEST(ControlBarWidgetTests, InitialControlsMatchLayoutContract) {
    EnsureApp();

    ControlBarWidget bar;

    // 组件自身固定高度，否则收起/展开播放区时会出现高度抖动
    EXPECT_EQ(QStringLiteral("ControlBar"), bar.objectName());
    EXPECT_EQ(56, bar.minimumHeight());
    EXPECT_EQ(56, bar.maximumHeight());

    QSlider* volume = bar.volumeSlider();
    ASSERT_TRUE(volume != nullptr);
    EXPECT_EQ(0, volume->minimum());
    EXPECT_EQ(100, volume->maximum());
    EXPECT_EQ(100, volume->value());
    EXPECT_EQ(QStringLiteral("音量: 100%"), volume->toolTip());

    // 进度条初始上下界都是 0（没有媒体时不可拖动）
    QSlider* seek = bar.seekSlider();
    ASSERT_TRUE(seek != nullptr);
    EXPECT_EQ(0, seek->minimum());
    EXPECT_EQ(0, seek->maximum());

    // 逐帧按钮默认隐藏（仅 raw 序列模式显示），其余主控件必须存在且可用
    ASSERT_TRUE(bar.prevFrameButton() != nullptr);
    ASSERT_TRUE(bar.nextFrameButton() != nullptr);
    EXPECT_TRUE(bar.prevFrameButton()->isHidden());
    EXPECT_TRUE(bar.nextFrameButton()->isHidden());
    ASSERT_TRUE(bar.playPauseButton() != nullptr);
    EXPECT_FALSE(bar.playPauseButton()->isHidden());
    EXPECT_TRUE(bar.playPauseButton()->isEnabled());

    // MV 是开关式按钮，默认关
    ASSERT_TRUE(bar.mvOverlayButton() != nullptr);
    EXPECT_TRUE(bar.mvOverlayButton()->isCheckable());
    EXPECT_FALSE(bar.mvOverlayButton()->isChecked());

    ASSERT_TRUE(bar.timeLabel() != nullptr);
    EXPECT_EQ(QStringLiteral("00:00:00 / 00:00:00"), bar.timeLabel()->text());
    ASSERT_TRUE(bar.timecodeLabel() != nullptr);
    EXPECT_TRUE(bar.timecodeLabel()->text().contains(QStringLiteral("--:--:--:--")));
}

// ---------------------------------------------------------------------------
// 2. 音量提示随滑块值联动
// ---------------------------------------------------------------------------

TEST(ControlBarWidgetTests, VolumeTooltipFollowsSliderValue) {
    EnsureApp();

    ControlBarWidget bar;
    QSlider* volume = bar.volumeSlider();
    ASSERT_TRUE(volume != nullptr);

    volume->setValue(35);
    EXPECT_EQ(35, volume->value());
    EXPECT_EQ(QStringLiteral("音量: 35%"), volume->toolTip());

    volume->setValue(0);
    EXPECT_EQ(QStringLiteral("音量: 0%"), volume->toolTip());
}

// ---------------------------------------------------------------------------
// 3. 静音/恢复的状态迁移：记住音量 -> 归零 -> 恢复
// ---------------------------------------------------------------------------

TEST(ControlBarWidgetTests, MuteRemembersAndRestoresPreviousVolume) {
    EnsureApp();

    ControlBarWidget bar;
    QSlider* volume = bar.volumeSlider();
    ASSERT_TRUE(volume != nullptr);

    volume->setValue(60);
    ASSERT_EQ(60, volume->value());

    // 点一次：静音（记住 60），滑块归零
    bar.volumeButton()->click();
    EXPECT_EQ(0, volume->value());

    // 再点一次：恢复上次音量
    bar.volumeButton()->click();
    EXPECT_EQ(60, volume->value());

    // 换一个音量再走一遍，确认记住的是"最近一次"而不是写死的初值 100
    volume->setValue(25);
    bar.volumeButton()->click();
    EXPECT_EQ(0, volume->value());
    bar.volumeButton()->click();
    EXPECT_EQ(25, volume->value());
}

// ---------------------------------------------------------------------------
// 4. 未注入 MediaPlayer：音量变化只更新本地提示，不崩
// ---------------------------------------------------------------------------

TEST(ControlBarWidgetTests, VolumeChangeWithoutPlayerIsSafe) {
    EnsureApp();

    ControlBarWidget bar;  // 不调用 SetMediaPlayer，player_ 为 nullptr
    QSlider* volume = bar.volumeSlider();
    ASSERT_TRUE(volume != nullptr);

    volume->setValue(10);
    EXPECT_EQ(10, volume->value());
    EXPECT_EQ(QStringLiteral("音量: 10%"), volume->toolTip());
}

// ---------------------------------------------------------------------------
// 5. 注入 MediaPlayer：音量变化转发到 SetVolume（含 0 静音）
// ---------------------------------------------------------------------------

TEST(ControlBarWidgetTests, VolumeChangeIsForwardedToInjectedPlayer) {
    EnsureApp();

    MediaPlayer player;
    ControlBarWidget bar;
    bar.SetMediaPlayer(&player);

    QSlider* volume = bar.volumeSlider();
    ASSERT_TRUE(volume != nullptr);

    volume->setValue(42);
    EXPECT_EQ(42, player.GetVolume());

    // 静音路径同样要走 SetVolume（把播放器也压到 0），否则界面静音而声音还在
    bar.volumeButton()->click();
    EXPECT_EQ(0, volume->value());
    EXPECT_EQ(0, player.GetVolume());

    bar.volumeButton()->click();
    EXPECT_EQ(42, player.GetVolume());
}