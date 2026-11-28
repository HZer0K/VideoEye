// 图片序列预览（一个目录的成品图片按帧率播放）的单元测试。
//
// 这条路径**不**经过 FFmpeg：一帧一次 QImageReader + 一次方向变换。因此它最容易
// 出错的不是解码而是"边界与账目"：
//   1. 什么时候该拒绝打开（目录里 0 张 / 只有 1 张 / Qt 根本解不了这个格式）；
//   2. 帧号边界（越界必须返回 false，且不能动当前画面）；
//   3. 控制栏契约（进度条范围 = 图片数-1、时间标签写"帧 n / N"、音量禁用）；
//   4. 停在末帧再按播放应从第一张重新开始（否则"点了没反应"）。
//
// 定时推进本身不在这里等真实时间 —— 那种断言在 CI 上必然是 flaky；Start/Stop 的
// 状态与信号顺序已经足够覆盖"播放开关"这条契约（见 test_animated_image_loop 里
// 对真实帧推进的覆盖方式）。
//
// 用 offscreen 平台跑，不需要显示器；图片用 QImage::save 现场生成，不依赖样本。

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStringList>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "core/domain/io/Utf8Path.h"
#include "ui/main_window/VideoWidget.h"
#include "ui/player/ControlBarWidget.h"
#include "ui/player/ImageSequencePlayer.h"

namespace fs = std::filesystem;

using videoeye::ToNativePath;
using videoeye::ToUtf8String;
using videoeye::ui::ControlBarWidget;
using videoeye::ui::ImageSequencePlayer;
using videoeye::ui::VideoWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_image_sequence_player";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

fs::path FreshFolder(const std::string& name) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("videoeye_image_seq_" + name);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

QString Q(const fs::path& path) {
    return QString::fromStdString(ToUtf8String(path));
}

// 写一张纯色 PNG（Qt 内置 png 读写，不依赖图片插件）。
void WritePng(const fs::path& path, int width, int height, QColor color) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    ASSERT_TRUE(image.save(Q(path), "PNG"));
}

void WriteGarbage(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "this is not an image";
}

// 被测组件 + 它的渲染目标与控制栏（三者生命周期绑在一个测试里）。
struct Fixture {
    VideoWidget video;
    ControlBarWidget bar;
    ImageSequencePlayer sequence;

    Fixture() {
        video.resize(320, 240);
        sequence.SetVideoWidget(&video);
        sequence.SetControlBar(&bar);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 1. 拒绝打开的条件
// ---------------------------------------------------------------------------
TEST(ImageSequencePlayerTests, RejectsEmptyFolder) {
    EnsureApp();
    const fs::path dir = FreshFolder("empty");
    Fixture f;

    QString message;
    QObject::connect(&f.sequence, &ImageSequencePlayer::StatusMessage,
                     [&message](const QString& text, int) { message = text; });

    EXPECT_FALSE(f.sequence.Load(Q(dir), 25));
    EXPECT_FALSE(f.sequence.IsShowing());
    EXPECT_EQ(f.sequence.FrameCount(), 0);
    EXPECT_FALSE(message.isEmpty()); // 必须说清为什么没打开
}

TEST(ImageSequencePlayerTests, RejectsSingleImageFolder) {
    EnsureApp();
    const fs::path dir = FreshFolder("single");
    WritePng(dir / "only.png", 8, 8, Qt::red);
    Fixture f;

    EXPECT_FALSE(f.sequence.Load(Q(dir), 25));
    EXPECT_FALSE(f.sequence.IsShowing());
}

TEST(ImageSequencePlayerTests, RejectsUndecodableImages) {
    EnsureApp();
    const fs::path dir = FreshFolder("garbage");
    WriteGarbage(dir / "a.png");
    WriteGarbage(dir / "b.png");
    Fixture f;

    QString message;
    QObject::connect(&f.sequence, &ImageSequencePlayer::StatusMessage,
                     [&message](const QString& text, int) { message = text; });

    // 目录里有 2 个"图片"（按扩展名），但都解不出来：不能在"打开成功"之后给黑屏。
    EXPECT_FALSE(f.sequence.Load(Q(dir), 25));
    EXPECT_FALSE(f.sequence.IsShowing());
    EXPECT_FALSE(message.isEmpty());
}

// ---------------------------------------------------------------------------
// 2. 加载成功后的账目与控制栏契约
// ---------------------------------------------------------------------------
TEST(ImageSequencePlayerTests, LoadFillsNavigationState) {
    EnsureApp();
    const fs::path dir = FreshFolder("three");
    WritePng(dir / "f_1.png", 16, 8, Qt::red);
    WritePng(dir / "f_2.png", 16, 8, Qt::green);
    WritePng(dir / "f_10.png", 16, 8, Qt::blue);
    Fixture f;

    ASSERT_TRUE(f.sequence.Load(Q(dir), 12));
    EXPECT_TRUE(f.sequence.IsShowing());
    EXPECT_EQ(f.sequence.FrameCount(), 3);
    EXPECT_EQ(f.sequence.CurrentIndex(), 0);
    EXPECT_EQ(f.sequence.Fps(), 12);
    EXPECT_FALSE(f.sequence.IsPlaying());

    // 进度条就是帧号：范围 [0, 2]，当前在 0。
    EXPECT_EQ(f.bar.seekSlider()->minimum(), 0);
    EXPECT_EQ(f.bar.seekSlider()->maximum(), 2);
    EXPECT_EQ(f.bar.seekSlider()->value(), 0);
    // 时间标签写"帧 n / N"，不用 00:00:00 冒充（那是视频的口径）。
    EXPECT_TRUE(f.bar.timeLabel()->text().contains(QStringLiteral("1 / 3")));

    // 序列没有音轨：音量控件禁用。
    EXPECT_FALSE(f.bar.volumeSlider()->isEnabled());
    EXPECT_FALSE(f.bar.volumeButton()->isEnabled());
    // 上一帧/下一帧可见，首帧时"上一帧"禁用。
    // 控件本身没 show（offscreen 且没有父窗口），isVisible() 恒为 false；
    // 这里要问的是"逻辑上有没有被隐藏"，用 isHidden()。
    EXPECT_FALSE(f.bar.prevFrameButton()->isHidden());
    EXPECT_FALSE(f.bar.nextFrameButton()->isHidden());
    EXPECT_FALSE(f.bar.prevFrameButton()->isEnabled());
    EXPECT_TRUE(f.bar.nextFrameButton()->isEnabled());
}

TEST(ImageSequencePlayerTests, ShowFrameMovesWithinBoundsOnly) {
    EnsureApp();
    const fs::path dir = FreshFolder("bounds");
    WritePng(dir / "a.png", 8, 8, Qt::red);
    WritePng(dir / "b.png", 8, 8, Qt::green);
    Fixture f;
    ASSERT_TRUE(f.sequence.Load(Q(dir), 25));

    ASSERT_TRUE(f.sequence.ShowFrame(1));
    EXPECT_EQ(f.sequence.CurrentIndex(), 1);
    EXPECT_EQ(f.bar.seekSlider()->value(), 1);
    EXPECT_TRUE(f.bar.prevFrameButton()->isEnabled());
    EXPECT_FALSE(f.bar.nextFrameButton()->isEnabled()); // 末帧

    // 越界：返回 false 且不改变当前帧（免得"点过头"跳到别的图）。
    EXPECT_FALSE(f.sequence.ShowFrame(2));
    EXPECT_FALSE(f.sequence.ShowFrame(-1));
    EXPECT_EQ(f.sequence.CurrentIndex(), 1);
}

// ---------------------------------------------------------------------------
// 3. 播放开关
// ---------------------------------------------------------------------------
TEST(ImageSequencePlayerTests, StartRestartsFromFirstFrameWhenSittingOnLast) {
    EnsureApp();
    const fs::path dir = FreshFolder("restart");
    WritePng(dir / "a.png", 8, 8, Qt::red);
    WritePng(dir / "b.png", 8, 8, Qt::green);
    Fixture f;
    ASSERT_TRUE(f.sequence.Load(Q(dir), 25));
    ASSERT_TRUE(f.sequence.ShowFrame(1)); // 停在末帧

    EXPECT_TRUE(f.sequence.Start());
    EXPECT_TRUE(f.sequence.IsPlaying());
    // 末帧再按播放必须回到第一张，否则用户看到的是"点了没反应"。
    EXPECT_EQ(f.sequence.CurrentIndex(), 0);

    f.sequence.Stop();
    EXPECT_FALSE(f.sequence.IsPlaying());
}

TEST(ImageSequencePlayerTests, ResetClearsEverything) {
    EnsureApp();
    const fs::path dir = FreshFolder("reset");
    WritePng(dir / "a.png", 8, 8, Qt::red);
    WritePng(dir / "b.png", 8, 8, Qt::green);
    Fixture f;
    ASSERT_TRUE(f.sequence.Load(Q(dir), 25));
    f.sequence.Start();

    f.sequence.Reset();
    EXPECT_FALSE(f.sequence.IsShowing());
    EXPECT_FALSE(f.sequence.IsPlaying());
    EXPECT_EQ(f.sequence.FrameCount(), 0);
    EXPECT_EQ(f.sequence.CurrentIndex(), -1);
    // 退出序列后按钮收起来，时间标签回到时码口径。
    EXPECT_TRUE(f.bar.nextFrameButton()->isHidden());
    EXPECT_TRUE(f.bar.timeLabel()->text().contains(QStringLiteral("00:00:00")));
}

TEST(ImageSequencePlayerTests, NonPositiveFpsFallsBackToDefault) {
    EnsureApp();
    const fs::path dir = FreshFolder("fps");
    WritePng(dir / "a.png", 8, 8, Qt::red);
    WritePng(dir / "b.png", 8, 8, Qt::green);
    Fixture f;

    ASSERT_TRUE(f.sequence.Load(Q(dir), 0));
    EXPECT_EQ(f.sequence.Fps(), 25);
}
