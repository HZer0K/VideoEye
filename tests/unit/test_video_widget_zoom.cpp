// VideoWidget 的缩放 / 平移接缝测试。
//
// 为什么值得单独测：这段几何一旦算错，界面上表现为"放大了但看不到想看的那块"或
// "拖到头了画面还能再拖出一片背景色"——两种都像卡死，光看截图分辨不出是几何错还是
// 绘制错。这里把三件事钉住：
//   1. 倍率语义：zoomFactor() 是**相对适应窗口**的倍率，1:1 像素是 1/适应比例。
//      直接改成"绝对缩放"会让 1:1 与适应窗口在窗口 resize 后互相跑掉。
//   2. 光标锚点：滚轮缩放后，光标下那个画面点必须还在光标下（否则放大十几下才能
//      找回刚才看的像素）。
//   3. 边界：平移不许越界露出背景；缩小/放大都夹在 [kZoomMin, kZoomMax] 内。
//
// 另外用 render() 做一次真实绘制检查：32 倍放大且未平移时，整个控件都该是画面像素
// （如果裁剪矩形算错，边缘会露出背景色 —— 这正是"放大后四边有黑边"那个 bug 的形状）。
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>

#include "ui/main_window/VideoWidget.h"

using videoeye::ui::VideoWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_video_widget_zoom";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QImage SolidImage(int width, int height, QColor color) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

void SendWheel(VideoWidget& widget, const QPointF& pos, int delta) {
    QWheelEvent event(pos, widget.mapToGlobal(pos.toPoint()), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &event);
}

void SendMouse(VideoWidget& widget, QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(pos), QPointF(widget.mapToGlobal(pos)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 倍率语义
// ---------------------------------------------------------------------------
TEST(VideoWidgetZoomTests, FitIsIdentityAndDisplayRectIsLetterboxed) {
    EnsureApp();
    VideoWidget widget;
    // 控件最小尺寸是 320x160，resize 到更小会被钳回去；用 400x400 让几何可算。
    widget.resize(400, 400);
    widget.SetFrame(SolidImage(100, 50, Qt::red));

    EXPECT_DOUBLE_EQ(widget.zoomFactor(), 1.0);
    EXPECT_TRUE(widget.IsFitMode());
    EXPECT_FALSE(widget.CanPan());

    // 适应窗口：100x50 在 400x400 里放大 4 倍，居中 —— 上下各留 100 像素黑边。
    const QRectF rect = widget.DisplayRect();
    EXPECT_DOUBLE_EQ(rect.width(), 400.0);
    EXPECT_DOUBLE_EQ(rect.height(), 200.0);
    EXPECT_DOUBLE_EQ(rect.left(), 0.0);
    EXPECT_DOUBLE_EQ(rect.top(), 100.0);
}

TEST(VideoWidgetZoomTests, ActualSizeMeansOneSourcePixelPerScreenPixel) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(400, 400);
    widget.SetFrame(SolidImage(100, 50, Qt::red));

    widget.SetActualSize();
    // 适应倍率是 4，"一个源像素对一个屏幕像素"就是 0.25 倍。
    EXPECT_NEAR(widget.zoomFactor(), 0.25, 1e-9);
    EXPECT_FALSE(widget.IsFitMode());

    const QRectF rect = widget.DisplayRect();
    EXPECT_NEAR(rect.width(), 100.0, 1e-6);
    EXPECT_NEAR(rect.height(), 50.0, 1e-6);
    EXPECT_NEAR(rect.left(), 150.0, 1e-6);
    EXPECT_NEAR(rect.top(), 175.0, 1e-6);

    widget.ResetZoom();
    EXPECT_DOUBLE_EQ(widget.zoomFactor(), 1.0);
    EXPECT_TRUE(widget.IsFitMode());
}

TEST(VideoWidgetZoomTests, ZoomIsClampedAtBothEnds) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(200, 200);
    widget.SetFrame(SolidImage(100, 50, Qt::red));

    for (int i = 0; i < 100; ++i) {
        widget.ZoomIn();
    }
    EXPECT_DOUBLE_EQ(widget.zoomFactor(), 32.0);
    for (int i = 0; i < 100; ++i) {
        widget.ZoomOut();
    }
    EXPECT_DOUBLE_EQ(widget.zoomFactor(), 0.05);
}

// ---------------------------------------------------------------------------
// 2. 光标锚点
// ---------------------------------------------------------------------------
TEST(VideoWidgetZoomTests, WheelKeepsThePointUnderTheCursorInPlace) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(400, 400);
    widget.SetFrame(SolidImage(400, 400, Qt::red));

    const QPointF cursor(100.0, 100.0);
    const QRectF before = widget.DisplayRect();
    const double nx = (cursor.x() - before.left()) / before.width();
    const double ny = (cursor.y() - before.top()) / before.height();

    SendWheel(widget, cursor, 120);

    EXPECT_GT(widget.zoomFactor(), 1.0);
    const QRectF after = widget.DisplayRect();
    // 同一个归一化画面点，缩放后仍落在光标位置（允许 1 像素内的浮点误差）。
    EXPECT_NEAR(after.left() + nx * after.width(), cursor.x(), 1.0);
    EXPECT_NEAR(after.top() + ny * after.height(), cursor.y(), 1.0);

    // 反向滚一格：又回到原点附近（倍率与平移都该退回去）。
    SendWheel(widget, cursor, -120);
    EXPECT_NEAR(widget.zoomFactor(), 1.0, 1e-9);
    EXPECT_TRUE(widget.DisplayRect().topLeft() == QPointF(0.0, 0.0));
}

// ---------------------------------------------------------------------------
// 3. 平移边界
// ---------------------------------------------------------------------------
TEST(VideoWidgetZoomTests, DragPansButNeverExposesBackground) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(400, 400);
    widget.SetFrame(SolidImage(400, 400, Qt::red));

    widget.SetZoom(2.0); // 800x800，四周各可移动 200 像素
    ASSERT_TRUE(widget.CanPan());

    SendMouse(widget, QEvent::MouseButtonPress, QPoint(200, 200), Qt::LeftButton, Qt::LeftButton);
    SendMouse(widget, QEvent::MouseMove, QPoint(900, 900), Qt::NoButton, Qt::LeftButton);
    SendMouse(widget, QEvent::MouseButtonRelease, QPoint(900, 900), Qt::LeftButton, Qt::NoButton);

    // 拖到最右下：画面左上角刚好贴住控件左上角，再往右就是背景色了，必须被钳住。
    const QRectF rect = widget.DisplayRect();
    EXPECT_NEAR(rect.left(), 0.0, 1e-6);
    EXPECT_NEAR(rect.top(), 0.0, 1e-6);

    // 反向拖回最左上：右下角贴住控件右下角。
    SendMouse(widget, QEvent::MouseButtonPress, QPoint(300, 300), Qt::LeftButton, Qt::LeftButton);
    SendMouse(widget, QEvent::MouseMove, QPoint(-900, -900), Qt::NoButton, Qt::LeftButton);
    SendMouse(widget, QEvent::MouseButtonRelease, QPoint(-900, -900), Qt::LeftButton, Qt::NoButton);
    const QRectF back = widget.DisplayRect();
    EXPECT_NEAR(back.right(), 400.0, 1e-6);
    EXPECT_NEAR(back.bottom(), 400.0, 1e-6);
}

TEST(VideoWidgetZoomTests, PanIsDroppedWhenZoomingBackToFit) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(400, 400);
    widget.SetFrame(SolidImage(400, 400, Qt::red));

    widget.SetZoom(4.0);
    SendMouse(widget, QEvent::MouseButtonPress, QPoint(200, 200), Qt::LeftButton, Qt::LeftButton);
    SendMouse(widget, QEvent::MouseMove, QPoint(260, 260), Qt::NoButton, Qt::LeftButton);
    SendMouse(widget, QEvent::MouseButtonRelease, QPoint(260, 260), Qt::LeftButton, Qt::NoButton);
    ASSERT_FALSE(widget.IsFitMode());

    widget.ResetZoom();
    // 缩回整幅可见时必须同时归零平移，否则画面会被推到一边（看起来像被裁了）。
    EXPECT_TRUE(widget.IsFitMode());
    EXPECT_TRUE(widget.DisplayRect().topLeft() == QPointF(0.0, 0.0));
}

// ---------------------------------------------------------------------------
// 4. 真实绘制：放大后不该露出背景
// ---------------------------------------------------------------------------
TEST(VideoWidgetZoomTests, ZoomedPaintCoversTheWholeViewport) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(200, 200); // 会被钳到最小 320x160 以上，只要求"放大后铺满"
    widget.SetFrame(SolidImage(400, 400, QColor(255, 0, 0)));
    widget.SetZoom(8.0);

    QImage canvas(200, 200, QImage::Format_RGB32);
    canvas.fill(QColor(1, 6, 14)); // 与控件的占位背景同色，露底会被认出来
    widget.render(&canvas);

    for (int y = 2; y < 198; y += 8) {
        for (int x = 2; x < 198; x += 8) {
            const QColor px = canvas.pixelColor(x, y);
            ASSERT_GT(px.red(), 200) << "放大后 (" << x << "," << y << ") 不是画面像素";
        }
    }
}

TEST(VideoWidgetZoomTests, ClearResetsZoomState) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(200, 200);
    widget.SetFrame(SolidImage(100, 50, Qt::red));
    widget.SetZoom(3.0);

    widget.Clear();
    EXPECT_DOUBLE_EQ(widget.zoomFactor(), 1.0);
    EXPECT_TRUE(widget.IsFitMode());
    EXPECT_TRUE(widget.DisplayRect().isNull());
}

TEST(VideoWidgetZoomTests, DoubleClickTogglesBetweenFitAndActualSize) {
    EnsureApp();
    VideoWidget widget;
    widget.resize(400, 400);
    widget.SetFrame(SolidImage(100, 50, Qt::red));

    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(100, 100), QPointF(widget.mapToGlobal(QPoint(100, 100))),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &dbl);
    EXPECT_NEAR(widget.zoomFactor(), 0.25, 1e-9); // 1:1

    QMouseEvent dbl2(QEvent::MouseButtonDblClick, QPointF(100, 100), QPointF(widget.mapToGlobal(QPoint(100, 100))),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &dbl2);
    EXPECT_DOUBLE_EQ(widget.zoomFactor(), 1.0); // 回到适应窗口
}
