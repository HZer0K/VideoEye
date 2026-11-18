#include "ui/main_window/VideoWidget.h"
#include "infrastructure/logging/Logger.h"
#include "ui/main_window/MotionVectorGeometry.h"
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace videoeye {
namespace ui {

namespace {

// 占位文字为中央播放圆盘让出的纵向距离（圆盘半径 36 + 文字半高 + 间隙）
constexpr int kCenterPlayDodge = 56;

// 叠加层一帧最多画多少条矢量。大分辨率一帧可能有几千条分区，10Hz 全量重绘会拖慢
// 每帧绘制；按记录顺序等距抽样，空间分布基本保持不变。
constexpr std::size_t kMaxOverlayVectors = 1500;

} // namespace

// ============================================================================
// VideoOverlayWidget
// ============================================================================
VideoOverlayWidget::VideoOverlayWidget(QWidget* parent) : QWidget(parent) {
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

bool VideoOverlayWidget::IsCenterPlayVisible() const {
    return show_center_play_ && !info_.is_playing && info_.has_video;
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
    if (fw <= 0 || fh <= 0)
        return QRectF(0, 0, width(), height());

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
        QColor bg = isStatus && info_.is_playing ? QColor(63, 185, 80, 60) // 绿色底
                                                 : QColor(0, 0, 0, 100);   // 黑色底
        painter.setPen(Qt::NoPen);
        painter.setBrush(bg);
        painter.drawRoundedRect(QRect(x, margin, badge_w, badge_h), badge_radius, badge_radius);

        // 文字
        QColor text_color = isStatus && info_.is_playing ? QColor(63, 185, 80) : QColor(240, 246, 252);
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
    if (IsCenterPlayVisible()) {
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
    if (mv_mode_ != MvOverlayMode::Off && mv_analysis_.frame_width > 0 && mv_analysis_.frame_height > 0 &&
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
    if (fw <= 0 || fh <= 0)
        return;

    const MvOverlayViewport viewport{display_rect.left(), display_rect.top(), display_rect.width(),
                                     display_rect.height()};

    // 运动矢量幅度放大因子 (子像素运动在屏幕上几乎不可见)
    const qreal mv_amp = 3.0;
    // 最小箭头长度 (像素), 幅度极小时也显示一个小点
    const qreal min_arrow = 2.0;

    painter.save();
    painter.setClipRect(display_rect);

    const std::size_t stride = MvSampleStride(mvs.size(), kMaxOverlayVectors);
    for (std::size_t i = 0; i < mvs.size(); i += stride) {
        const auto& mv = mvs[i];
        const double magnitude = MvPixelMagnitude(mv);

        // 跳过零运动矢量 (帧内预测块)
        if (magnitude < 0.1)
            continue;

        // dst_x/dst_y 是块中心（FFmpeg AVMotionVector 语义），箭头直接从中心出发。
        const MvArrowGeometry geo = ComputeMvArrow(mv, viewport, fw, fh, mv_amp, 60.0);
        const double cx = geo.start_x;
        const double cy = geo.start_y;
        const double ex = geo.end_x;
        const double ey = geo.end_y;

        if (geo.length < min_arrow) {
            // 幅度极小: 画一个小圆点
            QColor dot_color = (mv.source < 0) ? QColor(255, 107, 107, 120) : QColor(88, 166, 255, 120);
            painter.setPen(Qt::NoPen);
            painter.setBrush(dot_color);
            painter.drawEllipse(QPointF(cx, cy), 1.5, 1.5);
            continue;
        }

        // 颜色: 前向=红, 后向=蓝, 双向=紫
        QColor color;
        if (mv.source < 0) {
            color = QColor(255, 107, 107); // 前向 — 红
        } else if (mv.source > 0) {
            color = QColor(88, 166, 255); // 后向 — 蓝
        } else {
            color = QColor(196, 132, 252); // 双向/其他 — 紫
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
        double head_len = std::min(5.0, geo.length * 0.3);
        QPointF p1(ex - head_len * std::cos(angle - 0.5), ey - head_len * std::sin(angle - 0.5));
        QPointF p2(ex - head_len * std::cos(angle + 0.5), ey - head_len * std::sin(angle + 0.5));
        painter.drawLine(QPointF(ex, ey), p1);
        painter.drawLine(QPointF(ex, ey), p2);
    }

    painter.restore();
}

void VideoOverlayWidget::DrawMvBlocks(QPainter& painter, const QRectF& display_rect) {
    const auto& mvs = mv_analysis_.motion_vectors;
    const int fw = mv_analysis_.frame_width;
    const int fh = mv_analysis_.frame_height;
    if (fw <= 0 || fh <= 0)
        return;

    const MvOverlayViewport viewport{display_rect.left(), display_rect.top(), display_rect.width(),
                                     display_rect.height()};

    painter.save();
    painter.setClipRect(display_rect);

    const std::size_t stride = MvSampleStride(mvs.size(), kMaxOverlayVectors);
    for (std::size_t i = 0; i < mvs.size(); i += stride) {
        const auto& mv = mvs[i];
        const double magnitude = MvPixelMagnitude(mv);

        // 跳过零运动矢量
        if (magnitude < 0.1)
            continue;

        // dst 是块中心，热力图矩形要以它为中心向四周展开半个块，否则整体偏移半块。
        const MvBlockGeometry block = ComputeMvBlock(mv, viewport, fw, fh);

        QColor color = MvMagnitudeToColor(magnitude);
        color.setAlphaF(0.25 + 0.35 * std::min(1.0, magnitude / 16.0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRect(QRectF(block.left, block.top, block.width, block.height));
    }

    painter.restore();
}

void VideoOverlayWidget::DrawMvLegend(QPainter& painter, const QRectF& display_rect) {
    QFont font("Inter", 8);
    QFontMetrics fm(font);

    const std::size_t total = mv_analysis_.motion_vectors.size();
    const std::size_t stride = MvSampleStride(total, kMaxOverlayVectors);
    QString mv_info = tr("MV: %1 条 | 均: %2 | 峰: %3")
                          .arg(static_cast<qulonglong>(total))
                          .arg(mv_analysis_.stats.avg_motion_magnitude, 0, 'f', 1)
                          .arg(mv_analysis_.stats.max_motion_magnitude, 0, 'f', 1);
    if (stride > 1) {
        // 抽样必须显式标注：图例上的条数只统计已采集的矢量，画出来的箭头更少。
        const std::size_t drawn = (total + stride - 1) / stride;
        mv_info += tr(" | 抽样 %1/%2").arg(static_cast<qulonglong>(drawn)).arg(static_cast<qulonglong>(total));
    }

    const int legend_h = 56;
    const int margin = 14;
    // 宽度按文字实测：固定 140 连「MV: 2000 条 | 均: 2.0 | 峰: 9.0」都放不下，会溢出圆角框。
    const int legend_w = std::max(140, fm.horizontalAdvance(mv_info) + 20);
    int lx = static_cast<int>(display_rect.left()) + margin;
    int ly = static_cast<int>(display_rect.bottom()) - legend_h - margin;

    // 背景框
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 160));
    painter.drawRoundedRect(QRect(lx, ly, legend_w, legend_h), 6, 6);

    painter.setFont(font);

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
VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(320, 160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color: #01060E;");
    setAutoFillBackground(true);
    // 播放器快捷键（空格/方向键/M）只在画面获得焦点时生效，避免抢分析表格的按键。
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("视频画面"));
    setAccessibleDescription(tr("空格 播放/暂停；←/→ 快退/快进 5 秒；Shift+←/→ 逐帧；↑/↓ 音量；M 静音"));

    // 创建叠加层 (子 widget, 透明背景)
    overlay_ = new VideoOverlayWidget(this);
    overlay_->setGeometry(0, 0, width(), height());
    overlay_->raise();
    overlay_->show();
}

VideoWidget::~VideoWidget() = default;

void VideoWidget::mousePressEvent(QMouseEvent* event) {
    // 点一下画面就把焦点收过来，随后空格/方向键才作用于播放器。
    setFocus(Qt::MouseFocusReason);
    QWidget::mousePressEvent(event);
}

void VideoWidget::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Space:
        emit TogglePlayRequested();
        return;
    case Qt::Key_Left:
        if (event->modifiers() & Qt::ShiftModifier)
            emit StepFrameRequested(-1);
        else
            emit SeekRelativeRequested(-5000);
        return;
    case Qt::Key_Right:
        if (event->modifiers() & Qt::ShiftModifier)
            emit StepFrameRequested(+1);
        else
            emit SeekRelativeRequested(5000);
        return;
    case Qt::Key_Up:
        emit VolumeDeltaRequested(5);
        return;
    case Qt::Key_Down:
        emit VolumeDeltaRequested(-5);
        return;
    case Qt::Key_M:
        if (event->modifiers() == Qt::NoModifier) {
            emit MuteToggleRequested();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void VideoWidget::SetFrame(const QImage& image) {
    frame_ = image;
    update();
}

void VideoWidget::Clear() {
    frame_ = QImage();
    update();
}

void VideoWidget::SetOverlayInfo(const VideoOverlayInfo& info) {
    if (overlay_)
        overlay_->SetOverlayInfo(info);
}

void VideoWidget::SetCenterPlayButtonVisible(bool visible) {
    if (overlay_)
        overlay_->SetCenterPlayButtonVisible(visible);
}

void VideoWidget::SetMotionVectors(const model::MacroblockFrameAnalysis& analysis) {
    if (overlay_)
        overlay_->SetMotionVectors(analysis);
}

void VideoWidget::SetMvOverlayMode(MvOverlayMode mode) {
    if (overlay_)
        overlay_->SetMvOverlayMode(mode);
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
        // 中央播放按钮（直径 72）画在同一中心，占位文字要让开，否则字被三角压住
        QRect text_rect = rect();
        if (overlay_ && overlay_->IsCenterPlayVisible())
            text_rect.translate(0, -kCenterPlayDodge);
        painter.drawText(text_rect, Qt::AlignCenter, tr("视频显示区域"));
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
    if (overlay_)
        overlay_->setGeometry(0, 0, width(), height());
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
