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
#include <QWheelEvent>
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

// 缩放倍率范围（相对"适应窗口"）。下限放宽到 1.0 以下是为了 1:1 查看小图：
// 一张 640x480 的图在 1920 宽的控件里，"适应窗口"会把它放大 4 倍，而像素级
// 核对恰恰需要不放大 —— 那时 1:1 的倍率是 0.25。上限按"最多放大 32 倍"封顶，
// 再大也看不出更多信息，只会让绘制区域失控。
constexpr double kZoomMin = 0.05;
constexpr double kZoomMax = 32.0;
constexpr double kZoomStep = 1.25;

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
    // 画面上有缩放/平移时（图片查看、像素级核对），几何由 VideoWidget 下发：
    // 叠加层自己按 KeepAspectRatio 反算只会得到"未缩放"的矩形，箭头会整体跑偏。
    if (display_rect_override_valid_) {
        return display_rect_override_;
    }
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

void VideoOverlayWidget::SetDisplayRectOverride(bool valid, const QRectF& rect) {
    if (display_rect_override_valid_ == valid && display_rect_override_ == rect) {
        return;
    }
    display_rect_override_valid_ = valid;
    display_rect_override_ = rect;
    update();
}

void VideoOverlayWidget::ClearDisplayRectOverride() {
    SetDisplayRectOverride(false, QRectF());
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
    setAccessibleDescription(tr("空格 播放/暂停；←/→ 快退/快进 5 秒；Shift+←/→ 逐帧；↑/↓ 音量；M 静音；"
                                "滚轮 缩放；拖动 平移；双击 在适应窗口与 1:1 之间切换；Ctrl+0 适应窗口；Ctrl+1 1:1"));
    // 鼠标跟踪只为"悬停时显示可拖动光标"，按下/拖动不依赖它。
    setMouseTracking(true);

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
    // 播放中左键拖动的语义仍是"点画面收回焦点"；只有画面确实被放大到超出控件
    // （还有看不到的部分）时才接管成平移，否则拖了也只会顶到边上、像卡住了。
    if (event->button() == Qt::LeftButton && !frame_.isNull() && CanPan()) {
        panning_ = true;
        pan_anchor_ = event->pos();
        UpdateCursorShape();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!panning_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint delta = event->pos() - pan_anchor_;
    pan_anchor_ = event->pos();
    if (!delta.isNull()) {
        pan_ += QPointF(delta);
        ClampPan();
        scaled_valid_ = false;
        SyncOverlayDisplayRect();
        update();
    }
    event->accept();
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_ && event->button() == Qt::LeftButton) {
        panning_ = false;
        UpdateCursorShape();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !frame_.isNull()) {
        // 双击在"适应窗口"与"1:1 像素"之间来回：这是看图时最常用的两个视角，
        // 比让用户自己去滚轮凑倍率快得多。
        if (IsFitMode()) {
            SetActualSize();
        } else {
            ResetZoom();
        }
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VideoWidget::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    if (frame_.isNull() || delta == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    // 以光标为锚点缩放：指向哪个像素，缩放后它还应该停在光标下。否则放大后画面
    // 会从中心"漂走"，要连点十几次滚轮才能找回刚才看的那块。
    const QRectF before = DisplayRect();
    const QPointF pos = event->position();
    const double nx = before.width() > 0 ? (pos.x() - before.left()) / before.width() : 0.5;
    const double ny = before.height() > 0 ? (pos.y() - before.top()) / before.height() : 0.5;

    const double next_zoom = std::clamp(zoom_ * (delta > 0 ? kZoomStep : 1.0 / kZoomStep), kZoomMin, kZoomMax);
    if (qFuzzyCompare(next_zoom, zoom_)) {
        event->accept();
        return;
    }
    zoom_ = next_zoom;
    // 反解平移：让 (nx, ny) 这个归一化画面点仍落在 pos 上。
    const QRectF unscaled = DisplayRect(); // 此时 pan_ 还是旧值，先拿尺寸
    const QPointF center(width() / 2.0, height() / 2.0);
    pan_ = pos - center + QPointF(unscaled.width(), unscaled.height()) / 2.0 -
           QPointF(nx * unscaled.width(), ny * unscaled.height());
    ClampPan();
    scaled_valid_ = false;
    UpdateCursorShape();
    SyncOverlayDisplayRect();
    update();
    emit ZoomChanged(zoom_);
    event->accept();
}

void VideoWidget::keyPressEvent(QKeyEvent* event) {
    // 缩放快捷键先处理：Ctrl+0/1 与 Ctrl+=/- 都是常见看图约定，且不与播放控制冲突。
    if (event->modifiers() & Qt::ControlModifier) {
        switch (event->key()) {
        case Qt::Key_0:
            ResetZoom();
            return;
        case Qt::Key_1:
            SetActualSize();
            return;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            ZoomIn();
            return;
        case Qt::Key_Minus:
            ZoomOut();
            return;
        default:
            break;
        }
    }
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
    const bool size_changed = frame_.size() != image.size();
    frame_ = image;
    scaled_valid_ = false;
    if (size_changed) {
        // 换了一幅不同尺寸的画面（换文件 / 序列翻页）：旧平移量是按旧尺寸算的，
        // 不钳制的话画面会以"边缘露底"的姿态出现。缩放倍率保留 —— 用户刚调到
        // 1:1 看像素，翻到下一张还想按同样倍率看。
        ClampPan();
        UpdateCursorShape();
    }
    SyncOverlayDisplayRect();
    update();
}

void VideoWidget::Clear() {
    frame_ = QImage();
    scaled_frame_ = QImage();
    scaled_rect_ = QRect();
    scaled_valid_ = false;
    // 清屏即退出"看图"状态：倍率/平移留着没有意义，而且下次打开会以放大状态
    // 出现，用户会以为画面被裁了。
    zoom_ = 1.0;
    pan_ = QPointF();
    panning_ = false;
    UpdateCursorShape();
    if (overlay_)
        overlay_->ClearDisplayRectOverride();
    last_synced_display_rect_ = QRectF();
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

// ============================================================================
// 缩放 / 平移
// ============================================================================
QRectF VideoWidget::DisplayRect() const {
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        return QRectF();
    }
    const double fit =
        std::min(static_cast<double>(width()) / frame_.width(), static_cast<double>(height()) / frame_.height());
    const double scale = fit * zoom_;
    const QSizeF size(frame_.width() * scale, frame_.height() * scale);
    const QPointF center(width() / 2.0, height() / 2.0);
    return QRectF(center + pan_ - QPointF(size.width() / 2.0, size.height() / 2.0), size);
}

bool VideoWidget::IsFitMode() const {
    return qFuzzyCompare(zoom_, 1.0) && pan_.isNull();
}

bool VideoWidget::CanPan() const {
    const QRectF r = DisplayRect();
    return r.width() > width() + 0.5 || r.height() > height() + 0.5;
}

void VideoWidget::SetZoom(double factor) {
    ApplyZoom(std::clamp(factor, kZoomMin, kZoomMax));
}

void VideoWidget::ZoomIn() {
    ApplyZoom(std::clamp(zoom_ * kZoomStep, kZoomMin, kZoomMax));
}

void VideoWidget::ZoomOut() {
    ApplyZoom(std::clamp(zoom_ / kZoomStep, kZoomMin, kZoomMax));
}

void VideoWidget::SetActualSize() {
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        return;
    }
    // 一个源像素对一个屏幕像素：适应倍率是 fit，"1:1" 就是 1/fit 倍。
    const double fit =
        std::min(static_cast<double>(width()) / frame_.width(), static_cast<double>(height()) / frame_.height());
    if (fit <= 0.0) {
        return;
    }
    pan_ = QPointF();
    ApplyZoom(std::clamp(1.0 / fit, kZoomMin, kZoomMax));
}

void VideoWidget::ResetZoom() {
    if (IsFitMode()) {
        return; // 已经是适应窗口且没平移：不必让整帧重新缩放一次
    }
    pan_ = QPointF();
    ApplyZoom(1.0);
}

void VideoWidget::ApplyZoom(double factor) {
    // 不做"值没变就早退"的判断：调用方（ResetZoom / SetActualSize）可能已经改过 pan_，
    // 早退会把"平移归零"这件事一起吞掉，画面停在旧偏移上直到下一次重绘。
    zoom_ = factor;
    // 缩回"整幅可见"时平移量必须归零，否则画面会被旧的偏移推到一边（看起来像裁切）。
    ClampPan();
    scaled_valid_ = false;
    UpdateCursorShape();
    SyncOverlayDisplayRect();
    update();
    emit ZoomChanged(zoom_);
}

void VideoWidget::ClampPan() {
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        pan_ = QPointF();
        return;
    }
    const QRectF r = DisplayRect();
    const double max_dx = std::max(0.0, (r.width() - width()) / 2.0);
    const double max_dy = std::max(0.0, (r.height() - height()) / 2.0);
    pan_.setX(std::clamp(pan_.x(), -max_dx, max_dx));
    pan_.setY(std::clamp(pan_.y(), -max_dy, max_dy));
}

void VideoWidget::UpdateCursorShape() {
    if (panning_) {
        setCursor(Qt::ClosedHandCursor);
    } else if (CanPan()) {
        setCursor(Qt::OpenHandCursor);
    } else {
        unsetCursor();
    }
}

void VideoWidget::SyncOverlayDisplayRect() {
    if (!overlay_ || frame_.isNull()) {
        return;
    }
    const QRectF rect = DisplayRect();
    if (rect == last_synced_display_rect_) {
        return;
    }
    last_synced_display_rect_ = rect;
    // 叠加层只在"有覆盖说明"时才需要被推矩形；适应窗口模式下它的默认推算与这里
    // 的结果完全一致，仍然下发（浮点误差内一致，不会额外重绘）。
    overlay_->SetDisplayRectOverride(true, rect);
}

void VideoWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);

    QPainter painter(this);

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

    painter.fillRect(rect(), QColor("#01060E"));

    if (IsFitMode()) {
        // 适应窗口：缩放结果缓存在 scaled_frame_ 里，重复重绘不再重算
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        EnsureScaledFrame();
        if (!scaled_frame_.isNull()) {
            painter.drawImage(scaled_rect_.topLeft(), scaled_frame_);
        }
        return;
    }

    // 缩放/平移：只把真正落在控件里的那一块源像素放大到控件上。
    // 不先渲染整幅大图再裁 —— 32 倍的 4K 画面是几十 GB 的中间位图，必然 OOM。
    const QRectF target = DisplayRect();
    const QRectF visible = target.intersected(QRectF(rect()));
    if (visible.isEmpty() || target.width() <= 0 || target.height() <= 0) {
        return;
    }
    const double sx = frame_.width() / target.width();
    const double sy = frame_.height() / target.height();
    QRectF source((visible.left() - target.left()) * sx, (visible.top() - target.top()) * sy, visible.width() * sx,
                  visible.height() * sy);
    // 取整误差可能让源矩形比画面多出零点几个像素，Qt 会补浅色边；钳一下。
    source = source.intersected(QRectF(0, 0, frame_.width(), frame_.height()));
    if (source.isEmpty()) {
        return;
    }
    // 放大时用最近邻：像素级核对要看到"这一个像素"，插值会把边界糊掉。
    // 缩小时仍走平滑，否则缩小后的画面全是锯齿。
    painter.setRenderHint(QPainter::SmoothPixmapTransform, sx > 1.0 || sy > 1.0);
    painter.drawImage(visible, frame_, source);
}

void VideoWidget::EnsureScaledFrame() {
    if (scaled_valid_) {
        return;
    }
    scaled_valid_ = true;
    scaled_frame_ = QImage();
    scaled_rect_ = QRect();
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        return;
    }
    const QSize target = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    if (target.isEmpty()) {
        return;
    }
    // 显示尺寸与帧尺寸一致时直接引用原帧（QImage 隐式共享，不复制像素）。
    scaled_frame_ =
        (target == frame_.size()) ? frame_ : frame_.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    scaled_rect_ =
        QRect((width() - target.width()) / 2, (height() - target.height()) / 2, target.width(), target.height());
}

void VideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    scaled_valid_ = false;
    if (overlay_)
        overlay_->setGeometry(0, 0, width(), height());
    // 控件尺寸变了，"适应"的基准也变了：平移量按新的可见范围重新钳制，
    // 否则从大窗口缩小后画面会被推出边界，露出一片背景色。
    ClampPan();
    UpdateCursorShape();
    SyncOverlayDisplayRect();
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
