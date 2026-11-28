#pragma once

// 图片序列预览：把一个目录里的图片当成"按帧率播放的一组帧"。
//
// 为什么需要单独一个组件：VideoEye 的抽帧导出（FfmpegPanel）产出的就是一组图片，
// 而打开链路一次只能开一个文件 —— 用户拿到 120 张 frame_0001.png 只能一张张点。
// 这里把整目录按自然序加载，按指定帧率连续播放，逐帧前后翻，进度条当成帧号。
//
// 与既有路径的关系（重要，别混）：
//   * 它**不**走 FFmpeg 解码，也不进播放会话：一帧一次 QImageReader + 一次方向变换。
//     image2 解复用器虽然能吃序列 pattern，但要求文件名带 %04d 那样的占位符，
//     抽帧目录一旦被改过名就整组打不开；而逐帧重新 avformat_open_input 也不可能
//     按 24fps 跟得上。这条路径的定位是"预览"，不承担分析。
//   * 因此它**不产生**可分析的媒体：调用方必须把分析面板/首页的"当前文件"清掉
//     （与裸图像预览同一约定，见 OpenSessionController::OpenImageSequence）。
//   * 分析/导出仍请逐帧打开单文件（那是完整链路）。
//
// 与 RawImageSequence 的关系：那个管"无封装裸像素"（按宽高直读 + 手写 YUV 转换），
// 这个管"一堆成品图片文件"。两者的控制栏契约一致（进度条 = 帧号、末帧自停、不循环），
// 所以 PlayerPanel 里能用同一套分派。

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer; // 全局转发声明：不能写进 videoeye::ui，否则会声明出另一个类型

namespace videoeye {
namespace ui {

class VideoWidget;
class ControlBarWidget;

class ImageSequencePlayer : public QObject {
    Q_OBJECT

public:
    // parent 同时作为错误提示的归属窗口，也用于父子生命周期。
    explicit ImageSequencePlayer(QObject* parent = nullptr);

    void SetVideoWidget(VideoWidget* widget);
    void SetControlBar(ControlBarWidget* bar);

    // 加载一个目录（自然序）。少于 2 张不算序列（单张请直接用"打开文件"，
    // 那条路径有完整分析）。fps <= 0 时按 25 处理。
    // 失败时返回 false，失败原因经 StatusMessage 发出，状态保持"未加载"。
    bool Load(const QString& folder, int fps);
    void Reset();

    bool IsShowing() const {
        return !files_.isEmpty();
    }
    int FrameCount() const {
        return files_.size();
    }
    int CurrentIndex() const {
        return current_index_;
    }
    int Fps() const {
        return fps_;
    }

    // 显示第 index 帧（越界返回 false）。首帧解码失败会报错并保持原画面。
    bool ShowFrame(int index);

    // 连续播放：按帧率推进，到末帧自停（不循环 —— 与 Raw 序列一致，
    // 循环会掩盖"序列尾部缺帧"这类问题）。单帧/未加载时返回 false。
    bool Start();
    void Stop();
    bool IsPlaying() const {
        return playing_;
    }

signals:
    void StatusMessage(const QString& text, int timeout);
    // 显示一帧后通知面板：叠加层的编码/分辨率缓存归面板所有。
    void FrameShown(const QImage& frame, const QString& codec);
    void PlaybackStateChanged(bool playing);

private:
    void OnTick();
    void UpdateNavigation();
    // 解码一帧并按显示方向摆正。失败返回空图，reason 带面向用户的原因。
    QImage DecodeFrame(int index, QString& reason) const;

    QStringList files_;
    QString folder_;
    int current_index_ = -1;
    int fps_ = 25;
    bool playing_ = false;

    // 单次定时器：每解完一帧再排下一次。若固定间隔，解码慢于间隔时事件会积压，
    // 表现为"越播越快 / 卡一下之后连跳几帧"；单次重排只会让实际帧率变低，不会失真。
    QTimer* tick_timer_ = nullptr;

    VideoWidget* video_widget_ = nullptr;
    ControlBarWidget* control_bar_ = nullptr;
};

} // namespace ui
} // namespace videoeye
