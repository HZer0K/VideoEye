// RawImageSequence（裸图像序列的加载与逐帧浏览）的接缝测试。
//
// 为什么值得单独测：它从 PlayerPanel 抽出了整块 Raw 序列逻辑 —— 9 个成员状态 +
// 「给定路径 -> 逐帧显示」+ 回写 ControlBarWidget 的导航状态。加载路径要弹参数对话框，
// 不适合进单测；但**导航状态回写**和**分帧越界防护**是纯逻辑、又最容易写错：一旦
// 守卫条件反了，就会在非 Raw 模式下禁用播放按钮，或者对空序列发出越界读。
//
// 具体盯三件事：
//   1. 扩展名守卫：LoadRawImageFile 对非裸数据后缀必须立刻返回 false（在弹任何对话框
//      之前），否则打开普通视频会弹出参数对话框。
//   2. ShowRawFrame 的越界/未加载防护：未进入 Raw 模式、路径为空、负帧号、越界帧号
//      都必须返回 false，且不触碰渲染目标。
//   3. 导航状态回写：非 Raw 模式下调 UpdateRawNavigationState 必须把播放/音量/进度条
//      恢复成可用，时间标签复位为 00:00:00。这是"退出 Raw 模式后界面还卡在逐帧状态"
//      这类回归的唯一落点。
//
// 裸流参数对话框（QInputDialog）路径**不覆盖**：它必须有人工输入宽度/高度/像素格式，
// 硬闯只会得到一个永远等不到输入的模态框。见测试报告里的覆盖盲点说明。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSlider>

#include "ui/player/ControlBarWidget.h"
#include "ui/player/RawImageSequence.h"

using videoeye::ui::ControlBarWidget;
using videoeye::ui::RawImageSequence;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_raw_image_sequence";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 扩展名守卫：非裸数据后缀在弹对话框之前就被拒
// ---------------------------------------------------------------------------

TEST(RawImageSequenceTests, LoadRejectsUnsupportedSuffixBeforeAnyDialog) {
    EnsureApp();

    RawImageSequence sequence;

    EXPECT_FALSE(sequence.LoadRawImageFile(QStringLiteral("clip.mp4")));
    EXPECT_FALSE(sequence.LoadRawImageFile(QStringLiteral("movie.avi")));
    EXPECT_FALSE(sequence.LoadRawImageFile(QStringLiteral("noext")));
    EXPECT_FALSE(sequence.LoadRawImageFile(QString()));
    EXPECT_FALSE(sequence.IsShowingRawImage());
}

// ---------------------------------------------------------------------------
// 2. ShowRawFrame 的未加载 / 越界防护
// ---------------------------------------------------------------------------

TEST(RawImageSequenceTests, ShowRawFrameIsGuardedWithoutLoadedSequence) {
    EnsureApp();

    RawImageSequence sequence;

    // 未进入 Raw 模式：任何帧号都拒绝
    EXPECT_FALSE(sequence.ShowRawFrame(0));

    // 进入 Raw 模式但路径为空（等价于加载失败后的残留状态）：仍拒绝
    sequence.SetRawImageMode(true);
    EXPECT_TRUE(sequence.IsShowingRawImage());
    EXPECT_FALSE(sequence.ShowRawFrame(0));
    EXPECT_FALSE(sequence.ShowRawFrame(-1));
    EXPECT_FALSE(sequence.ShowRawFrame(999));

    // 逐帧导航在空序列上必须是 no-op，不能崩
    sequence.OnPrevRawFrame();
    sequence.OnNextRawFrame();
    EXPECT_EQ(0, 0);
}

// ---------------------------------------------------------------------------
// 3. 非 Raw 模式下回写导航控件 -> 播放控制恢复可用
// ---------------------------------------------------------------------------

TEST(RawImageSequenceTests, NonRawNavigationStateRestoresPlaybackControls) {
    EnsureApp();

    ControlBarWidget bar;
    RawImageSequence sequence;
    sequence.SetControlBar(&bar);

    // 预置成"停在 Raw 模式"的样子：播放/音量/进度条全被禁用
    bar.playPauseButton()->setEnabled(false);
    bar.volumeButton()->setEnabled(false);
    bar.volumeSlider()->setEnabled(false);
    bar.seekSlider()->setRange(0, 9);
    bar.seekSlider()->setEnabled(false);
    bar.timeLabel()->setText(QStringLiteral("帧 1 / 10"));
    ASSERT_FALSE(bar.playPauseButton()->isEnabled());

    // showing_raw_image_ 仍为 false，走复位分支
    sequence.UpdateRawNavigationState();

    EXPECT_TRUE(bar.playPauseButton()->isEnabled());
    EXPECT_TRUE(bar.volumeButton()->isEnabled());
    EXPECT_TRUE(bar.volumeSlider()->isEnabled());
    EXPECT_TRUE(bar.seekSlider()->isEnabled());
    EXPECT_EQ(QStringLiteral("00:00:00 / 00:00:00"), bar.timeLabel()->text());
    // 逐帧按钮在非 Raw 模式保持隐藏
    EXPECT_TRUE(bar.prevFrameButton()->isHidden());
    EXPECT_TRUE(bar.nextFrameButton()->isHidden());
}

// ---------------------------------------------------------------------------
// 4. SetRawImageMode / Reset 对"是否显示 Raw 图"的迁移
// ---------------------------------------------------------------------------

TEST(RawImageSequenceTests, SetModeAndResetToggleShowingFlag) {
    EnsureApp();

    RawImageSequence sequence;
    EXPECT_FALSE(sequence.IsShowingRawImage());

    sequence.SetRawImageMode(true);
    EXPECT_TRUE(sequence.IsShowingRawImage());

    sequence.Reset();
    EXPECT_FALSE(sequence.IsShowingRawImage());

    // Reset 停在"已复位"状态：Reset 后即便再开模式，空路径仍拒绝显示
    sequence.SetRawImageMode(true);
    EXPECT_FALSE(sequence.ShowRawFrame(0));
}

// ---------------------------------------------------------------------------
// 5. 未注入 ControlBar 时回写导航状态不崩
// ---------------------------------------------------------------------------

TEST(RawImageSequenceTests, UpdateNavigationStateWithoutControlBarIsSafe) {
    EnsureApp();

    RawImageSequence sequence;  // 不调用 SetControlBar
    sequence.UpdateRawNavigationState();
    sequence.SetRawImageMode(true);
    sequence.UpdateRawNavigationState();
    sequence.Reset();
    sequence.UpdateRawNavigationState();
    EXPECT_FALSE(sequence.IsShowingRawImage());
}