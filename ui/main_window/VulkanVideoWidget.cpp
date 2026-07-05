#include "VulkanVideoWidget.h"
#include "core/player/VulkanRenderer.h"
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

namespace videoeye {
namespace ui {

VulkanVideoWidget::VulkanVideoWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow, true);   // 确保有原生窗口句柄
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(320, 160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color: black;");
    setAutoFillBackground(true);
}

VulkanVideoWidget::~VulkanVideoWidget() {
}

void VulkanVideoWidget::SetVulkanRenderer(player::VulkanRenderer* renderer) {
    renderer_ = renderer;
#ifdef HAVE_VULKAN
    if (renderer_ && isVisible() && first_show_) {
        if (renderer_->Initialize(nullptr, winId(), width(), height())) {
            vulkan_active_ = true;
        }
        first_show_ = false;
    }
#else
    vulkan_active_ = false;
#endif
}

void VulkanVideoWidget::SetFallbackImage(const QImage& image) {
    if (!vulkan_active_) {
        fallback_image_ = image;
        update();
    }
}

void VulkanVideoWidget::Clear() {
    fallback_image_ = QImage();
    update();
}

void VulkanVideoWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    if (vulkan_active_) {
        // Vulkan 直接渲染到窗口表面，Qt 不绘制
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (fallback_image_.isNull()) {
        painter.fillRect(rect(), Qt::black);
        painter.setPen(Qt::white);
        QFont font;
        font.setPixelSize(std::max(14, std::min(height() / 30, 30)));
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, tr("视频显示区域"));
    } else {
        // 缩放并居中显示
        QSize scaled = fallback_image_.size().scaled(size(), Qt::KeepAspectRatio);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        painter.drawImage(QRect(x, y, scaled.width(), scaled.height()), fallback_image_);
    }
}

void VulkanVideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (renderer_ && vulkan_active_) {
#ifdef HAVE_VULKAN
        renderer_->Resize(event->size().width(), event->size().height());
#endif
    }
}

void VulkanVideoWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Vulkan 渲染器延迟到窗口显示后初始化（此时原生句柄已就绪）
    if (first_show_ && renderer_) {
#ifdef HAVE_VULKAN
        // Note: VulkanContext must be set up externally before calling Initialize
        // The actual initialization happens when MainWindow sets up the renderer
#endif
        first_show_ = false;
    }
}

} // namespace ui
} // namespace videoeye
