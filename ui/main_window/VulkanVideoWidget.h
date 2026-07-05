#pragma once

#include <QWidget>
#include <QImage>

namespace videoeye {
namespace player { class VulkanRenderer; }

namespace ui {

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

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    player::VulkanRenderer* renderer_ = nullptr;
    QImage fallback_image_;
    bool vulkan_active_ = false;
    bool first_show_ = true;
};

} // namespace ui
} // namespace videoeye
