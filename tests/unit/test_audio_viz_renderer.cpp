// AudioVizRenderer（纯音频模式下的音频可视化渲染器）的接缝测试。
//
// 为什么值得单独测：它把「瞬时音量 / 频谱 / 波形」画到注入的 VideoWidget 上，是从
// PlayerPanel 抽出的独立组件。它有一个明确的防护分支（渲染目标未注入时直接返回），
// 以及一条把「收到数据 -> 画一帧 -> SetFrame 到目标控件」串起来的渲染链。这条链断了
// 不会崩，只会让纯音频模式停在一片占位黑屏 —— 界面上最难发现的一类回归。
//
// 具体盯三件事：
//   1. 未注入渲染目标时收发数据、Reset 都不能崩（player_ 为空指针的历史坑）。
//   2. 首个音量回调必须真的把一帧画进 VideoWidget（渲染前后控件自绘输出不同）。
//   3. 频谱/波形数据必须改变渲染结果 —— 用两个同尺寸、同电平、同时间戳的渲染器对比，
//      排除背景脉动与信息文字造成的干扰，只留"数据确实被画出来"这一条。Reset 之后
//      再收到数据仍然要能正常出帧。
//
// 用 offscreen 平台跑，不需要显示器；靠 QWidget::render() 取控件自绘结果比较像素。

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>

#include <vector>

#include "core/domain/model/AudioVisualizationFrame.h"
#include "ui/main_window/VideoWidget.h"
#include "ui/player/AudioVizRenderer.h"

using videoeye::model::AudioVisualizationFrame;
using videoeye::ui::AudioVizRenderer;
using videoeye::ui::VideoWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_audio_viz_renderer";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 让控件自绘一次到一个内存图（不需要 show），用于比较"画了什么"。
QImage RenderToImage(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    widget.render(&image);
    return image;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 未注入渲染目标：收发数据与 Reset 都不能崩（防护分支）
// ---------------------------------------------------------------------------

TEST(AudioVizRendererTests, WithoutRenderTargetIsSafe) {
    EnsureApp();

    AudioVizRenderer renderer;  // 不调用 SetVideoWidget
    renderer.OnAudioLevelReady(0.8, 0.0);

    AudioVisualizationFrame frame;
    frame.timestamp_seconds = 0.1;
    frame.spectrum_bins.assign(48, 0.9);
    frame.waveform_points.assign(48, 0.5);
    renderer.OnAudioVisualizationForDisplay(frame);

    renderer.Reset();
    renderer.OnAudioLevelReady(0.5, 1.0);

    SUCCEED();  // 走到这里即代表空指针路径没有解引用
}

// ---------------------------------------------------------------------------
// 2. 首个音量回调必须把一帧画进 VideoWidget
// ---------------------------------------------------------------------------

TEST(AudioVizRendererTests, FirstLevelReadyRendersFrameIntoVideoWidget) {
    EnsureApp();

    VideoWidget widget;
    widget.resize(320, 480);

    const QImage before = RenderToImage(widget);  // 未注入时的占位画面

    AudioVizRenderer renderer;
    renderer.SetVideoWidget(&widget);
    renderer.OnAudioLevelReady(0.9, 0.5);

    const QImage after = RenderToImage(widget);
    ASSERT_FALSE(before.isNull());
    ASSERT_FALSE(after.isNull());
    // 渲染链真的落地：控件从"视频显示区域"占位变成了渲染出来的可视化帧
    EXPECT_NE(before, after);
}

// ---------------------------------------------------------------------------
// 3. 频谱 / 波形数据必须改变渲染结果，且 Reset 后仍能继续出帧
// ---------------------------------------------------------------------------

TEST(AudioVizRendererTests, SpectrumAndWaveformDataReachRenderTarget) {
    EnsureApp();

    // 基线：同尺寸、同电平、同时间戳，但不喂频谱/波形
    VideoWidget plain_widget;
    plain_widget.resize(320, 480);
    AudioVizRenderer plain_renderer;
    plain_renderer.SetVideoWidget(&plain_widget);
    plain_renderer.OnAudioLevelReady(0.0, 0.5);
    const QImage plain = RenderToImage(plain_widget);

    // 对照：先喂频谱/波形，再以完全相同的电平与时间戳触发渲染
    VideoWidget rich_widget;
    rich_widget.resize(320, 480);
    AudioVizRenderer rich_renderer;
    rich_renderer.SetVideoWidget(&rich_widget);

    AudioVisualizationFrame frame;
    frame.timestamp_seconds = 0.5;
    frame.spectrum_bins.assign(64, 0.9);
    frame.waveform_points.assign(64, 0.5);
    rich_renderer.OnAudioVisualizationForDisplay(frame);
    rich_renderer.OnAudioLevelReady(0.0, 0.5);

    const QImage rich = RenderToImage(rich_widget);

    // 背景脉动、占位符、信息文字在两路里完全一致，差异只能来自频谱条/波形
    ASSERT_FALSE(plain.isNull());
    ASSERT_FALSE(rich.isNull());
    EXPECT_NE(plain, rich);

    // Reset 清空运行状态后，再来一帧仍应正常渲染（不能只剩占位画面）
    VideoWidget placeholder;
    placeholder.resize(320, 480);
    const QImage placeholder_image = RenderToImage(placeholder);

    rich_renderer.Reset();
    rich_renderer.OnAudioLevelReady(0.7, 1.0);
    const QImage after_reset = RenderToImage(rich_widget);
    EXPECT_NE(placeholder_image, after_reset);
}