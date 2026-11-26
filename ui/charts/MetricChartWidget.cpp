#include "ui/charts/MetricChartWidget.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

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
// 竖排轴标题横向占位 = 一个字高，再留一点与刻度文本之间的间隙
constexpr int kAxisTitleGap = 4;
// 刻度文本与轴线之间的间隙
constexpr int kTickPad = 6;
constexpr int kHitRadiusPx = 8;
constexpr int kMaxXTicks = 6;

// 支持 "%d" / "%.1f" 等 printf 风格格式；%d 时按整型取值，避免 double 传入 %d 的未定义行为。
QString FormatTick(double value, const QString& format) {
    // 默认格式（"%g"）按 6 位有效数字输出，长尾会把刻度写成 264.695 / 66.1739。
    // 统一压到 4 位并消掉 -0，读数更干净。
    if (format.isEmpty() || format == QLatin1String("%g")) {
        if (std::abs(value) < 5e-5)
            value = 0.0;
        return QString::number(value, 'g', 4);
    }
    const QByteArray fmt = format.toUtf8();
    if (format.contains(QLatin1Char('d')) || format.contains(QLatin1Char('i'))) {
        return QString::asprintf(fmt.constData(), static_cast<int>(std::llround(value)));
    }
    return QString::asprintf(fmt.constData(), value);
}

bool FinitePoint(const QPointF& p) {
    return std::isfinite(p.x()) && std::isfinite(p.y());
}

bool UsesIntegerFormat(const QString& format) {
    return format.contains(QLatin1Char('d')) || format.contains(QLatin1Char('i'));
}

// 格式能表达的最小变化量：%d → 1，%.0f → 1，%.1f → 0.1，%g → 0（不限制）。
// 刻度步长必须不小于它，否则 0.5 会被 %.0f 写成 "1" 却画在中间。
double TickResolution(const QString& format) {
    if (UsesIntegerFormat(format))
        return 1.0;
    const int dot = format.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return 0.0;
    int end = dot + 1;
    while (end < format.size() && format.at(end).isDigit())
        ++end;
    bool ok = false;
    const int decimals = format.mid(dot + 1, end - dot - 1).toInt(&ok);
    if (!ok)
        return 0.0;
    return std::pow(10.0, -decimals);
}

// 把"大致步长"收敛到 1 / 2 / 5 × 10^n，刻度落在整数位上（0、50、100…），
// 而不是按像素等分后得到 264.7 / 198.5 这种刻度。
double NiceStep(double rough) {
    if (!(rough > 0.0) || !std::isfinite(rough))
        return 1.0;
    const double base = std::pow(10.0, std::floor(std::log10(rough)));
    const double f = rough / base;
    const double nice = (f <= 1.5) ? 1.0 : (f <= 3.0) ? 2.0 : (f <= 7.0) ? 5.0 : 10.0;
    return nice * base;
}

// 生成刻度值。对齐到步长而不是硬等分，避免粗格式把相邻刻度写成同一个字；
// 步长按格式精度取下限，0~1 区间用 %d / %.0f 时只会得到 0 和 1 两个刻度。
//
// 可放置的整数刻度不足 2 个时退回等分刻度，并通过 label_format 让调用方
// 改用默认格式，避免满屏重复的 0 / 1。
QVector<double> TickValues(double lo, double hi, int count, const QString& format, QString* label_format) {
    const int n = std::max(2, count);
    QVector<double> values;
    const double span = hi - lo;
    if (span > 0.0) {
        double step = NiceStep(span / (n - 1));
        step = std::max(step, TickResolution(format));
        const double first = std::ceil(lo / step - 1e-9) * step;
        for (double v = first; v <= hi + 1e-9 && values.size() < 64; v += step)
            values.append(v);
        if (values.size() >= 2) {
            if (label_format)
                *label_format = format;
            return values;
        }
        values.clear();
    }
    values.reserve(n);
    for (int i = 0; i < n; ++i)
        values.append(lo + (hi - lo) * i / (n - 1));
    if (label_format)
        *label_format = TickResolution(format) > 0.0 ? QString() : format;
    return values;
}

// 某个 Y 轴在当前量程下，所有刻度文本里最宽的那个有多宽。
//
// 边距必须按"实际要写的字"来算：左轴写 191.25 这种 6 字符刻度，右轴只写 0.75，
// 写死一个常数不是太挤（叠字）就是太空（绘图区白白变窄）。量程定了刻度值就定了，
// 所以这里能把每个刻度格式化一遍取最大宽度。
int TickLabelMaxWidth(const ChartAxis& axis, double lo, double hi, const QFontMetrics& fm) {
    QString label_format;
    const QVector<double> values = TickValues(lo, hi, axis.TickCount(), axis.LabelFormat(), &label_format);
    int width = 0;
    for (double value : values) {
        width = std::max(width, fm.horizontalAdvance(FormatTick(value, label_format)));
    }
    return width;
}

} // namespace

// --- ChartAxis ---------------------------------------------------------------
//
// 属性变化必须让图表的布局缓存失效：左右边距是按刻度文本宽度实测出来的，
// 量程 / 刻度数 / 格式 / 轴标题任一变化都会改变边距。setter 放 .cpp 是因为
// 这里 MetricChartWidget 已是完整类型。
void ChartAxis::SetRange(double min, double max) {
    min_ = min;
    max_ = max;
    has_range_ = true;
    NotifyChanged();
}

void ChartAxis::ResetRange() {
    has_range_ = false;
    NotifyChanged();
}

void ChartAxis::SetLabelFormat(const QString& format) {
    label_format_ = format;
    NotifyChanged();
}

void ChartAxis::SetTitleText(const QString& title) {
    title_ = title;
    NotifyChanged();
}

void ChartAxis::SetTickCount(int count) {
    tick_count_ = count < 2 ? 2 : count;
    NotifyChanged();
}

void ChartAxis::SetCategories(const QStringList& categories) {
    categories_ = categories;
    NotifyChanged();
}

void ChartAxis::NotifyChanged() {
    if (owner_)
        owner_->InvalidateGeometry();
}

void ChartSeries::NotifyUpdate() {
    // 数据或外观一变就排一次重绘，不等别的事件顺带刷出去 —— 以前改完数据
    // 界面停在旧曲线上，要等切页/缩放之类的随机事件才更新，用户看到的就是"没刷新"。
    // update() 是排期不是立即画，Qt 会把同一帧内的多次调用合并成一次 paint，
    // 所以批量 Append / Replace 不会退化成逐点重绘。
    if (owner_) {
        // x/y 量程由数据决定：新点会改变范围，布局缓存必须一起失效。
        owner_->InvalidateGeometry();
        owner_->update();
    }
}

MetricChartWidget::MetricChartWidget(QWidget* parent)
    : QWidget(parent), axis_x_(std::make_unique<ChartAxis>()), axis_y_(std::make_unique<ChartAxis>()),
      axis_y2_(std::make_unique<ChartAxis>()) {
    // 轴属性变化要回头把布局缓存置脏
    axis_x_->owner_ = this;
    axis_y_->owner_ = this;
    axis_y2_->owner_ = this;
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void MetricChartWidget::InvalidateGeometry() {
    geometry_dirty_ = true;
}

const MetricChartWidget::Geometry& MetricChartWidget::ChartGeometry() const {
    if (geometry_dirty_) {
        cached_geometry_ = ComputeGeometry();
        geometry_dirty_ = false;
    }
    return cached_geometry_;
}

void MetricChartWidget::SetTitle(const QString& title) {
    if (title_ == title)
        return;
    title_ = title;
    InvalidateGeometry();
    update();
}

void MetricChartWidget::SetLegendVisible(bool visible) {
    if (legend_visible_ == visible)
        return;
    legend_visible_ = visible;
    InvalidateGeometry();
    update();
}

ChartSeries* MetricChartWidget::AddSeries(const QString& name, ChartSeriesKind kind, const QColor& color) {
    // 建系列时把"我画在哪个图表上"交代给系列，后面的 Append / Replace / setter
    // 才能自己排重绘
    series_.push_back(std::make_unique<ChartSeries>(kind, name, color, this));
    InvalidateGeometry();
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
    ResetView();
    InvalidateGeometry();
    update();
}

void MetricChartWidget::SetAxisY2Visible(bool visible) {
    if (axis_y2_visible_ == visible)
        return;
    axis_y2_visible_ = visible;
    InvalidateGeometry();
    update();
}

void MetricChartWidget::AttachAxis(ChartSeries* series, ChartAxis* axis) {
    if (!series || !axis)
        return;
    // X 轴全图共用，只需要区分左右两个 Y 轴
    if (axis == axis_y2_.get()) {
        series->SetValueAxisIndex(1);
    } else if (axis == axis_y_.get()) {
        series->SetValueAxisIndex(0);
    }
}

QSize MetricChartWidget::sizeHint() const {
    return QSize(360, 220);
}
QSize MetricChartWidget::minimumSizeHint() const {
    return QSize(120, 80);
}

int MetricChartWidget::LegendHeight(int width, int start_x) const {
    if (!legend_visible_)
        return 0;
    QFontMetrics fm(font());
    int x = start_x;
    int rows = 0;
    for (const auto& s : series_) {
        if (s->Name().isEmpty())
            continue;
        const int w = 16 + fm.horizontalAdvance(s->Name()) + 12;
        if (rows == 0) {
            rows = 1;
        } else if (x + w > width - kRightMargin) {
            x = start_x;
            ++rows;
        }
        x += w;
    }
    return rows * kLegendRowHeight + (rows > 0 ? 4 : 0);
}

MetricChartWidget::Geometry MetricChartWidget::ComputeGeometry() const {
    Geometry geo;
    const QRect full = rect();

    // ---- X 量程 ----
    double x_min = 0.0;
    double x_max = 1.0;
    bool has_x = false;
    for (const auto& s : series_) {
        for (const QPointF& p : s->Points()) {
            if (!FinitePoint(p))
                continue;
            if (!has_x) {
                x_min = x_max = p.x();
                has_x = true;
                continue;
            }
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
    if (geo.x_max - geo.x_min < 1e-9)
        geo.x_max = geo.x_min + 1.0;

    // 记录未缩放的完整区间（缩放钳制边界），再套用局部视图。
    // 这里按边界钳制而不改 view_active_：ComputeGeometry 是 const 缓存计算，
    // 数据缩水时视图会自动退回完整区间，不需要额外写回。
    geo.base_x_min = geo.x_min;
    geo.base_x_max = geo.x_max;
    if (view_active_) {
        const double lo = std::max(view_x_min_, geo.base_x_min);
        const double hi = std::min(view_x_max_, geo.base_x_max);
        if (hi - lo > 1e-12) {
            geo.x_min = lo;
            geo.x_max = hi;
        }
    }

    // ---- Y 量程（左 / 右轴各自独立）----
    const bool has_bars = std::any_of(series_.begin(), series_.end(), [](const std::unique_ptr<ChartSeries>& s) {
        return s->Kind() == ChartSeriesKind::Bar;
    });
    for (int axis_index = 0; axis_index < 2; ++axis_index) {
        ChartAxis* axis = (axis_index == 0) ? axis_y_.get() : axis_y2_.get();
        double* out_min = (axis_index == 0) ? &geo.y_min : &geo.y2_min;
        double* out_max = (axis_index == 0) ? &geo.y_max : &geo.y2_max;

        double v_min = 0.0;
        double v_max = 0.0;
        bool has_v = false;
        for (const auto& s : series_) {
            if (s->ValueAxisIndex() != axis_index)
                continue;
            for (const QPointF& p : s->Points()) {
                if (!FinitePoint(p))
                    continue;
                if (!has_v) {
                    v_min = v_max = p.y();
                    has_v = true;
                    continue;
                }
                v_min = std::min(v_min, p.y());
                v_max = std::max(v_max, p.y());
            }
        }
        if (axis->HasRange()) {
            *out_min = axis->Min();
            *out_max = axis->Max();
        } else if (has_v) {
            if (has_bars)
                v_min = std::min(0.0, v_min);
            const double pad = std::max(1e-6, (v_max - v_min) * 0.08);
            *out_min = v_min - pad;
            *out_max = v_max + pad;
        } else {
            *out_min = 0.0;
            *out_max = 1.0;
        }
        if (*out_max - *out_min < 1e-9)
            *out_max = *out_min + 1.0;
    }

    // ---- 左右边距：量程定了才算得出刻度文字，算完再回头定绘图区 ----
    //
    // 绘图区左右各留两段：最外侧是竖排轴标题（旋转 90° 后横向只占一个字高），
    // 往里是刻度文本。两段都按当前字体实测，谁都不会压到谁。
    QFontMetrics fm(font());
    const int left_tick_w = TickLabelMaxWidth(*axis_y_, geo.y_min, geo.y_max, fm) + kTickPad;
    const int right_tick_w = axis_y2_visible_ ? TickLabelMaxWidth(*axis_y2_, geo.y2_min, geo.y2_max, fm) + kTickPad : 0;
    const int left_title_w = axis_y_->HasTitle() ? fm.height() + kAxisTitleGap : 0;
    const int right_title_w = (axis_y2_visible_ && axis_y2_->HasTitle()) ? fm.height() + kAxisTitleGap : 0;

    // 下限兜住没有轴标题、刻度又很短的情况，避免绘图区贴到控件边缘
    const int left_margin = std::max(kLeftMargin, left_title_w + left_tick_w);
    const int right_margin = std::max(kRightMargin, right_title_w + right_tick_w);

    const int left = full.left() + left_margin;
    const int right = full.right() - right_margin;

    // ---- 上下边界 ----
    int top = full.top() + kTopMargin;
    if (!title_.isEmpty())
        top += kTitleHeight;
    top += LegendHeight(full.width(), left);

    int bottom = full.bottom() - kBottomMargin;
    if (axis_x_->HasTitle())
        bottom -= kAxisTitleExtra;

    geo.plot = QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));

    geo.left_tick_width = left_tick_w;
    geo.right_tick_width = right_tick_w;
    geo.left_title_x = full.left() + left_title_w / 2;
    geo.right_title_x = full.right() - right_title_w / 2;
    return geo;
}

void MetricChartWidget::DrawTitle(QPainter& painter, const Geometry& geo) {
    Q_UNUSED(geo);
    if (title_.isEmpty())
        return;
    QFont f = font();
    f.setBold(true);
    painter.setFont(f);
    painter.setPen(palette().color(QPalette::WindowText));
    const QRect r(rect().left(), rect().top() + 2, rect().width(), kTitleHeight);
    painter.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, title_);
    painter.setFont(font());
}

void MetricChartWidget::DrawLegend(QPainter& painter, const Geometry& geo) {
    if (!legend_visible_)
        return;
    QFontMetrics fm(font());
    const QRect full = rect();
    // 与绘图区左边界对齐：图例起点跟着左边距走，才不会压到左轴最上面那个刻度
    const int start_x = geo.plot.left();
    int x = start_x;
    int y = full.top() + kTopMargin + (title_.isEmpty() ? 0 : kTitleHeight) + 2;

    painter.setPen(palette().color(QPalette::WindowText));
    for (const auto& s : series_) {
        if (s->Name().isEmpty())
            continue;
        const int w = 16 + fm.horizontalAdvance(s->Name()) + 12;
        if (x + w > full.right() - kRightMargin) {
            x = start_x;
            y += kLegendRowHeight;
        }
        painter.fillRect(QRect(x, y + 3, 10, 10), s->Color());
        painter.drawText(QRect(x + 16, y, w - 16, kLegendRowHeight), Qt::AlignLeft | Qt::AlignVCenter, s->Name());
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

    // 刻度文本区：紧挨轴线外侧，宽度由 ComputeGeometry 按实测文本给出
    const int left_tick_w = geo.left_tick_width > 0 ? geo.left_tick_width : kLeftMargin;
    const int right_tick_w = geo.right_tick_width > 0 ? geo.right_tick_width : kLeftMargin;

    // 横向网格 + 左轴刻度
    //
    // 标签去重：格式较粗（如空图上 "%d"）时相邻刻度会算成同一个字，
    // 以前会画出 "1 1 1 0" 这种重复刻度。网格线保留，只压掉重复文本。
    QString y_label_format;
    const QVector<double> y_values =
        TickValues(geo.y_min, geo.y_max, axis_y_->TickCount(), axis_y_->LabelFormat(), &y_label_format);
    QString last_y_label;
    bool has_last_y = false;
    for (double value : y_values) {
        const qreal y = to_py(value, 0);
        painter.setPen(QPen(grid_color, 1, Qt::SolidLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const QString label = FormatTick(value, y_label_format);
        if (has_last_y && label == last_y_label)
            continue;
        last_y_label = label;
        has_last_y = true;
        painter.setPen(text_color);
        painter.drawText(QRect(plot.left() - left_tick_w, static_cast<int>(y) - fm.height() / 2, left_tick_w - kTickPad,
                               fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter, label);
    }

    // 右轴（仅刻度，不画网格，避免与左轴网格打架）
    if (axis_y2_visible_) {
        QString y2_label_format;
        const QVector<double> y2_values =
            TickValues(geo.y2_min, geo.y2_max, axis_y2_->TickCount(), axis_y2_->LabelFormat(), &y2_label_format);
        QString last_y2_label;
        bool has_last_y2 = false;
        for (double value : y2_values) {
            const qreal y = to_py(value, 1);
            const QString label = FormatTick(value, y2_label_format);
            if (has_last_y2 && label == last_y2_label)
                continue;
            last_y2_label = label;
            has_last_y2 = true;
            painter.setPen(text_color);
            painter.drawText(QRect(plot.right() + kTickPad, static_cast<int>(y) - fm.height() / 2,
                                   right_tick_w - kTickPad, fm.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, label);
        }
    }

    // 纵向网格 + X 轴刻度
    const QStringList& categories = axis_x_->Categories();
    painter.setPen(QPen(grid_color, 1, Qt::SolidLine));
    if (categories.isEmpty()) {
        const int x_tick_cap = std::min(kMaxXTicks, std::max(2, plot.width() / 90));
        QString x_label_format;
        const QVector<double> x_values =
            TickValues(geo.x_min, geo.x_max, x_tick_cap, axis_x_->LabelFormat(), &x_label_format);
        QString last_x_label;
        bool has_last_x = false;
        for (double value : x_values) {
            const qreal x = to_px(value);
            painter.setPen(QPen(grid_color, 1, Qt::SolidLine));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            const QString label = FormatTick(value, x_label_format);
            if (has_last_x && label == last_x_label)
                continue;
            last_x_label = label;
            has_last_x = true;
            painter.setPen(text_color);
            painter.drawText(QRect(static_cast<int>(x) - 45, plot.bottom() + 4, 90, fm.height()),
                             Qt::AlignHCenter | Qt::AlignTop, label);
        }
    } else {
        const int n = categories.size();
        const double group = 1.0; // 分类轴: 一个分类占 1 个数据单位
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
        if (axis_index == 1 && !axis_y2_visible_)
            continue;
        ChartAxis* axis = (axis_index == 0) ? axis_y_.get() : axis_y2_.get();
        if (!axis->HasTitle())
            continue;
        // 左轴标题贴控件左边、右轴标题贴控件右边 —— 以前两边都写死 x=12，
        // 于是"亮度"和"黑像素比例"两条竖排标题叠在同一条窄带上，还一起压住刻度数字
        const int title_x = (axis_index == 0) ? geo.left_title_x : geo.right_title_x;
        painter.save();
        painter.translate(title_x, plot.center().y());
        painter.rotate(-90);
        painter.setPen(text_color);
        // 竖排标题超过绘图区高度时按可用长度省略，避免在文字中间被硬切
        const QString title_text = fm.elidedText(axis->Title(), Qt::ElideRight, plot.height() - 4);
        painter.drawText(QRect(-plot.height() / 2, -fm.height() / 2, plot.height(), fm.height()), Qt::AlignCenter,
                         title_text);
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
        if (s->Kind() == ChartSeriesKind::Bar)
            ++bar_series_count;
    }
    if (bar_series_count > 0) {
        const QStringList& categories = axis_x_->Categories();
        int bar_index = 0;
        for (const auto& s : series_) {
            if (s->Kind() != ChartSeriesKind::Bar)
                continue;
            const QVector<QPointF>& pts = s->Points();
            painter.setPen(Qt::NoPen);
            painter.setBrush(s->Color());
            if (!categories.isEmpty()) {
                const double group_w = static_cast<double>(plot.width()) / categories.size();
                const double bar_w = std::max(2.0, group_w * 0.8 / bar_series_count);
                const double offset = (bar_index - (bar_series_count - 1) / 2.0) * bar_w;
                for (const QPointF& p : pts) {
                    if (!FinitePoint(p))
                        continue;
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
                    if (!FinitePoint(p))
                        continue;
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
        if (s->Kind() != ChartSeriesKind::Line)
            continue;
        const QVector<QPointF>& pts = s->Points();
        if (pts.isEmpty())
            continue;
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
            if (!FinitePoint(p))
                continue;
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
        if (s->Kind() != ChartSeriesKind::Scatter)
            continue;
        const int axis_index = s->ValueAxisIndex();
        const double r = s->MarkerSize() * 0.5;
        painter.setPen(QPen(s->BorderColor(), 1.0));
        painter.setBrush(s->Color());
        for (const QPointF& p : s->Points()) {
            if (!FinitePoint(p))
                continue;
            const QPointF c(to_px(p.x()), to_py(p.y(), axis_index));
            switch (s->MarkerShape()) {
            case ChartMarkerShape::Rectangle:
                painter.drawRect(QRectF(c.x() - r, c.y() - r, r * 2, r * 2));
                break;
            case ChartMarkerShape::Triangle: {
                QPolygonF tri;
                tri << QPointF(c.x(), c.y() - r) << QPointF(c.x() + r, c.y() + r) << QPointF(c.x() - r, c.y() + r);
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

    const Geometry& geo = ChartGeometry();
    DrawTitle(painter, geo);
    DrawLegend(painter, geo);
    DrawAxisAndGrid(painter, geo);
    DrawSeries(painter, geo);

    // 缩放态提示：不占布局空间，叠在绘图区右上角。
    const double base_span = geo.base_x_max - geo.base_x_min;
    if (base_span > 0.0 && (geo.x_max - geo.x_min) < base_span * 0.999) {
        painter.save();
        painter.setFont(font());
        const QString hint = QStringLiteral("已缩放 · 双击复位");
        const QFontMetrics fm(font());
        const int tw = fm.horizontalAdvance(hint) + 12;
        const QRect box(geo.plot.right() - tw - 6, geo.plot.top() + 6, tw, fm.height() + 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 96));
        painter.drawRoundedRect(box, 3, 3);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(box, Qt::AlignCenter, hint);
        painter.restore();
    }
}

double MetricChartWidget::DataXAt(const QPoint& pos, const Geometry& geo) const {
    const double span_x = geo.x_max - geo.x_min;
    if (geo.plot.width() <= 0 || !(span_x > 0.0))
        return geo.x_min;
    const double ratio =
        std::clamp(static_cast<double>(pos.x() - geo.plot.left()) / static_cast<double>(geo.plot.width()), 0.0, 1.0);
    return geo.x_min + ratio * span_x;
}

bool MetricChartWidget::HitTest(const QPoint& pos, const Geometry& geo, QPointF* out) {
    const QRect& plot = geo.plot;
    if (!plot.contains(pos))
        return false;
    const double span_x = geo.x_max - geo.x_min;
    const auto to_px = [&](double x) {
        return plot.left() + (x - geo.x_min) / span_x * static_cast<double>(plot.width());
    };
    const auto to_py = [&](double y, int axis_index) {
        const double lo = (axis_index == 0) ? geo.y_min : geo.y2_min;
        const double hi = (axis_index == 0) ? geo.y_max : geo.y2_max;
        return plot.bottom() - (y - lo) / (hi - lo) * static_cast<double>(plot.height());
    };

    // 光标左右 kHitRadiusPx 对应的数据 x 区间。按时间排序的曲线只需在这个窗口内二分，
    // 不必每次鼠标移动都线性扫过全部点（4000 点 × 每次 move 的旧口径）。
    const double window_x_min = DataXAt(QPoint(pos.x() - kHitRadiusPx, pos.y()), geo);
    const double window_x_max = DataXAt(QPoint(pos.x() + kHitRadiusPx, pos.y()), geo);

    double best = static_cast<double>(kHitRadiusPx * kHitRadiusPx);
    bool found = false;
    for (int si = static_cast<int>(series_.size()) - 1; si >= 0; --si) {
        const auto& s = series_[si];
        const QVector<QPointF>& pts = s->Points();
        if (pts.isEmpty())
            continue;
        const int axis_index = s->ValueAxisIndex();
        const auto consider = [&](int i) {
            const QPointF& p = pts.at(i);
            if (!FinitePoint(p))
                return;
            const double dx = to_px(p.x()) - pos.x();
            const double dy = to_py(p.y(), axis_index) - pos.y();
            const double dist = dx * dx + dy * dy;
            if (dist <= best) {
                best = dist;
                hover_series_ = si;
                hover_index_ = i;
                if (out)
                    *out = p;
                found = true;
            }
        };
        if (s->XSorted()) {
            const auto first = std::lower_bound(pts.cbegin(), pts.cend(), window_x_min,
                                                [](const QPointF& p, double v) { return p.x() < v; });
            const auto last =
                std::upper_bound(first, pts.cend(), window_x_max, [](double v, const QPointF& p) { return v < p.x(); });
            for (auto it = first; it != last; ++it)
                consider(static_cast<int>(it - pts.cbegin()));
        } else {
            for (int i = 0; i < pts.size(); ++i)
                consider(i);
        }
    }
    return found;
}

void MetricChartWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        const Geometry& geo = ChartGeometry();
        const int dx = event->pos().x() - press_pos_.x();
        if (!drag_moved_ && std::abs(dx) >= 3)
            drag_moved_ = true;
        const double span = drag_view_max_ - drag_view_min_;
        const double base_span = geo.base_x_max - geo.base_x_min;
        if (view_active_ && drag_moved_ && span > 0.0 && base_span >= span && geo.plot.width() > 0) {
            const double shift = -static_cast<double>(dx) / static_cast<double>(geo.plot.width()) * span;
            const double lo = std::clamp(drag_view_min_ + shift, geo.base_x_min, geo.base_x_max - span);
            view_x_min_ = lo;
            view_x_max_ = lo + span;
            InvalidateGeometry();
            update();
            emit ViewChanged(view_x_min_, view_x_max_);
        }
        QWidget::mouseMoveEvent(event);
        return;
    }

    const Geometry& geo = ChartGeometry();
    QPointF hit;
    const bool found = HitTest(event->pos(), geo, &hit);
    const int prev_series = hover_series_;
    const int prev_index = hover_index_;
    if (!found) {
        hover_series_ = -1;
        hover_index_ = -1;
    }
    // 悬浮点没变就不重绘：旧实现只要命中就 update()，鼠标停在一个点附近抖动也会
    // 一直排重绘。
    const bool changed = found ? (prev_series != hover_series_ || prev_index != hover_index_) : (prev_series >= 0);
    if (changed)
        update();
    if (found && changed)
        emit PointHovered(hit, true);
    else if (!found && prev_series >= 0)
        emit PointHovered(QPointF(), false);
    QWidget::mouseMoveEvent(event);
}

void MetricChartWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const Geometry& geo = ChartGeometry();
        dragging_ = geo.plot.contains(event->pos());
        drag_moved_ = false;
        press_pos_ = event->pos();
        drag_view_min_ = geo.x_min;
        drag_view_max_ = geo.x_max;
    }
    QWidget::mousePressEvent(event);
}

void MetricChartWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        const Geometry& geo = ChartGeometry();
        const bool was_click = !drag_moved_ && geo.plot.contains(event->pos());
        dragging_ = false;
        drag_moved_ = false;
        if (was_click)
            emit PlotClicked(DataXAt(event->pos(), geo));
    }
    QWidget::mouseReleaseEvent(event);
}

void MetricChartWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const Geometry& geo = ChartGeometry();
        if (geo.plot.contains(event->pos()) && view_active_) {
            ResetView();
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void MetricChartWidget::wheelEvent(QWheelEvent* event) {
    const Geometry& geo = ChartGeometry();
    if (!geo.plot.contains(event->position().toPoint())) {
        QWidget::wheelEvent(event);
        return;
    }
    const double base_span = geo.base_x_max - geo.base_x_min;
    const double cur_span = geo.x_max - geo.x_min;
    const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
    if (!(base_span > 0.0) || !(cur_span > 0.0) || delta == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    // 向上滚放大（0.8×），向下缩小；以光标位置为锚点，缩放时视线不跑。
    const double new_span =
        std::clamp(cur_span * (delta > 0 ? 0.8 : 1.25), std::max(base_span * 0.002, 1e-9), base_span);
    if (new_span >= base_span * 0.999) {
        ResetView();
        event->accept();
        return;
    }

    const double cursor_x = DataXAt(event->position().toPoint(), geo);
    const double ratio = std::clamp((cursor_x - geo.x_min) / cur_span, 0.0, 1.0);
    double lo = cursor_x - ratio * new_span;
    double hi = lo + new_span;
    if (lo < geo.base_x_min) {
        lo = geo.base_x_min;
        hi = lo + new_span;
    }
    if (hi > geo.base_x_max) {
        hi = geo.base_x_max;
        lo = hi - new_span;
    }
    view_active_ = true;
    view_x_min_ = lo;
    view_x_max_ = hi;
    InvalidateGeometry();
    update();
    emit ViewChanged(view_x_min_, view_x_max_);
    event->accept();
}

void MetricChartWidget::ResetView() {
    if (!view_active_)
        return;
    view_active_ = false;
    InvalidateGeometry();
    update();
    const Geometry& geo = ChartGeometry();
    emit ViewChanged(geo.x_min, geo.x_max);
}

void MetricChartWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    InvalidateGeometry();
}

void MetricChartWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
        // 边距按字体实测，换字体/主题必须重算
        InvalidateGeometry();
        break;
    default:
        break;
    }
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

} // namespace ui
} // namespace videoeye
