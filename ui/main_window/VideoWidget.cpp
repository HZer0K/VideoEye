#include "VideoWidget.h"
#include "utils/Logger.h"
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <cmath>

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

void VideoOverlayWidget::SetMotionVectors(const model::MacroblockFrameAnalysis& analysis) {
    mv_analysis_ = analysis;
    if (mv_mode_ != MvOverlayMode::Off) {
        update();
    }
}

void VideoOverlayWidget::SetMvOverlayMode(MvOverlayMode mode) {
    mv_mode_ = mode;
    update();
}

QRectF VideoOverlayWidget::ComputeVideoDisplayRect() const {
    const int fw = mv_analysis_.frame_width;
    const int fh = mv_analysis_.frame_height;
    if (fw <= 0 || fh <= 0) return QRectF(0, 0, width(), height());

    // 保持宽高比居中
    QSize video_size(fw, fh);
    QSize scaled = video_size.scaled(size(), Qt::KeepAspectRatio);
    qreal x = (width() - scaled.width()) / 2.0;
    qreal y = (height() - scaled.height()) / 2.0;
    return QRectF(x, y, scaled.width(), scaled.height());
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

    // === 运动矢量叠加 ===
    if (mv_mode_ != MvOverlayMode::Off &&
        mv_analysis_.frame_width > 0 && mv_analysis_.frame_height > 0 &&
        mv_analysis_.has_motion_vectors && !mv_analysis_.motion_vectors.empty()) {
        QRectF display_rect = ComputeVideoDisplayRect();
        if (mv_mode_ == MvOverlayMode::Blocks || mv_mode_ == MvOverlayMode::ArrowsAndBlocks) {
            DrawMvBlocks(painter, display_rect);
        }
        if (mv_mode_ == MvOverlayMode::Arrows || mv_mode_ == MvOverlayMode::ArrowsAndBlocks) {
            DrawMvArrows(painter, display_rect);
        }
        DrawMvLegend(painter, display_rect);
    }
}

// MV 幅度 → 热力图颜色 (0=绿 → 中=黄 → 大=红)
static QColor MvMagnitudeToColor(double magnitude) {
    // 归一化到 0-1 (16 像素以上为最大)
    double t = std::min(1.0, magnitude / 16.0);
    // 绿 (0,255,100) → 黄 (255,220,0) → 红 (255,60,60)
    int r, g, b;
    if (t < 0.5) {
        double k = t * 2.0;
        r = static_cast<int>(0 + k * 255);
        g = static_cast<int>(255 - k * 35);
        b = static_cast<int>(100 - k * 100);
    } else {
        double k = (t - 0.5) * 2.0;
        r = 255;
        g = static_cast<int>(220 - k * 160);
        b = static_cast<int>(0 + k * 60);
    }
    return QColor(r, g, b);
}

void VideoOverlayWidget::DrawMvArrows(QPainter& painter, const QRectF& display_rect) {
    const auto& mvs = mv_analysis_.motion_vectors;
    const int fw = mv_analysis_.frame_width;
    const int fh = mv_analysis_.frame_height;
    if (fw <= 0 || fh <= 0) return;

    const qreal sx = display_rect.width() / fw;
    const qreal sy = display_rect.height() / fh;

    // 运动矢量幅度放大因子 (子像素运动在屏幕上几乎不可见)
    const qreal mv_amp = 3.0;
    // 最小箭头长度 (像素), 幅度极小时也显示一个小点
    const qreal min_arrow = 2.0;

    painter.save();
    painter.setClipRect(display_rect);

    for (const auto& mv : mvs) {
        double mv_scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
        double px_mx = mv.motion_x / mv_scale;
        double px_my = mv.motion_y / mv_scale;
        double magnitude = std::sqrt(px_mx * px_mx + px_my * px_my);

        // 跳过零运动矢量 (帧内预测块)
        if (magnitude < 0.1) continue;

        qreal cx = display_rect.left() + (mv.block_x + mv.block_w / 2.0) * sx;
        qreal cy = display_rect.top() + (mv.block_y + mv.block_h / 2.0) * sy;
        qreal ex = cx + px_mx * sx * mv_amp;
        qreal ey = cy + px_my * sy * mv_amp;

        // 限制箭头最大长度, 避免过长的箭头遮挡画面
        qreal arrow_len = std::sqrt((ex - cx) * (ex - cx) + (ey - cy) * (ey - cy));
        const qreal max_arrow = 60.0;
        if (arrow_len > max_arrow) {
            qreal ratio = max_arrow / arrow_len;
            ex = cx + (ex - cx) * ratio;
            ey = cy + (ey - cy) * ratio;
            arrow_len = max_arrow;
        }
        if (arrow_len < min_arrow) {
            // 幅度极小: 画一个小圆点
            QColor dot_color = (mv.source < 0) ? QColor(255, 107, 107, 120)
                                                : QColor(88, 166, 255, 120);
            painter.setPen(Qt::NoPen);
            painter.setBrush(dot_color);
            painter.drawEllipse(QPointF(cx, cy), 1.5, 1.5);
            continue;
        }

        // 颜色: 前向=红, 后向=蓝, 双向=紫
        QColor color;
        if (mv.source < 0) {
            color = QColor(255, 107, 107);   // 前向 — 红
        } else if (mv.source > 0) {
            color = QColor(88, 166, 255);    // 后向 — 蓝
        } else {
            color = QColor(196, 132, 252);   // 双向/其他 — 紫
        }
        // 幅度越大越亮
        double brightness = std::min(1.0, magnitude / 16.0);
        color.setAlphaF(0.4 + 0.6 * brightness);

        // 箭头线
        QPen pen(color, 1.5);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(QPointF(cx, cy), QPointF(ex, ey));

        // 箭头头部
        double angle = std::atan2(ey - cy, ex - cx);
        double head_len = std::min(5.0, arrow_len * 0.3);
        QPointF p1(ex - head_len * std::cos(angle - 0.5),
                   ey - head_len * std::sin(angle - 0.5));
        QPointF p2(ex - head_len * std::cos(angle + 0.5),
                   ey - head_len * std::sin(angle + 0.5));
        painter.drawLine(QPointF(ex, ey), p1);
        painter.drawLine(QPointF(ex, ey), p2);
    }

    painter.restore();
}

void VideoOverlayWidget::DrawMvBlocks(QPainter& painter, const QRectF& display_rect) {
    const auto& mvs = mv_analysis_.motion_vectors;
    const int fw = mv_analysis_.frame_width;
    const int fh = mv_analysis_.frame_height;
    if (fw <= 0 || fh <= 0) return;

    const qreal sx = display_rect.width() / fw;
    const qreal sy = display_rect.height() / fh;

    painter.save();
    painter.setClipRect(display_rect);

    for (const auto& mv : mvs) {
        double mv_scale = (mv.motion_scale > 0) ? static_cast<double>(mv.motion_scale) : 1.0;
        double px_mx = mv.motion_x / mv_scale;
        double px_my = mv.motion_y / mv_scale;
        double magnitude = std::sqrt(px_mx * px_mx + px_my * px_my);

        // 跳过零运动矢量
        if (magnitude < 0.1) continue;

        qreal bx = display_rect.left() + mv.block_x * sx;
        qreal by = display_rect.top() + mv.block_y * sy;
        qreal bw = mv.block_w * sx;
        qreal bh = mv.block_h * sy;

        QColor color = MvMagnitudeToColor(magnitude);
        color.setAlphaF(0.25 + 0.35 * std::min(1.0, magnitude / 16.0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRect(QRectF(bx, by, bw, bh));
    }

    painter.restore();
}

void VideoOverlayWidget::DrawMvLegend(QPainter& painter, const QRectF& display_rect) {
    const int legend_w = 140;
    const int legend_h = 56;
    const int margin = 14;
    int lx = static_cast<int>(display_rect.left()) + margin;
    int ly = static_cast<int>(display_rect.bottom()) - legend_h - margin;

    // 背景框
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 160));
    painter.drawRoundedRect(QRect(lx, ly, legend_w, legend_h), 6, 6);

    QFont font("Inter", 8);
    painter.setFont(font);
    QFontMetrics fm(font);

    int y = ly + 8;
    int x = lx + 10;

    // 前向/后向图例
    painter.setPen(QPen(QColor(255, 107, 107), 2));
    painter.drawLine(x, y + 6, x + 16, y + 6);
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(x + 22, y + 10, tr("前向"));

    painter.setPen(QPen(QColor(88, 166, 255), 2));
    painter.drawLine(x + 58, y + 6, x + 74, y + 6);
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(x + 80, y + 10, tr("后向"));

    // 幅度图例
    y += 18;
    QString mv_info = tr("MV: %1 块 | 均: %2 | 峰: %3")
        .arg(mv_analysis_.stats.total_blocks)
        .arg(mv_analysis_.stats.avg_motion_magnitude, 0, 'f', 1)
        .arg(mv_analysis_.stats.max_motion_magnitude, 0, 'f', 1);
    painter.setPen(QColor(180, 180, 180));
    painter.drawText(x, y + 10, mv_info);

    // 幅度色条
    y += 18;
    int bar_w = legend_w - 20;
    for (int i = 0; i < bar_w; ++i) {
        double t = static_cast<double>(i) / bar_w;
        QColor c = MvMagnitudeToColor(t * 16.0);
        painter.setPen(c);
        painter.drawPoint(x + i, y + 4);
    }
    painter.setPen(QColor(160, 160, 160));
    painter.drawText(x, y + 14, "0");
    painter.drawText(x + bar_w - fm.horizontalAdvance("16"), y + 14, "16px");
}

// ============================================================================
// VideoWidget
// ============================================================================
VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent) {
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

VideoWidget::~VideoWidget() = default;

void VideoWidget::SetFrame(const QImage& image) {
    frame_ = image;
    update();
}

void VideoWidget::Clear() {
    frame_ = QImage();
    update();
}

void VideoWidget::SetOverlayInfo(const VideoOverlayInfo& info) {
    if (overlay_) overlay_->SetOverlayInfo(info);
}

void VideoWidget::SetCenterPlayButtonVisible(bool visible) {
    if (overlay_) overlay_->SetCenterPlayButtonVisible(visible);
}

void VideoWidget::SetMotionVectors(const model::MacroblockFrameAnalysis& analysis) {
    if (overlay_) overlay_->SetMotionVectors(analysis);
}

void VideoWidget::SetMvOverlayMode(MvOverlayMode mode) {
    if (overlay_) overlay_->SetMvOverlayMode(mode);
}

void VideoWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (frame_.isNull()) {
        painter.fillRect(rect(), QColor("#01060E"));
        painter.setPen(QColor(0x8B, 0x94, 0x9E));
        QFont font;
        font.setPixelSize(std::max(14, std::min(height() / 30, 30)));
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, tr("视频显示区域"));
        return;
    }

    // 缩放并居中显示
    const QSize scaled = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    const int x = (width() - scaled.width()) / 2;
    const int y = (height() - scaled.height()) / 2;
    painter.fillRect(rect(), QColor("#01060E"));
    painter.drawImage(QRect(x, y, scaled.width(), scaled.height()), frame_);
}

void VideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (overlay_) overlay_->setGeometry(0, 0, width(), height());
}

void VideoWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (overlay_) {
        overlay_->raise();
        overlay_->show();
    }
}

} // namespace ui
} // namespace videoeye
