#pragma once

#include <QObject>
#include <QWidget>
#include <QString>
#include <QImage>

namespace videoeye {
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
class RawImageSequence : public QObject {
    Q_OBJECT

public:
    // parent 同时作为 QInputDialog / QMessageBox 的父窗口（防止对话框无归属）。
    explicit RawImageSequence(QWidget* parent = nullptr);

    // 注入渲染目标与导航控件（SetupUI 创建后调用）。
    void SetVideoWidget(VideoWidget* widget);
    void SetControlBar(ControlBarWidget* bar);

    // --- 业务接口（PlayerPanel 透传 MainWindow 调用）---
    bool LoadRawImageFile(const QString& filename);
    bool ShowRawFrame(int frame_index);
    void UpdateRawNavigationState();
    void OnPrevRawFrame();
    void OnNextRawFrame();
    void SetRawImageMode(bool on);

    // 复位所有 Raw 序列状态（不刷新导航控件，调用方自行在之后调 UpdateRawNavigationState）。
    void Reset();

    bool IsShowingRawImage() const { return showing_raw_image_; }

signals:
    // 与 PlayerPanel 同名信号对齐，由面板转发到真正订阅者（MainWindow 状态栏 / 媒体信息框）。
    void StatusMessage(const QString& text, int timeout);
    void RawImageInfoReady(const QString& info);
    // 显示一帧后通知面板：由面板写 last_overlay_codec_ 并刷新叠加层（overlay 状态归面板所有）。
    void RawFrameShown(const QImage& frame, const QString& codec);

private:
    // === Raw 序列状态（原 PlayerPanel 成员整体迁入）===
    bool showing_raw_image_ = false;
    QString raw_image_path_;
    QString raw_pixel_format_;
    int raw_width_ = 0;
    int raw_height_ = 0;
    qint64 raw_frame_size_ = 0;
    int raw_total_frames_ = 0;
    int raw_current_frame_ = 0;

    // 非拥有依赖
    QWidget* dialog_parent_ = nullptr;
    VideoWidget* video_widget_ = nullptr;
    ControlBarWidget* control_bar_ = nullptr;
};

} // namespace ui
} // namespace videoeye
