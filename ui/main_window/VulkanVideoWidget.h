#pragma once

#include <QWidget>
#include <QImage>
#include <QString>

namespace videoeye {
namespace player { class VulkanRenderer; }

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

// 视频叠加层 Widget — 透明背景，绘制信息徽标和中央播放按钮
class VideoOverlayWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoOverlayWidget(QWidget* parent = nullptr);
    void SetOverlayInfo(const VideoOverlayInfo& info);
    void SetCenterPlayButtonVisible(bool visible);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VideoOverlayInfo info_;
    bool show_center_play_ = true;
};

// Vulkan 渲染 Widget — 支持 Vulkan 直接渲染 + CPU 回退模式
class VulkanVideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VulkanVideoWidget(QWidget* parent = nullptr);
    ~VulkanVideoWidget();

    // 设置 Vulkan 渲染器 (非拥有)
    void SetVulkanRenderer(player::VulkanRenderer* renderer);

    // 回退模式: 当 Vulkan 不可用时显示 QImage
    void SetFallbackImage(const QImage& image);

    // 清除显示内容（重置为黑屏）
    void Clear();

    // 获取原生窗口句柄
    WId GetNativeHandle() const { return winId(); }

    // Vulkan 渲染是否活跃
    bool IsVulkanActive() const { return vulkan_active_; }

    // 更新叠加层信息
    void SetOverlayInfo(const VideoOverlayInfo& info);

    // 设置中央播放按钮可见性
    void SetCenterPlayButtonVisible(bool visible);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    player::VulkanRenderer* renderer_ = nullptr;
    QImage fallback_image_;
    bool vulkan_active_ = false;
    bool first_show_ = true;
    VideoOverlayWidget* overlay_;  // 叠加层
};

} // namespace ui
} // namespace videoeye
