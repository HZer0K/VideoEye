#include "VulkanVideoWidget.h"
#include "core/player/VulkanRenderer.h"
#include "utils/Logger.h"
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>

namespace videoeye {
namespace ui {

// ============================================================================
// VideoOverlayWidget
// ============================================================================
VideoOverlayWidget::VideoOverlayWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
}

void VideoOverlayWidget::SetOverlayInfo(const VideoOverlayInfo& info) {
    info_ = info;
    update();
}

void VideoOverlayWidget::SetCenterPlayButtonVisible(bool visible) {
    show_center_play_ = visible;
    update();
}

void VideoOverlayWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    const int margin = 14;
    const int badge_h = 26;
    const int badge_radius = 6;

    // === 顶部信息条 ===
    auto drawBadge = [&](const QString& text, int x, bool isStatus = false) {
        QFont font("Inter", 9);
        font.setBold(isStatus);
        painter.setFont(font);
        QFontMetrics fm(font);
        int text_w = fm.horizontalAdvance(text);
        int badge_w = text_w + 20;

        // 半透明黑底
        QColor bg = isStatus && info_.is_playing
            ? QColor(63, 185, 80, 60)    // 绿色底
            : QColor(0, 0, 0, 100);      // 黑色底
        painter.setPen(Qt::NoPen);
        painter.setBrush(bg);
        painter.drawRoundedRect(QRect(x, margin, badge_w, badge_h), badge_radius, badge_radius);

        // 文字
        QColor text_color = isStatus && info_.is_playing
            ? QColor(63, 185, 80)
            : QColor(240, 246, 252);
        painter.setPen(text_color);
        painter.drawText(QRect(x, margin, badge_w, badge_h), Qt::AlignCenter, text);

        return badge_w;
    };

    // 左侧: 分辨率 + 编码
    int left_x = margin;
    if (!info_.resolution.isEmpty()) {
        left_x += drawBadge(info_.resolution, left_x) + 6;
    }
    if (!info_.codec.isEmpty()) {
        left_x += drawBadge(info_.codec, left_x) + 6;
    }

    // 右侧: FPS + 状态
    int right_x = w - margin;
    if (!info_.status.isEmpty()) {
        QFont font("Inter", 9);
        font.setBold(true);
        painter.setFont(font);
        QFontMetrics fm(font);
        int text_w = fm.horizontalAdvance(info_.status);
        int badge_w = text_w + 20;
        right_x -= badge_w;
        drawBadge(info_.status, right_x, true);
        right_x -= 6;
    }
    if (!info_.fps.isEmpty()) {
        QFont font("Inter", 9);
        painter.setFont(font);
        QFontMetrics fm(font);
        int text_w = fm.horizontalAdvance(info_.fps);
        int badge_w = text_w + 20;
        right_x -= badge_w;
        drawBadge(info_.fps, right_x);
    }

    // === 中央播放按钮 ===
    if (show_center_play_ && !info_.is_playing && info_.has_video) {
        int btn_d = 72;
        int cx = w / 2;
        int cy = h / 2;
        int btn_x = cx - btn_d / 2;
        int btn_y = cy - btn_d / 2;

        // 半透明蓝色背景圆
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(88, 166, 255, 30));
        painter.drawEllipse(QPoint(cx, cy), btn_d / 2, btn_d / 2);

        // 蓝色边框圆
        painter.setPen(QPen(QColor(88, 166, 255, 80), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPoint(cx, cy), btn_d / 2, btn_d / 2);

        // 播放三角图标
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(88, 166, 255, 220));
        QPainterPath triangle;
        int tri_size = 22;
        triangle.moveTo(cx - tri_size / 3, cy - tri_size / 2);
        triangle.lineTo(cx - tri_size / 3, cy + tri_size / 2);
        triangle.lineTo(cx + tri_size * 2 / 3, cy);
        triangle.closeSubpath();
        painter.drawPath(triangle);
    }
}

// ============================================================================
// VulkanVideoWidget
// ============================================================================
VulkanVideoWidget::VulkanVideoWidget(QWidget* parent)
    : QWidget(parent)
    , overlay_(nullptr) {
    setAttribute(Qt::WA_NativeWindow, true);   // 确保有原生窗口句柄
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(320, 160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color: #01060E;");
    setAutoFillBackground(true);

    // 创建叠加层 (子 widget, 透明背景)
    overlay_ = new VideoOverlayWidget(this);
    overlay_->setGeometry(0, 0, width(), height());
    overlay_->raise();
    overlay_->show();
}

VulkanVideoWidget::~VulkanVideoWidget() {
    // 等待后台 Vulkan 初始化线程结束, 避免渲染器/上下文被析构时仍在使用
    if (init_thread_.joinable()) init_thread_.join();
}

void VulkanVideoWidget::SetVulkanRenderer(player::VulkanRenderer* renderer, player::VulkanContext* ctx) {
    renderer_ = renderer;
    vulkan_ctx_ = ctx;
    // 实际 Initialize 延迟到 showEvent (原生窗口句柄 winId() 就绪时)
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

void VulkanVideoWidget::SetOverlayInfo(const VideoOverlayInfo& info) {
    if (overlay_) {
        overlay_->SetOverlayInfo(info);
    }
}

void VulkanVideoWidget::SetCenterPlayButtonVisible(bool visible) {
    if (overlay_) {
        overlay_->SetCenterPlayButtonVisible(visible);
    }
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
        painter.fillRect(rect(), QColor("#01060E"));
        painter.setPen(QColor(0x8B, 0x94, 0x9E));
        QFont font;
        font.setPixelSize(std::max(14, std::min(height() / 30, 30)));
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, tr("视频显示区域"));
    } else {
        // 缩放并居中显示
        QSize scaled = fallback_image_.size().scaled(size(), Qt::KeepAspectRatio);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        painter.fillRect(rect(), QColor("#01060E"));
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
    // 叠加层跟随大小变化
    if (overlay_) {
        overlay_->setGeometry(0, 0, width(), height());
    }
}

void VulkanVideoWidget::TryInitializeVulkan() {
    if (vulkan_active_.load() || init_running_.load()) return;  // 已激活或进行中, 幂等
    // first_show_ 仅在真正尝试初始化后才置 false, 因此若此时窗口尺寸无效
    // (尺寸为 0, 例如布局尚未生效), 则不消费该次机会, 留待后续 showEvent/resize。
    if (first_show_ && renderer_ && vulkan_ctx_ && vulkan_ctx_->IsValid() &&
        width() > 0 && height() > 0) {
#ifdef HAVE_VULKAN
        // 重量级 Vulkan 初始化放在后台线程, 避免阻塞 GUI 线程导致窗口无法显示。
        // 无论成功/失败/异常, 主窗口都不会卡死; 失败自动回退 CPU 显示。
        init_running_.store(true);
        WId handle = winId();
        int w = width(), h = height();
        player::VulkanRenderer* r = renderer_;
        player::VulkanContext* c = vulkan_ctx_;
        QPointer<VulkanVideoWidget> self(this);
        init_thread_ = std::thread([self, r, c, handle, w, h]() {
            bool ok = false;
            std::string err;
            try {
                ok = r->Initialize(c, handle, w, h);
            } catch (const std::exception& e) {
                err = e.what();
            } catch (...) {
                err = "non-std exception";
            }
            // 切回 GUI 线程更新状态 (self 为 QPointer, 对象已销毁则自动跳过)
            QMetaObject::invokeMethod(self, [self, r, ok, err]() {
                if (!self) return;
                self->init_running_.store(false);
                if (ok && r->IsInitialized()) {
                    self->vulkan_active_.store(true);
                    LOG_INFO("VulkanVideoWidget: 渲染器初始化成功, GPU 直渲已启用");
                } else {
                    LOG_WARN("VulkanVideoWidget: 渲染器初始化失败, 回退到 CPU 显示"
                             + (err.empty() ? std::string() : (" (" + err + ")")));
                    self->vulkan_active_.store(false);
                }
                self->first_show_ = false;
            }, Qt::QueuedConnection);
        });
#else
        vulkan_active_.store(false);
        first_show_ = false;
#endif
    }
}

void VulkanVideoWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Vulkan 渲染器延迟到窗口首次显示后初始化（此时原生窗口句柄 winId() 已就绪）
    TryInitializeVulkan();
    if (overlay_) {
        overlay_->raise();
        overlay_->show();
    }
}

} // namespace ui
} // namespace videoeye
