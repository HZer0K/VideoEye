#pragma once

// 自绘轻量图表控件（替代 QtCharts）。
//
// 目标：项目只保留 Qt Widgets + FFmpeg 两个硬依赖，图表这类"好看但不必引第三方库"
// 的功能全部用 QPainter 自己画。支持三种系列：
//   Line    折线（可选显示点）
//   Scatter 散点（圆 / 方 / 三角标记）
//   Bar     柱状（支持分类轴分组并列）
// 另外支持左右双 Y 轴、坐标轴标题/刻度格式/量程、图例与鼠标悬停命中。

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <vector>

namespace videoeye {
namespace ui {

enum class ChartSeriesKind { Line, Scatter, Bar };

enum class ChartMarkerShape { Circle, Rectangle, Triangle };

class MetricChartWidget;

// 坐标轴。由 MetricChartWidget 持有，外部只改属性。
//
// 改属性要通知所属图表把布局缓存置脏（刻度文本宽度 → 左右边距）。
// setter 实现在 .cpp：那里 MetricChartWidget 已是完整类型。
class ChartAxis {
public:
    void SetRange(double min, double max);
    void ResetRange();

    void SetLabelFormat(const QString& format);
    void SetTitleText(const QString& title);
    void SetTickCount(int count);
    // 柱状图用：设置后 X 轴按分类渲染，柱子分组并列
    void SetCategories(const QStringList& categories);

    double Min() const {
        return min_;
    }
    double Max() const {
        return max_;
    }
    bool HasRange() const {
        return has_range_;
    }
    bool HasTitle() const {
        return !title_.isEmpty();
    }
    const QString& Title() const {
        return title_;
    }
    const QString& LabelFormat() const {
        return label_format_;
    }
    int TickCount() const {
        return tick_count_;
    }
    const QStringList& Categories() const {
        return categories_;
    }

private:
    // 属性变化后通知所属图表：坐标轴量程 / 刻度格式都会改变绘图区边距。
    void NotifyChanged();

    MetricChartWidget* owner_ = nullptr;
    friend class MetricChartWidget;

    double min_ = 0.0;
    double max_ = 1.0;
    bool has_range_ = false;
    int tick_count_ = 5;
    QString label_format_ = QStringLiteral("%g");
    QString title_;
    QStringList categories_;
};

// 数据系列。内存由 MetricChartWidget 独占，外部持有裸指针即可。
//
// owner 是"本系列画在哪个图表上"：AddSeries 创建时由图表填进来。有了它，
// 改数据（Append / Replace / Clear）与改外观（SetColor / SetMarkerSize ...）
// 都能顺手排一次重绘，不再依赖别的事件顺带把界面刷出来。
class ChartSeries {
public:
    ChartSeries(ChartSeriesKind kind, const QString& name, const QColor& color, MetricChartWidget* owner = nullptr)
        : kind_(kind), name_(name), color_(color), border_color_(color), owner_(owner) {
    }

    ChartSeriesKind Kind() const {
        return kind_;
    }

    void SetName(const QString& name) {
        name_ = name;
        NotifyUpdate();
    }
    const QString& Name() const {
        return name_;
    }

    void SetColor(const QColor& color) {
        color_ = color;
        NotifyUpdate();
    }
    QColor Color() const {
        return color_;
    }

    void SetBorderColor(const QColor& color) {
        border_color_ = color;
        NotifyUpdate();
    }
    QColor BorderColor() const {
        return border_color_;
    }

    void SetMarkerSize(double size) {
        marker_size_ = size < 1.0 ? 1.0 : size;
        NotifyUpdate();
    }
    double MarkerSize() const {
        return marker_size_;
    }

    void SetMarkerShape(ChartMarkerShape shape) {
        marker_shape_ = shape;
        NotifyUpdate();
    }
    ChartMarkerShape MarkerShape() const {
        return marker_shape_;
    }

    void SetPointsVisible(bool visible) {
        points_visible_ = visible;
        NotifyUpdate();
    }
    bool PointsVisible() const {
        return points_visible_;
    }

    // 0 = 左轴, 1 = 右轴
    void SetValueAxisIndex(int index) {
        value_axis_ = (index == 1) ? 1 : 0;
        NotifyUpdate();
    }
    int ValueAxisIndex() const {
        return value_axis_;
    }

    void Clear() {
        points_.clear();
        x_sorted_ = true;
        NotifyUpdate();
    }
    void Append(double x, double y) {
        TrackOrder(x);
        points_.append(QPointF(x, y));
        NotifyUpdate();
    }
    void Append(const QPointF& point) {
        TrackOrder(point.x());
        points_.append(point);
        NotifyUpdate();
    }
    ChartSeries& operator<<(const QPointF& point) {
        TrackOrder(point.x());
        points_.append(point);
        NotifyUpdate();
        return *this;
    }
    void Replace(QVector<QPointF> points) {
        points_ = std::move(points);
        x_sorted_ = true;
        for (int i = 1; i < points_.size(); ++i) {
            if (!(points_[i - 1].x() <= points_[i].x())) {
                x_sorted_ = false;
                break;
            }
        }
        NotifyUpdate();
    }

    const QVector<QPointF>& Points() const {
        return points_;
    }
    int Count() const {
        return points_.size();
    }
    bool IsEmpty() const {
        return points_.isEmpty();
    }
    // x 是否单调不减。命中测试据此用二分缩小搜索范围；
    // 顺序被打乱过（含 NaN）就退回线性扫描，避免二分在错序数据上漏点。
    bool XSorted() const {
        return x_sorted_;
    }

private:
    void TrackOrder(double x) {
        if (!points_.isEmpty() && !(points_.last().x() <= x))
            x_sorted_ = false;
    }
    // 定义在 .cpp：那里 ChartSeries 与 MetricChartWidget 都已是完整类型
    void NotifyUpdate();

    MetricChartWidget* owner_ = nullptr; // 本系列所属图表，nullptr = 还没挂上
    ChartSeriesKind kind_ = ChartSeriesKind::Line;
    QString name_;
    QColor color_ = QColor("#42a5f5");
    QColor border_color_ = QColor("#42a5f5");
    double marker_size_ = 6.0;
    ChartMarkerShape marker_shape_ = ChartMarkerShape::Circle;
    bool points_visible_ = false;
    bool x_sorted_ = true;
    int value_axis_ = 0;
    QVector<QPointF> points_;
};

class MetricChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit MetricChartWidget(QWidget* parent = nullptr);

    void SetTitle(const QString& title);
    const QString& Title() const {
        return title_;
    }

    void SetLegendVisible(bool visible);
    bool LegendVisible() const {
        return legend_visible_;
    }

    // 工厂：创建系列并由图表接管所有权，返回裸指针供外部保存
    ChartSeries* AddSeries(const QString& name, ChartSeriesKind kind, const QColor& color);
    ChartSeries* AddLineSeries(const QString& name, const QColor& color = QColor("#42a5f5"));
    ChartSeries* AddScatterSeries(const QString& name, const QColor& color = QColor("#e53935"));
    ChartSeries* AddBarSeries(const QString& name, const QColor& color = QColor("#42a5f5"));

    // 清空所有系列的数据点（保留系列本身与配色）
    void ClearSeriesData();

    ChartAxis* AxisX() {
        return axis_x_.get();
    }
    ChartAxis* AxisY() {
        return axis_y_.get();
    }
    ChartAxis* AxisY2() {
        return axis_y2_.get();
    }
    void SetAxisY2Visible(bool visible);

    // 当一个图表里有多个 Y 轴时，用它把系列绑到右轴
    void AttachAxis(ChartSeries* series, ChartAxis* axis);

    void SetCategories(const QStringList& categories) {
        axis_x_->SetCategories(categories);
    }

    // 局部缩放：滚轮缩放时间轴、左键拖动平移、双击复位。
    // ResetView() 在整份数据被替换时由调用方显式调用，避免旧视窗带到新数据上。
    void ResetView();
    bool HasActiveView() const {
        return view_active_;
    }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // 鼠标悬停命中某个数据点时发出；移出时 state=false。
    // 只在悬浮点真正变化时发出，鼠标在同一点上移动不会重复刷屏。
    void PointHovered(const QPointF& point, bool state);
    // 单击绘图区（未拖动）时发出，参数是命中位置的 x 值（通常是时间，单位秒）。
    void PlotClicked(double x_value);
    // 局部缩放/平移后的可见 x 区间；复位时也会发一次完整区间。
    void ViewChanged(double x_min, double x_max);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    // 绘图区左/右两侧留给刻度文本与轴标题的实际宽度（按当前字体与刻度值算出来）。
    // 以前是写死的 kLeftMargin，轴标题（竖排）与刻度文本抢同一条窄带，
    // 于是"亮度""黑像素比例"两条竖排标题互相叠，还压住 191.25 这类长刻度。
    struct Geometry {
        QRect plot;
        double x_min = 0.0;
        double x_max = 1.0;
        double base_x_min = 0.0; // 未缩放时的完整 x 区间（缩放钳制边界）
        double base_x_max = 1.0;
        double y_min = 0.0;
        double y_max = 1.0;
        double y2_min = 0.0;
        double y2_max = 1.0;
        int left_tick_width = 0;  // 左轴刻度文本区宽度（含与轴线之间的间隙）
        int right_tick_width = 0; // 右轴刻度文本区宽度，右轴隐藏时为 0
        int left_title_x = 0;     // 左轴竖排标题的中心 x
        int right_title_x = 0;    // 右轴竖排标题的中心 x
    };

    Geometry ComputeGeometry() const;
    // 布局缓存：鼠标移动 / 重绘都复用同一份。数据、量程、尺寸、字体变化时置脏。
    const Geometry& ChartGeometry() const;
    void InvalidateGeometry();
    double DataXAt(const QPoint& pos, const Geometry& geo) const;

    void DrawAxisAndGrid(QPainter& painter, const Geometry& geo);
    void DrawSeries(QPainter& painter, const Geometry& geo);
    void DrawLegend(QPainter& painter, const Geometry& geo);
    void DrawTitle(QPainter& painter, const Geometry& geo);
    // start_x = 图例首行的起始横坐标（与绘图区左边界对齐），行数算准了才知道
    // 绘图区该从哪里开始，也就不会让"255"这类顶部刻度顶到图例行上
    int LegendHeight(int width, int start_x) const;
    bool HitTest(const QPoint& pos, const Geometry& geo, QPointF* out);

    QString title_;
    bool legend_visible_ = true;
    bool axis_y2_visible_ = false;

    std::unique_ptr<ChartAxis> axis_x_;
    std::unique_ptr<ChartAxis> axis_y_;
    std::unique_ptr<ChartAxis> axis_y2_;
    std::vector<std::unique_ptr<ChartSeries>> series_;

    int hover_series_ = -1;
    int hover_index_ = -1;

    mutable Geometry cached_geometry_;
    mutable bool geometry_dirty_ = true;

    // 局部缩放（仅 x 轴）。y 轴保持全量程，缩放时曲线起伏不会被重新拉伸。
    bool view_active_ = false;
    double view_x_min_ = 0.0;
    double view_x_max_ = 1.0;

    // 左键拖动平移；拖动超过阈值后不再当作单击（避免"拖一下顺带跳播"）。
    bool dragging_ = false;
    bool drag_moved_ = false;
    QPoint press_pos_;
    double drag_view_min_ = 0.0;
    double drag_view_max_ = 1.0;

    friend class ChartSeries;
    friend class ChartAxis;
};

} // namespace ui
} // namespace videoeye
