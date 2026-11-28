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
// 但"弹窗之后"的整条链路（帧尺寸校验 → 常驻句柄 → 顺序/随机读 → 像素转换）走
// OpenRawSequence 这个不弹窗的入口，见文件末尾第 7 节。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>

#include <vector>

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

} // namespace

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

    RawImageSequence sequence; // 不调用 SetControlBar
    sequence.UpdateRawNavigationState();
    sequence.SetRawImageMode(true);
    sequence.UpdateRawNavigationState();
    sequence.Reset();
    sequence.UpdateRawNavigationState();
    EXPECT_FALSE(sequence.IsShowingRawImage());
}

// ---------------------------------------------------------------------------
// 6. 连续播放的前置条件：没有加载序列时拒绝启动
//
// 启动播放依赖"路径 + 帧长 + 总帧数"三样都在，缺一样都必须返回 false ——
// 否则定时器会在没有数据的情况下一帧帧空转、或对空路径读盘。
// ---------------------------------------------------------------------------

TEST(RawImageSequenceTests, PlaybackRequiresLoadedSequence) {
    EnsureApp();

    RawImageSequence sequence;
    EXPECT_FALSE(sequence.IsRawPlaying());
    // 未进入 Raw 模式
    EXPECT_FALSE(sequence.StartRawPlayback());

    // 进入 Raw 模式但没加载文件（路径/帧长/总帧数都为空）
    sequence.SetRawImageMode(true);
    EXPECT_FALSE(sequence.StartRawPlayback());
    EXPECT_FALSE(sequence.IsRawPlaying());

    // 未在播放时 Stop 是空操作
    sequence.StopRawPlayback();
    EXPECT_FALSE(sequence.IsRawPlaying());

    sequence.Reset();
    EXPECT_FALSE(sequence.IsShowingRawImage());
    EXPECT_FALSE(sequence.IsRawPlaying());
}
// ---------------------------------------------------------------------------
// 7. 加载与逐帧读盘（绕过对话框的 OpenRawSequence 入口）
//
// 这一节覆盖的是"读盘 + 转像素"的真链路：
//   * 常驻文件句柄：连续读时不该重新 open/close（靠顺序读与随机读结果一致来判）；
//   * 顺序读快路径：跳过 seek 之后读到的必须仍是"下一帧"，不是上一帧；
//   * 帧号越界 / 序列复位后重开：既不能读到脏数据，也不能把句柄留在失效位置上。
// ---------------------------------------------------------------------------

namespace {

// 一帧 I420：Y 取 luma，色度中性（128）—— 结果是灰度，便于按亮度判帧号。
QByteArray BuildI420Frame(int width, int height, int luma) {
    const int y_size = width * height;
    QByteArray frame(y_size * 3 / 2, '\0');
    for (int i = 0; i < y_size; ++i) {
        frame[i] = static_cast<char>(luma);
    }
    for (int i = y_size; i < frame.size(); ++i) {
        frame[i] = static_cast<char>(128);
    }
    return frame;
}

// 三帧 I420 序列（8x8）：亮度 16 / 126 / 235，对应 limited range 的黑 / 中灰 / 白。
QString WriteThreeFrameSequence(const QTemporaryDir& dir, const std::vector<int>& luma) {
    const QString path = dir.filePath(QStringLiteral("seq.yuv"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    for (const int value : luma) {
        file.write(BuildI420Frame(8, 8, value));
    }
    file.close();
    return path;
}

// 收集最后一次上屏的帧（RawFrameShown 是组件的对外出口）。
struct FrameSink {
    QImage last;
    int count = 0;

    void Attach(RawImageSequence& sequence) {
        QObject::connect(&sequence, &RawImageSequence::RawFrameShown, [this](const QImage& frame, const QString&) {
            last = frame;
            ++count;
        });
    }

    int GrayAt(int x, int y) const {
        return last.isNull() ? -1 : qGray(last.pixel(x, y));
    }
};

} // namespace

TEST(RawImageSequenceTests, OpenRawSequenceRejectsBadInput) {
    EnsureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = WriteThreeFrameSequence(dir, {16, 126, 235});
    ASSERT_FALSE(path.isEmpty());

    RawImageSequence sequence;
    FrameSink sink;
    sink.Attach(sequence);

    // 标签必须是格式表里的名字（不做模糊匹配）。
    EXPECT_FALSE(sequence.OpenRawSequence(path, 8, 8, QStringLiteral("I420"), 25));
    // 4:2:0 的奇数宽高装不下色度平面。
    EXPECT_FALSE(sequence.OpenRawSequence(path, 7, 8, QStringLiteral("YUV420P(I420)"), 25));
    // 文件里连一帧都不够。
    EXPECT_FALSE(sequence.OpenRawSequence(path, 64, 64, QStringLiteral("YUV420P(I420)"), 25));
    // 路径不存在。
    EXPECT_FALSE(sequence.OpenRawSequence(dir.filePath(QStringLiteral("missing.yuv")), 8, 8,
                                          QStringLiteral("YUV420P(I420)"), 25));

    // 全部失败：不能进 Raw 模式、不能上屏、也不能留下半个序列状态。
    EXPECT_FALSE(sequence.IsShowingRawImage());
    EXPECT_EQ(sink.count, 0);
    EXPECT_FALSE(sequence.ShowRawFrame(0));
}

TEST(RawImageSequenceTests, OpenRawSequenceShowsFirstFrameAndStepsInOrder) {
    EnsureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = WriteThreeFrameSequence(dir, {16, 126, 235});
    ASSERT_FALSE(path.isEmpty());

    RawImageSequence sequence;
    FrameSink sink;
    sink.Attach(sequence);

    ASSERT_TRUE(sequence.OpenRawSequence(path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));
    EXPECT_TRUE(sequence.IsShowingRawImage());
    ASSERT_EQ(sink.count, 1);
    EXPECT_EQ(sink.last.width(), 8);
    EXPECT_EQ(sink.last.height(), 8);
    // 打开即显示第 0 帧：limited range 的 16 应压到黑。
    EXPECT_LE(sink.GrayAt(4, 4), 4);

    // 顺序推进：每帧的亮度必须跟着变（若"跳过 seek"的游标算错，会反复读到同一帧）。
    ASSERT_TRUE(sequence.ShowRawFrame(1));
    const int middle = sink.GrayAt(4, 4);
    EXPECT_GE(middle, 118);
    EXPECT_LE(middle, 138);

    ASSERT_TRUE(sequence.ShowRawFrame(2));
    EXPECT_GE(sink.GrayAt(4, 4), 250);

    // 越界帧号一律拒绝，且不改变当前画面。
    EXPECT_FALSE(sequence.ShowRawFrame(3));
    EXPECT_FALSE(sequence.ShowRawFrame(-1));
    EXPECT_GE(sink.GrayAt(4, 4), 250);
}

TEST(RawImageSequenceTests, RandomAccessReadsTheRequestedFrame) {
    EnsureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = WriteThreeFrameSequence(dir, {16, 126, 235});
    ASSERT_FALSE(path.isEmpty());

    RawImageSequence sequence;
    FrameSink sink;
    sink.Attach(sequence);

    ASSERT_TRUE(sequence.OpenRawSequence(path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));

    // 往前跳（跳过 seek 的快路径不适用），再往回跳（必须重新 seek）。
    ASSERT_TRUE(sequence.ShowRawFrame(2));
    EXPECT_GE(sink.GrayAt(0, 0), 250);
    ASSERT_TRUE(sequence.ShowRawFrame(1));
    const int middle = sink.GrayAt(0, 0);
    EXPECT_GE(middle, 118);
    EXPECT_LE(middle, 138);
    ASSERT_TRUE(sequence.ShowRawFrame(0));
    EXPECT_LE(sink.GrayAt(0, 0), 4);

    // 顺序读与随机读必须得到同一帧：先顺序走一遍，再单独打开直接跳到第 1 帧对照。
    ASSERT_TRUE(sequence.ShowRawFrame(1));
    const int sequential = sink.GrayAt(2, 2);

    RawImageSequence fresh;
    FrameSink fresh_sink;
    fresh_sink.Attach(fresh);
    ASSERT_TRUE(fresh.OpenRawSequence(path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));
    ASSERT_TRUE(fresh.ShowRawFrame(1));
    EXPECT_EQ(sequential, fresh_sink.GrayAt(2, 2));
}

TEST(RawImageSequenceTests, ResetClosesSequenceAndAllowsReopen) {
    EnsureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = WriteThreeFrameSequence(dir, {16, 126, 235});
    ASSERT_FALSE(path.isEmpty());

    RawImageSequence sequence;
    FrameSink sink;
    sink.Attach(sequence);

    ASSERT_TRUE(sequence.OpenRawSequence(path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));
    EXPECT_TRUE(sequence.IsShowingRawImage());

    sequence.Reset();
    EXPECT_FALSE(sequence.IsShowingRawImage());
    // 复位后一切读操作都必须被挡住（句柄已关，帧长/总帧数已清零）。
    EXPECT_FALSE(sequence.ShowRawFrame(0));
    EXPECT_FALSE(sequence.StartRawPlayback());

    // 复位只清状态，不破坏"还能再打开同一份文件"这件事（句柄要能被重新建立）。
    ASSERT_TRUE(sequence.OpenRawSequence(path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));
    EXPECT_LE(sink.GrayAt(4, 4), 4);
}

TEST(RawImageSequenceTests, ResetReleasesTheFileHandle) {
    EnsureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = WriteThreeFrameSequence(dir, {16, 126, 235});
    ASSERT_FALSE(path.isEmpty());

    RawImageSequence sequence;
    ASSERT_TRUE(sequence.OpenRawSequence(path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));

    sequence.Reset();
    // 常驻句柄的意义是"序列活着的时候不反复 open/close"，但复位/换源之后必须放开：
    // Windows 上句柄不关就删不掉也改不了名（共享冲突），用户会看到"文件被占用"。
    // Linux 上 unlink 本来就允许带打开句柄，这条只在 Windows 有判别力。
    EXPECT_TRUE(QFile::remove(path)) << "Reset 之后文件仍被占用（常驻句柄没关）";
}

TEST(RawImageSequenceTests, ReopenSwitchesToTheNewFile) {
    EnsureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString black_path = WriteThreeFrameSequence(dir, {16, 126, 235});
    ASSERT_FALSE(black_path.isEmpty());

    // 另一份：单帧纯白（16 帧长 → 只要一帧的字节）。
    QFile white_file(dir.filePath(QStringLiteral("white.yuv")));
    ASSERT_TRUE(white_file.open(QIODevice::WriteOnly));
    white_file.write(BuildI420Frame(8, 8, 235));
    white_file.close();

    RawImageSequence sequence;
    FrameSink sink;
    sink.Attach(sequence);

    ASSERT_TRUE(sequence.OpenRawSequence(black_path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));
    EXPECT_LE(sink.GrayAt(4, 4), 4);

    // 换源：旧句柄必须让位，读到的必须是新文件的第 0 帧（不是旧文件残留在缓存里的帧）。
    ASSERT_TRUE(sequence.OpenRawSequence(white_file.fileName(), 8, 8, QStringLiteral("YUV420P(I420)"), 25));
    EXPECT_GE(sink.GrayAt(4, 4), 250);
}

TEST(RawImageSequenceTests, PlaybackAdvancesToLastFrameAndStops) {
    EnsureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = WriteThreeFrameSequence(dir, {16, 126, 235});
    ASSERT_FALSE(path.isEmpty());

    RawImageSequence sequence;
    FrameSink sink;
    sink.Attach(sequence);

    ASSERT_TRUE(sequence.OpenRawSequence(path, 8, 8, QStringLiteral("YUV420P(I420)"), 25));
    // 直接驱动"下一个 / 上一个"（定时器只是它的节拍器，这里省掉等待时间）。
    sequence.OnNextRawFrame();
    sequence.OnNextRawFrame();
    EXPECT_GE(sink.GrayAt(4, 4), 250);
    // 末帧之后不再前进（不循环、不越界读）。
    sequence.OnNextRawFrame();
    EXPECT_GE(sink.GrayAt(4, 4), 250);
    sequence.OnPrevRawFrame();
    const int middle = sink.GrayAt(4, 4);
    EXPECT_GE(middle, 118);
    EXPECT_LE(middle, 138);
}
