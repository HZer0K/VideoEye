// PlayerPanel 的「图片序列」接线测试。
//
// ImageSequencePlayer 自己的边界在 test_image_sequence_player 里覆盖；这里盯的是
// **协调层**有没有把它接上 —— 这段代码的失效方式很隐蔽：
//   * 打开序列后按钮点了没反应（分派写漏了某个入口）；
//   * 停止后序列还在后台推进（OnStop 忘了 Reset）；
//   * 加载失败时仍被当成"正在显示"，后续所有操作都对着一个空序列。
// 这些都不会崩，只会"看起来不对"，所以用可观察量（进度条值 / IsShowing*）钉住。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QPushButton>
#include <QSlider>

#include <filesystem>
#include <system_error>

#include "core/domain/io/Utf8Path.h"
#include "ui/player/PlayerPanel.h"

namespace fs = std::filesystem;

using videoeye::ToNativePath;
using videoeye::ToUtf8String;
using videoeye::ui::PlayerPanel;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_player_panel_image_mode";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

fs::path FreshFolder(const std::string& name) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("videoeye_panel_image_" + name);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

QString Q(const fs::path& path) {
    return QString::fromStdString(ToUtf8String(path));
}

void WritePng(const fs::path& path, QColor color) {
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(color);
    ASSERT_TRUE(image.save(Q(path), "PNG"));
}

} // namespace

TEST(PlayerPanelImageModeTests, LoadSequenceTurnsOnSequenceMode) {
    EnsureApp();
    const fs::path dir = FreshFolder("load");
    WritePng(dir / "a.png", Qt::red);
    WritePng(dir / "b.png", Qt::green);
    WritePng(dir / "c.png", Qt::blue);

    PlayerPanel panel;
    panel.resize(640, 480);
    EXPECT_FALSE(panel.IsShowingImageSequence());

    ASSERT_TRUE(panel.LoadImageSequence(Q(dir), 10));
    EXPECT_TRUE(panel.IsShowingImageSequence());
    // 序列与 Raw 序列是互斥模式，不能同时成立。
    EXPECT_FALSE(panel.IsShowingRawImage());
}

TEST(PlayerPanelImageModeTests, LoadSequenceFailsOnEmptyFolder) {
    EnsureApp();
    const fs::path dir = FreshFolder("empty");

    PlayerPanel panel;
    panel.resize(640, 480);
    EXPECT_FALSE(panel.LoadImageSequence(Q(dir), 10));
    EXPECT_FALSE(panel.IsShowingImageSequence());
}

TEST(PlayerPanelImageModeTests, StopClearsSequenceMode) {
    EnsureApp();
    const fs::path dir = FreshFolder("stop");
    WritePng(dir / "a.png", Qt::red);
    WritePng(dir / "b.png", Qt::green);

    PlayerPanel panel;
    panel.resize(640, 480);
    ASSERT_TRUE(panel.LoadImageSequence(Q(dir), 10));

    // 停止必须把序列一起收掉：它的"播放"不属于播放器状态机，不主动停就会一直翻。
    panel.StopPlayback();
    EXPECT_FALSE(panel.IsShowingImageSequence());
}

TEST(PlayerPanelImageModeTests, NextFrameButtonAdvancesTheSequence) {
    EnsureApp();
    const fs::path dir = FreshFolder("nav");
    WritePng(dir / "a.png", Qt::red);
    WritePng(dir / "b.png", Qt::green);
    WritePng(dir / "c.png", Qt::blue);

    PlayerPanel panel;
    panel.resize(640, 480);
    ASSERT_TRUE(panel.LoadImageSequence(Q(dir), 10));

    // 进度条在序列模式下就是帧号：点一次"下一帧"应该走到 1。
    // 取进度条不能靠"第一个 QSlider"—— 控制栏里还有音量条（objectName 显式标了）。
    QSlider* slider = nullptr;
    for (QSlider* candidate : panel.findChildren<QSlider*>()) {
        if (candidate->objectName() != QStringLiteral("volumeSlider") && candidate->maximum() == 2) {
            slider = candidate;
            break;
        }
    }
    QPushButton* next = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>()) {
        if (!button->isHidden() && button->toolTip().contains(QStringLiteral("下一帧"))) {
            next = button;
            break;
        }
    }
    ASSERT_NE(next, nullptr) << "找不到「下一帧」按钮（说明序列模式没把导航按钮摆出来）";
    ASSERT_NE(slider, nullptr) << "找不到进度条（或它的范围不是 0..图片数-1）";

    next->click();
    EXPECT_EQ(slider->value(), 1);
    next->click();
    EXPECT_EQ(slider->value(), 2);
    // 末帧再点：越界不跳（ShowFrame 返回 false，帧号停在 2）。
    next->click();
    EXPECT_EQ(slider->value(), 2);
}
