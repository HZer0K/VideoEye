#include "ui/charts/MetricChartWidget.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace videoeye {
namespace ui {

namespace {

constexpr int kTitleHeight = 20;
constexpr int kLegendRowHeight = 16;
constexpr int kLeftMargin = 56;
constexpr int kRightMargin = 16;
constexpr int kTopMargin = 6;
constexpr int kBottomMargin = 22;
constexpr int kAxisTitleExtra = 16;
constexpr int kHitRadiusPx = 8;
constexpr int kMaxXTicks = 6;

// 支持 "%d" / "%.1f" 等 printf 风格格式；%d 时按整型取值，避免 double 传入 %d 的未定义行为。
QString FormatTick(double value, const QString& format) {
    if (format.isEmpty()) {
        return QString::number(value, 'g', 4);
    }
    const QByteArray fmt = format.toUtf8();
    if (format.contains(QLatin1Char('d')) || format.contains(QLatin1Char('i'))) {
        return QString::asprintf(fmt.constData(), static_cast<long long>(std::llround(value)));
    }
    return QString::asprintf(fmt.constData(), value);
}

bool FinitePoint(const QPointF& p) {
    return std::isfinite(p.x()) && std::isfinite(p.y());
}

}  // namespace

MetricChartWidget::MetricChartWidget(QWidget* parent)
    : QWidget(parent)
    , axis_x_(std::make_unique<ChartAxis>())
    , axis_y_(std::make_unique<ChartAxis>())
    , axis_y2_(std::make_unique<ChartAxis>()) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void MetricChartWidget::SetTitle(const QString& title) {
    title_ = title;
    update();
}

void MetricChartWidget::SetLegendVisible(bool visible) {
    legend_visible_ = visible;
    update();
}

ChartSeries* MetricChartWidget::AddSeries(const QString& name, ChartSeriesKind kind, const QColor& color) {
    series_.push_back(std::make_unique<ChartSeries>(kind, name, color));
    update();
    return series_.back().get();
}

ChartSeries* MetricChartWidget::AddLineSeries(const QString& name, const QColor& color) {
    return AddSeries(name, ChartSeriesKind::Line, color);
}

ChartSeries* MetricChartWidget::AddScatterSeries(const QString& name, const QColor& color) {
    return AddSeries(name, ChartSeriesKind::Scatter, color);
}

ChartSeries* MetricChartWidget::AddBarSeries(const QString& name, const QColor& color) {
    return AddSeries(name, ChartSeriesKind::Bar, color);
}

void MetricChartWidget::ClearSeriesData() {
    for (auto& s : series_) {
        s->Clear();
    }
    hover_series_ = -1;
    hover_index_ = -1;
    update();
}

void MetricChartWidget::SetAxisY2Visible(bool visible) {
    axis_y2_visible_ = visible;
    update();
}

void MetricChartWidget::AttachAxis(ChartSeries* series, ChartAxis* axis) {
    if (!series || !axis) return;
    // X 轴全图共用，只需要区分左右两个 Y 轴
    if (axis == axis_y2_.get()) {
        series->SetValueAxisIndex(1);
    } else if (axis == axis_y_.get()) {
        series->SetValueAxisIndex(0);
    }
}

QSize MetricChartWidget::sizeHint() const { return QSize(360, 220); }
QSize MetricChartWidget::minimumSizeHint() const { return QSize(120, 80); }

int MetricChartWidget::LegendHeight(int width) const {
    if (!legend_visible_) return 0;
    QFontMetrics fm(font());
    int x = kLeftMargin;
    int rows = 0;
    for (const auto& s : series_) {
        if (s->Name().isEmpty()) continue;
        const int w = 16 + fm.horizontalAdvance(s->Name()) + 12;
        if (rows == 0) {
            rows = 1;
        } else if (x + w > width - kRightMargin) {
            x = kLeftMargin;
            ++rows;
        }
        x += w;
    }
    return rows * kLegendRowHeight + (rows > 0 ? 4 : 0);
}

MetricChartWidget::Geometry MetricChartWidget::ComputeGeometry() const {
    Geometry geo;
    const QRect full = rect();

    int top = full.top() + kTopMargin;
    if (!title_.isEmpty()) top += kTitleHeight;
    top += LegendHeight(full.width());

    int bottom = full.bottom() - kBottomMargin;
    if (axis_x_->HasTitle()) bottom -= kAxisTitleExtra;

    const int left = full.left() + kLeftMargin;
    const int right = full.right() - (axis_y2_visible_ ? kLeftMargin : kRightMargin);
    geo.plot = QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));

    // ---- X 量程 ----
    double x_min = 0.0;
    double x_max = 1.0;
    bool has_x = false;
    for (const auto& s : series_) {
        for (const QPointF& p : s->Points()) {
            if (!FinitePoint(p)) continue;
            if (!has_x) { x_min = x_max = p.x(); has_x = true; continue; }
            x_min = std::min(x_min, p.x());
            x_max = std::max(x_max, p.x());
        }
    }
    if (!axis_x_->Categories().isEmpty()) {
        x_min = -0.5;
        x_max = static_cast<double>(axis_x_->Categories().size()) - 0.5;
        has_x = true;
    }
    if (axis_x_->HasRange()) {
        geo.x_min = axis_x_->Min();
        geo.x_max = axis_x_->Max();
    } else if (has_x) {
        geo.x_min = x_min;
        geo.x_max = x_max;
    } else {
        geo.x_min = 0.0;
        geo.x_max = 1.0;
    }
    if (geo.x_max - geo.x_min < 1e-9) geo.x_max = geo.x_min + 1.0;

    // ---- Y 量程（左 / 右轴各自独立）----
    const bool has_bars = std::any_of(series_.begin(), series_.end(),
        [](const std::unique_ptr<ChartSeries>& s) { return s->Kind() == ChartSeriesKind::Bar; });
    for (int axis_index = 0; axis_index < 2; ++axis_index) {
        ChartAxis* axis = (axis_index == 0) ? axis_y_.get() : axis_y2_.get();
        double* out_min = (axis_index == 0) ? &geo.y_min : &geo.y2_min;
        double* out_max = (axis_index == 0) ? &geo.y_max : &geo.y2_max;

        double v_min = 0.0;
        double v_max = 0.0;
        bool has_v = false;
        for (const auto& s : series_) {
            if (s->ValueAxisIndex() != axis_index) continue;
            for (const QPointF& p : s->Points()) {
                if (!FinitePoint(p)) continue;
                if (!has_v) { v_min = v_max = p.y(); has_v = true; continue; }
                v_min = std::min(v_min, p.y());
                v_max = std::max(v_max, p.y());
            }
        }
        if (axis->HasRange()) {
            *out_min = axis->Min();
            *out_max = axis->Max();
        } else if (has_v) {
            if (has_bars) v_min = std::min(0.0, v_min);
            const double pad = std::max(1e-6, (v_max - v_min) * 0.08);
            *out_min = v_min - pad;
            *out_max = v_max + pad;
        } else {
            *out_min = 0.0;
            *out_max = 1.0;
        }
        if (*out_max - *out_min < 1e-9) *out_max = *out_min + 1.0;
    }
    return geo;
}

void MetricChartWidget::DrawTitle(QPainter& painter, const Geometry& geo) {
    Q_UNUSED(geo);
    if (title_.isEmpty()) return;
    QFont f = font();
    f.setBold(true);
    painter.setFont(f);
    painter.setPen(palette().color(QPalette::WindowText));
    const QRect r(rect().left(), rect().top() + 2, rect().width(), kTitleHeight);
    painter.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, title_);
    painter.setFont(font());
}

void MetricChartWidget::DrawLegend(QPainter& painter, const Geometry& geo) {
    Q_UNUSED(geo);
    if (!legend_visible_) return;
    QFontMetrics fm(font());
    const QRect full = rect();
    int x = full.left() + kLeftMargin;
    int y = full.top() + kTopMargin + (title_.isEmpty() ? 0 : kTitleHeight) + 2;

    painter.setPen(palette().color(QPalette::WindowText));
    for (const auto& s : series_) {
        if (s->Name().isEmpty()) continue;
        const int w = 16 + fm.horizontalAdvance(s->Name()) + 12;
        if (x + w > full.right() - kRightMargin) {
            x = full.left() + kLeftMargin;
            y += kLegendRowHeight;
        }
        painter.fillRect(QRect(x, y + 3, 10, 10), s->Color());
        painter.drawText(QRect(x + 16, y, w - 16, kLegendRowHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, s->Name());
        x += w;
    }
}

void MetricChartWidget::DrawAxisAndGrid(QPainter& painter, const Geometry& geo) {
    const QRect& plot = geo.plot;
    const QColor text_color = palette().color(QPalette::WindowText);
    QColor grid_color = text_color;
    grid_color.setAlpha(45);

    painter.fillRect(plot, palette().color(QPalette::Base));

    const double span_x = geo.x_max - geo.x_min;
    const auto to_px = [&](double x) {
        return plot.left() + (x - geo.x_min) / span_x * static_cast<double>(plot.width());
    };
    const auto to_py = [&](double y, int axis_index) {
        const double lo = (axis_index == 0) ? geo.y_min : geo.y2_min;
        const double hi = (axis_index == 0) ? geo.y_max : geo.y2_max;
        const double span = hi - lo;
        return plot.bottom() - (y - lo) / span * static_cast<double>(plot.height());
    };

    QFontMetrics fm(font());
    painter.setPen(QPen(grid_color, 1, Qt::SolidLine));

    // 横向网格 + 左轴刻度
    const int y_ticks = std::max(2, axis_y_->TickCount());
    for (int i = 0; i < y_ticks; ++i) {
        const double value = geo.y_min + (geo.y_max - geo.y_min) * i / (y_ticks - 1);
        const qreal y = to_py(value, 0);
        painter.setPen(QPen(grid_color, 1, Qt::SolidLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(text_color);
        const QString label = FormatTick(value, axis_y_->LabelFormat());
        painter.drawText(QRect(plot.left() - kLeftMargin + 4, static_cast<int>(y) - fm.height() / 2,
                               kLeftMargin - 8, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter, label);
    }

    // 右轴（仅刻度，不画网格，避免与左轴网格打架）
    if (axis_y2_visible_) {
        const int y2_ticks = std::max(2, axis_y2_->TickCount());
        for (int i = 0; i < y2_ticks; ++i) {
            const double value = geo.y2_min + (geo.y2_max - geo.y2_min) * i / (y2_ticks - 1);
            const qreal y = to_py(value, 1);
            painter.setPen(text_color);
            painter.drawText(QRect(plot.right() + 6, static_cast<int>(y) - fm.height() / 2,
                                   kLeftMargin - 10, fm.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             FormatTick(value, axis_y2_->LabelFormat()));
        }
    }

    // 纵向网格 + X 轴刻度
    const QStringList& categories = axis_x_->Categories();
    painter.setPen(QPen(grid_color, 1, Qt::SolidLine));
    if (categories.isEmpty()) {
        const int x_ticks = std::min(kMaxXTicks, std::max(2, plot.width() / 90));
        for (int i = 0; i < x_ticks; ++i) {
            const double value = geo.x_min + span_x * i / (x_ticks - 1);
            const qreal x = to_px(value);
            painter.setPen(QPen(grid_color, 1, Qt::SolidLine));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.setPen(text_color);
            const QString label = FormatTick(value, axis_x_->LabelFormat());
            painter.drawText(QRect(static_cast<int>(x) - 45, plot.bottom() + 4, 90, fm.height()),
                             Qt::AlignHCenter | Qt::AlignTop, label);
        }
    } else {
        const int n = categories.size();
        const double group = 1.0;   // 分类轴: 一个分类占 1 个数据单位
        for (int i = 0; i < n; ++i) {
            const double value = -0.5 + (i + 0.5) * group;
            const qreal x = to_px(value);
            painter.setPen(text_color);
            painter.drawText(QRect(static_cast<int>(x) - 45, plot.bottom() + 4, 90, fm.height()),
                             Qt::AlignHCenter | Qt::AlignTop, categories.at(i));
        }
    }

    // 轴标题
    if (axis_x_->HasTitle()) {
        painter.setPen(text_color);
        painter.drawText(QRect(plot.left(), plot.bottom() + fm.height() + 4, plot.width(), fm.height()),
                         Qt::AlignHCenter | Qt::AlignTop, axis_x_->Title());
    }
    for (int axis_index = 0; axis_index < 2; ++axis_index) {
        if (axis_index == 1 && !axis_y2_visible_) continue;
        ChartAxis* axis = (axis_index == 0) ? axis_y_.get() : axis_y2_.get();
        if (!axis->HasTitle()) continue;
        painter.save();
        painter.translate(12, plot.center().y());
        painter.rotate(-90);
        painter.setPen(text_color);
        painter.drawText(QRect(-plot.height() / 2, -fm.height() / 2, plot.height(), fm.height()),
                         Qt::AlignCenter, axis->Title());
        painter.restore();
    }

    // 边框
    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.drawRect(plot);
}

void MetricChartWidget::DrawSeries(QPainter& painter, const Geometry& geo) {
    const QRect& plot = geo.plot;
    const double span_x = geo.x_max - geo.x_min;
    const auto to_px = [&](double x) {
        return plot.left() + (x - geo.x_min) / span_x * static_cast<double>(plot.width());
    };
    const auto to_py = [&](double y, int axis_index) {
        const double lo = (axis_index == 0) ? geo.y_min : geo.y2_min;
        const double hi = (axis_index == 0) ? geo.y_max : geo.y2_max;
        return plot.bottom() - (y - lo) / (hi - lo) * static_cast<double>(plot.height());
    };

    painter.save();
    painter.setClipRect(plot);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 1) 柱状（先画，避免盖住折线）
    int bar_series_count = 0;
    for (const auto& s : series_) {
        if (s->Kind() == ChartSeriesKind::Bar) ++bar_series_count;
    }
    if (bar_series_count > 0) {
        const QStringList& categories = axis_x_->Categories();
        int bar_index = 0;
        for (const auto& s : series_) {
            if (s->Kind() != ChartSeriesKind::Bar) continue;
            const QVector<QPointF>& pts = s->Points();
            painter.setPen(Qt::NoPen);
            painter.setBrush(s->Color());
            if (!categories.isEmpty()) {
                const double group_w = static_cast<double>(plot.width()) / categories.size();
                const double bar_w = std::max(2.0, group_w * 0.8 / bar_series_count);
                const double offset = (bar_index - (bar_series_count - 1) / 2.0) * bar_w;
                for (const QPointF& p : pts) {
                    if (!FinitePoint(p)) continue;
                    const double cx = to_px(p.x()) + offset;
                    const double zero = to_py(0.0, s->ValueAxisIndex());
                    const double top = to_py(p.y(), s->ValueAxisIndex());
                    const double y0 = std::min(zero, top);
                    const double y1 = std::max(zero, top);
                    painter.fillRect(QRectF(cx - bar_w / 2.0, y0, bar_w, std::max(1.5, y1 - y0)), s->Color());
                }
            } else {
                const int n = std::max(1, static_cast<int>(pts.size()));
                const double bar_w = std::max(2.0, static_cast<double>(plot.width()) / n * 0.7);
                for (const QPointF& p : pts) {
                    if (!FinitePoint(p)) continue;
                    const double cx = to_px(p.x());
                    const double zero = to_py(0.0, s->ValueAxisIndex());
                    const double top = to_py(p.y(), s->ValueAxisIndex());
                    const double y0 = std::min(zero, top);
                    const double y1 = std::max(zero, top);
                    painter.fillRect(QRectF(cx - bar_w / 2.0, y0, bar_w, std::max(1.5, y1 - y0)), s->Color());
                }
            }
            ++bar_index;
        }
    }

    // 2) 折线
    for (const auto& s : series_) {
        if (s->Kind() != ChartSeriesKind::Line) continue;
        const QVector<QPointF>& pts = s->Points();
        if (pts.isEmpty()) continue;
        const int axis_index = s->ValueAxisIndex();
        painter.setPen(QPen(s->Color(), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (pts.size() == 1) {
            const QPointF p(to_px(pts[0].x()), to_py(pts[0].y(), axis_index));
            painter.drawEllipse(p, 2.5, 2.5);
            continue;
        }
        QVector<QPointF> polyline;
        polyline.reserve(pts.size());
        for (const QPointF& p : pts) {
            if (!FinitePoint(p)) continue;
            polyline.append(QPointF(to_px(p.x()), to_py(p.y(), axis_index)));
        }
        painter.drawPolyline(polyline);

        if (s->PointsVisible()) {
            painter.setBrush(s->Color());
            painter.setPen(Qt::NoPen);
            for (const QPointF& p : polyline) {
                painter.drawEllipse(p, 2.0, 2.0);
            }
        }
    }

    // 3) 散点（画在最上层）
    for (const auto& s : series_) {
        if (s->Kind() != ChartSeriesKind::Scatter) continue;
        const int axis_index = s->ValueAxisIndex();
        const double r = s->MarkerSize() * 0.5;
        painter.setPen(QPen(s->BorderColor(), 1.0));
        painter.setBrush(s->Color());
        for (const QPointF& p : s->Points()) {
            if (!FinitePoint(p)) continue;
            const QPointF c(to_px(p.x()), to_py(p.y(), axis_index));
            switch (s->MarkerShape()) {
                case ChartMarkerShape::Rectangle:
                    painter.drawRect(QRectF(c.x() - r, c.y() - r, r * 2, r * 2));
                    break;
                case ChartMarkerShape::Triangle: {
                    QPolygonF tri;
                    tri << QPointF(c.x(), c.y() - r) << QPointF(c.x() + r, c.y() + r)
                        << QPointF(c.x() - r, c.y() + r);
                    painter.drawPolygon(tri);
                    break;
                }
                case ChartMarkerShape::Circle:
                default:
                    painter.drawEllipse(c, r, r);
                    break;
            }
        }
    }

    // 4) 悬停高亮
    if (hover_series_ >= 0 && hover_series_ < static_cast<int>(series_.size())) {
        const auto& s = series_[hover_series_];
        if (hover_index_ >= 0 && hover_index_ < s->Count()) {
            const QPointF& p = s->Points().at(hover_index_);
            if (FinitePoint(p)) {
                const QPointF c(to_px(p.x()), to_py(p.y(), s->ValueAxisIndex()));
                painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(c, 6.0, 6.0);
            }
        }
    }

    painter.restore();
}

void MetricChartWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font());

    const Geometry geo = ComputeGeometry();
    DrawTitle(painter, geo);
    DrawLegend(painter, geo);
    DrawAxisAndGrid(painter, geo);
    DrawSeries(painter, geo);
}

bool MetricChartWidget::HitTest(const QPoint& pos, const Geometry& geo, QPointF* out) {
    const QRect& plot = geo.plot;
    if (!plot.contains(pos)) return false;
    const double span_x = geo.x_max - geo.x_min;
    const auto to_px = [&](double x) {
        return plot.left() + (x - geo.x_min) / span_x * static_cast<double>(plot.width());
    };
    const auto to_py = [&](double y, int axis_index) {
        const double lo = (axis_index == 0) ? geo.y_min : geo.y2_min;
        const double hi = (axis_index == 0) ? geo.y_max : geo.y2_max;
        return plot.bottom() - (y - lo) / (hi - lo) * static_cast<double>(plot.height());
    };

    double best = static_cast<double>(kHitRadiusPx * kHitRadiusPx);
    bool found = false;
    for (int si = static_cast<int>(series_.size()) - 1; si >= 0; --si) {
        const auto& s = series_[si];
        const QVector<QPointF>& pts = s->Points();
        for (int i = 0; i < pts.size(); ++i) {
            const QPointF& p = pts.at(i);
            if (!FinitePoint(p)) continue;
            const double dx = to_px(p.x()) - pos.x();
            const double dy = to_py(p.y(), s->ValueAxisIndex()) - pos.y();
            const double dist = dx * dx + dy * dy;
            if (dist <= best) {
                best = dist;
                hover_series_ = si;
                hover_index_ = i;
                if (out) *out = p;
                found = true;
            }
        }
    }
    return found;
}

void MetricChartWidget::mouseMoveEvent(QMouseEvent* event) {
    const Geometry geo = ComputeGeometry();
    QPointF hit;
    const bool found = HitTest(event->pos(), geo, &hit);
    const int prev_series = hover_series_;
    const int prev_index = hover_index_;
    if (!found) {
        hover_series_ = -1;
        hover_index_ = -1;
    }
    if (found || prev_series != hover_series_ || prev_index != hover_index_) {
        update();
    }
    if (found) {
        emit PointHovered(hit, true);
    } else if (prev_series >= 0) {
        emit PointHovered(QPointF(), false);
    }
    QWidget::mouseMoveEvent(event);
}

void MetricChartWidget::leaveEvent(QEvent* event) {
    if (hover_series_ >= 0) {
        hover_series_ = -1;
        hover_index_ = -1;
        update();
        emit PointHovered(QPointF(), false);
    }
    QWidget::leaveEvent(event);
}

}  // namespace ui
}  // namespace videoeye
