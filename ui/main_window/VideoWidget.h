#pragma once

#include <QWidget>
#include <QImage>
#include <QString>
#include <QPointer>
#include <QRectF>

#include "core/model/MacroblockInfo.h"

namespace videoeye {
namespace ui {

// 视频叠加信息 (分辨率/编码/FPS/状态)
struct VideoOverlayInfo {
    QString resolution;    // e.g. "1920x1080"
    QString codec;         // e.g. "H.264"
    QString fps;           // e.g. "30 fps"
    QString status;        // e.g. "播放中"
    bool is_playing = false;
    bool has_video = false;
};

// MV 叠加显示模式
enum class MvOverlayMode {
    Off,            // 关闭
    Arrows,         // 箭头模式 (默认): 每个块画一个箭头表示运动方向和幅度
    Blocks,         // 块模式: 用颜色填充块, 颜色表示运动幅度 (热力图)
    ArrowsAndBlocks // 箭头+块: 同时显示
};

// 视频叠加层 Widget — 透明背景，绘制信息徽标、中央播放按钮和运动矢量叠加
class VideoOverlayWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoOverlayWidget(QWidget* parent = nullptr);
    void SetOverlayInfo(const VideoOverlayInfo& info);
    void SetCenterPlayButtonVisible(bool visible);

    // 运动矢量叠加
    void SetMotionVectors(const model::MacroblockFrameAnalysis& analysis);
    void SetMvOverlayMode(MvOverlayMode mode);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VideoOverlayInfo info_;
    bool show_center_play_ = true;

    // MV 叠加数据
    model::MacroblockFrameAnalysis mv_analysis_;
    MvOverlayMode mv_mode_ = MvOverlayMode::Off;

    // 计算视频在 widget 中的实际显示区域 (保持宽高比居中)
    QRectF ComputeVideoDisplayRect() const;
    void DrawMvArrows(QPainter& painter, const QRectF& display_rect);
    void DrawMvBlocks(QPainter& painter, const QRectF& display_rect);
    void DrawMvLegend(QPainter& painter, const QRectF& display_rect);
};

// 视频显示 Widget — QImage / CPU 渲染。
//
// 说明: 早期版本在这里挂了一套 Vulkan 直渲路径 (VulkanVideoWidget)，
// 但对一个以文件分析为核心的工具来说，它带来的构建与环境成本远大于收益
// （本机实测也因缺少 present-capable queue family 长期回退 CPU），故已移除。
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    // 显示一帧（解码线程转换好的 QImage）。空图时显示占位文案。
    void SetFrame(const QImage& image);

    // 清除显示内容（重置为占位画面）
    void Clear();

    // 更新叠加层信息
    void SetOverlayInfo(const VideoOverlayInfo& info);

    // 设置中央播放按钮可见性
    void SetCenterPlayButtonVisible(bool visible);

    // 运动矢量叠加
    void SetMotionVectors(const model::MacroblockFrameAnalysis& analysis);
    void SetMvOverlayMode(MvOverlayMode mode);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QImage frame_;
    VideoOverlayWidget* overlay_ = nullptr;
};

} // namespace ui
} // namespace videoeye
