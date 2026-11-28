#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

class QFile;  // 全局转发声明：不能写进 videoeye::ui，否则会声明出另一个类型
class QTimer; // 同上

namespace videoeye {

namespace player {
class RawFrameConverter; // 裸帧 → RGB888（sws_scale 后端），见 core/player/RawFrameConverter.h
}

namespace ui {

// 前向声明必须放在 videoeye::ui 内：真实类是 videoeye::ui::VideoWidget /
// videoeye::ui::ControlBarWidget，写在全局命名空间会声明出两个不存在的 ::VideoWidget /
// ::ControlBarWidget，导致成员变成不完全类型。
class VideoWidget;
class ControlBarWidget;

// Raw 图像序列（.yuv/.nv12/.rgb/.yuy2 等无封装裸数据）的加载与逐帧浏览。
//
// 从 PlayerPanel 抽出为独立组件：原 PlayerPanel 把整个 Raw 序列逻辑（解析文件名、
// 弹参数对话框、读盘解码、绘制到 VideoWidget、回写控制栏导航状态）和 9 个成员混在
// 一起，导致该类膨胀。本组件只负责"给定路径 → 逐帧显示"，PlayerPanel 仍是协调层：
//   - 渲染目标（VideoWidget）与导航控件（ControlBarWidget）由面板注入；
//   - overlay 的编码/分辨率缓存仍归面板（属于全局叠加层状态，非 Raw 私有），
//     故显示一帧后发 RawFrameShown(frame, codec) 信号，面板据此写 last_overlay_codec_
//     并调 RefreshOverlayMediaInfo；
//   - 状态栏 / 媒体信息框信号由本组件发出、面板原样转发（保持 MainWindow 零改动）。
//
// 逐帧读盘与像素转换不在本组件里实现：文件句柄常驻本组件（省掉每帧 open/close），
// 裸字节 → RGB888 交给 core/player/RawFrameConverter（sws_scale）。界面层不该再出现
// 一份手写的 BT.601/709 系数 —— 那是"库已经做过、且做得更对"的事。
class RawImageSequence : public QObject {
    Q_OBJECT

public:
    // parent 同时作为 QInputDialog / QMessageBox 的父窗口（防止对话框无归属）。
    explicit RawImageSequence(QWidget* parent = nullptr);
    // 必须定义在实现文件里：成员里有 QFile / RawFrameConverter 的 unique_ptr，
    // 它们在这里只是转发声明。隐式析构会在每个销毁点（包括单测）要求完整类型。
    ~RawImageSequence() override;

    // 注入渲染目标与导航控件（SetupUI 创建后调用）。
    void SetVideoWidget(VideoWidget* widget);
    void SetControlBar(ControlBarWidget* bar);

    // --- 业务接口（PlayerPanel 透传 MainWindow 调用）---
    bool LoadRawImageFile(const QString& filename);

    // 参数已知时直接加载，不弹任何对话框（失败时不改变当前序列状态）。
    // 弹窗路径只是"收集参数 → 交给这里"，所以测试能靠它覆盖整条逐帧读盘链路 ——
    // 靠 QInputDialog 的路径永远进不了自动化。
    bool OpenRawSequence(const QString& path, int width, int height, const QString& format_label, int fps);

    bool ShowRawFrame(int frame_index);
    void UpdateRawNavigationState();
    void OnPrevRawFrame();
    void OnNextRawFrame();
    void SetRawImageMode(bool on);

    // 连续播放：按加载时确认的帧率定时推进帧。单帧/未加载时返回 false。
    // 播放到最后一帧自动停止（停在最后一帧），不会循环。
    bool StartRawPlayback();
    void StopRawPlayback();
    bool IsRawPlaying() const {
        return raw_playing_;
    }

    // 复位所有 Raw 序列状态（不刷新导航控件，调用方自行在之后调 UpdateRawNavigationState）。
    void Reset();

    bool IsShowingRawImage() const {
        return showing_raw_image_;
    }

signals:
    // 与 PlayerPanel 同名信号对齐，由面板转发到真正订阅者（MainWindow 状态栏 / 媒体信息框）。
    void StatusMessage(const QString& text, int timeout);
    void RawImageInfoReady(const QString& info);
    // 显示一帧后通知面板：由面板写 last_overlay_codec_ 并刷新叠加层（overlay 状态归面板所有）。
    void RawFrameShown(const QImage& frame, const QString& codec);
    // 连续播放状态变化（开始/暂停/播到末帧）。面板据此切换播放按钮图标。
    void RawPlaybackStateChanged(bool playing);

private:
    void AdvancePlaybackFrame();

    // 常驻句柄：首次读帧时打开，换源/停止/复位时关闭。打开失败会把原因交给状态栏。
    bool EnsureRawFileOpen();
    void CloseRawFile();

    // === Raw 序列状态（原 PlayerPanel 成员整体迁入）===
    bool showing_raw_image_ = false;
    QString raw_image_path_;
    QString raw_pixel_format_;
    int raw_width_ = 0;
    int raw_height_ = 0;
    qint64 raw_frame_size_ = 0;
    int raw_total_frames_ = 0;
    int raw_current_frame_ = 0;
    int raw_fps_ = 25;

    // 逐帧读盘的常驻状态：
    //   * raw_file_ 常驻打开 —— 连续播放时每帧一次 open/close 是白送给文件系统的开销，
    //     网络盘/机械盘上单次 open 就要几百微秒；
    //   * frame_buffer_ 复用 —— 每帧 new 一次 QByteArray 等于每秒几十次 MB 级分配；
    //   * raw_next_offset_ 记录游标该在的位置，顺序读（下一帧/连续播放）直接免掉 seek。
    std::unique_ptr<QFile> raw_file_;
    std::vector<std::uint8_t> frame_buffer_;
    qint64 raw_next_offset_ = -1;

    // 裸帧 → RGB888。跟着序列一起建/销毁，SwsContext 因此跨帧复用。
    std::unique_ptr<player::RawFrameConverter> converter_;

    // 连续播放（Raw 没有音频，QTimer 即时钟）。
    QTimer* playback_timer_ = nullptr;
    bool raw_playing_ = false;

    // 非拥有依赖
    QWidget* dialog_parent_ = nullptr;
    VideoWidget* video_widget_ = nullptr;
    ControlBarWidget* control_bar_ = nullptr;
};

} // namespace ui
} // namespace videoeye
