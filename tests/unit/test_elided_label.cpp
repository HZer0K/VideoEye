// ElidedLabel 的回归（ui/main_window/ElidedLabel）。
//
// 守的是一个"只在特定屏幕上才显形"的排版坑：应用栏里的媒体路径是任意长度的绝对
// 路径，而 QLabel 非自动换行时的 minimumSizeHint 就是整段文本宽度 —— 布局把
// minimumSizeHint 当硬下限，于是打开一个深层目录里的文件就能把应用栏撑宽，
// 右侧的「打开文件 / 打开 URL」被挤出屏幕（1366x768 + 150% 缩放约 910 逻辑像素）。
//
// 所以这里钉住三件事：
//   1. minimumSizeHint 远小于文本宽度（长路径不许抬高布局下限）；
//   2. 宽度不够时展示文本被省略，但完整文本仍可从 FullText()/tooltip 取到；
//   3. 宽度变化会重新省略（而不是只在设文本那一刻算一次）。
//
// 只构造控件（offscreen），不渲染。

#include <gtest/gtest.h>

#include <QApplication>
#include <QFontMetrics>
#include <QWidget>

#include "ui/main_window/ElidedLabel.h"

using videoeye::ui::ElidedLabel;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_elided_label";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

const QString kLongPath =
    QStringLiteral("D:/projects/broadcast/2026/october/customer_asset_library/raw_originals/camera_a/scene_017/"
                   "take_004/final_grading/veryeye_probe_4k_hevc_10bit_main.mkv");

} // namespace

// 1) 长路径不许抬高布局下限：这是整个类的存在理由
TEST(ElidedLabelTests, LongPathDoesNotRaiseTheMinimumWidth) {
    EnsureApp();
    QWidget host;
    host.show(); // Qt 只对**可见**控件发 resizeEvent：隐藏控件的 resize 不会重算省略
    ElidedLabel label(&host);
    label.show();
    label.SetFullText(kLongPath);

    const int text_width = QFontMetrics(label.font()).horizontalAdvance(kLongPath);
    ASSERT_GT(text_width, 400) << "用例前提：这条路径本身足够宽";

    EXPECT_LT(label.minimumSizeHint().width(), 160)
        << "下限必须很小，否则布局会把应用栏撑到比屏幕还宽（实测值 " << label.minimumSizeHint().width() << "）";
    EXPECT_LT(label.sizeHint().width(), text_width) << "sizeHint 也不该等于文本宽度，否则布局没有理由把它压窄";
}

// 2) 窄的时候省略中间，完整文本仍拿得到
TEST(ElidedLabelTests, NarrowWidthElidesTheMiddleButKeepsTheFullText) {
    EnsureApp();
    QWidget host;
    host.show(); // Qt 只对**可见**控件发 resizeEvent：隐藏控件的 resize 不会重算省略
    ElidedLabel label(&host);
    label.show();
    label.SetFullText(kLongPath);
    label.resize(120, 24);

    EXPECT_EQ(label.FullText(), kLongPath);
    EXPECT_EQ(label.toolTip(), kLongPath) << "完整路径必须还能看到（悬停提示）";
    EXPECT_NE(label.text(), kLongPath) << "窄宽度下不该原样铺开";
    EXPECT_TRUE(label.text().contains(QStringLiteral("…"))) << label.text().toStdString();
    // 省略保留首尾：盘符开头与扩展名结尾都还在（ElideMiddle 在极窄宽度下会把
    // 尾部保留到只剩扩展名本身，所以这里比扩展名而不是完整文件名）
    EXPECT_TRUE(label.text().startsWith(QStringLiteral("D:"))) << label.text().toStdString();
    EXPECT_TRUE(label.text().endsWith(QStringLiteral("mkv"))) << label.text().toStdString();
}

// 3) 宽度变化要重新算：先窄后宽必须恢复完整文本
TEST(ElidedLabelTests, ResizingRecomputesTheElision) {
    EnsureApp();
    QWidget host;
    host.show(); // Qt 只对**可见**控件发 resizeEvent：隐藏控件的 resize 不会重算省略
    ElidedLabel label(&host);
    label.show();
    label.SetFullText(kLongPath);
    label.resize(120, 24);
    ASSERT_TRUE(label.text().contains(QStringLiteral("…")));

    // 留足余量：只多给几像素时 elidedText 仍可能判"放不下"（度量按整像素取整）
    const int needed = QFontMetrics(label.font()).horizontalAdvance(kLongPath) + 32;
    label.resize(needed, 24);
    EXPECT_EQ(label.text(), kLongPath) << "宽度够时应当完整显示";

    label.resize(100, 24);
    EXPECT_TRUE(label.text().contains(QStringLiteral("…"))) << "再缩回去必须重新省略";
}

// 4) 空文本不留下"…"这种残影
TEST(ElidedLabelTests, EmptyTextClearsEverything) {
    EnsureApp();
    QWidget host;
    host.show(); // Qt 只对**可见**控件发 resizeEvent：隐藏控件的 resize 不会重算省略
    ElidedLabel label(&host);
    label.resize(120, 24);
    label.SetFullText(QString());
    EXPECT_TRUE(label.text().isEmpty());
    EXPECT_TRUE(label.FullText().isEmpty());
}
