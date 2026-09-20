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
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <vector>

namespace videoeye {
namespace ui {

enum class ChartSeriesKind {
    Line,
    Scatter,
    Bar
};

enum class ChartMarkerShape {
    Circle,
    Rectangle,
    Triangle
};

// 坐标轴。由 MetricChartWidget 持有，外部只改属性。
class ChartAxis {
public:
    void SetRange(double min, double max) {
        min_ = min;
        max_ = max;
        has_range_ = true;
    }
    void ResetRange() { has_range_ = false; }

    void SetLabelFormat(const QString& format) { label_format_ = format; }
    void SetTitleText(const QString& title) { title_ = title; }
    void SetTickCount(int count) { tick_count_ = count < 2 ? 2 : count; }
    // 柱状图用：设置后 X 轴按分类渲染，柱子分组并列
    void SetCategories(const QStringList& categories) { categories_ = categories; }

    double Min() const { return min_; }
    double Max() const { return max_; }
    bool HasRange() const { return has_range_; }
    bool HasTitle() const { return !title_.isEmpty(); }
    const QString& Title() const { return title_; }
    const QString& LabelFormat() const { return label_format_; }
    int TickCount() const { return tick_count_; }
    const QStringList& Categories() const { return categories_; }

private:
    double min_ = 0.0;
    double max_ = 1.0;
    bool has_range_ = false;
    int tick_count_ = 5;
    QString label_format_ = QStringLiteral("%g");
    QString title_;
    QStringList categories_;
};

// 数据系列。内存由 MetricChartWidget 独占，外部持有裸指针即可。
class ChartSeries {
public:
    ChartSeries(ChartSeriesKind kind, const QString& name, const QColor& color)
        : kind_(kind), name_(name), color_(color), border_color_(color) {}

    ChartSeriesKind Kind() const { return kind_; }

    void SetName(const QString& name) { name_ = name; }
    const QString& Name() const { return name_; }

    void SetColor(const QColor& color) { color_ = color; }
    QColor Color() const { return color_; }

    void SetBorderColor(const QColor& color) { border_color_ = color; }
    QColor BorderColor() const { return border_color_; }

    void SetMarkerSize(double size) { marker_size_ = size < 1.0 ? 1.0 : size; }
    double MarkerSize() const { return marker_size_; }

    void SetMarkerShape(ChartMarkerShape shape) { marker_shape_ = shape; }
    ChartMarkerShape MarkerShape() const { return marker_shape_; }

    void SetPointsVisible(bool visible) { points_visible_ = visible; }
    bool PointsVisible() const { return points_visible_; }

    // 0 = 左轴, 1 = 右轴
    void SetValueAxisIndex(int index) { value_axis_ = (index == 1) ? 1 : 0; }
    int ValueAxisIndex() const { return value_axis_; }

    void Clear() { points_.clear(); }
    void Append(double x, double y) { points_.append(QPointF(x, y)); }
    void Append(const QPointF& point) { points_.append(point); }
    ChartSeries& operator<<(const QPointF& point) { points_.append(point); return *this; }
    void Replace(QVector<QPointF> points) { points_ = std::move(points); }

    const QVector<QPointF>& Points() const { return points_; }
    int Count() const { return points_.size(); }
    bool IsEmpty() const { return points_.isEmpty(); }

private:
    ChartSeriesKind kind_ = ChartSeriesKind::Line;
    QString name_;
    QColor color_ = QColor("#42a5f5");
    QColor border_color_ = QColor("#42a5f5");
    double marker_size_ = 6.0;
    ChartMarkerShape marker_shape_ = ChartMarkerShape::Circle;
    bool points_visible_ = false;
    int value_axis_ = 0;
    QVector<QPointF> points_;
};

class MetricChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit MetricChartWidget(QWidget* parent = nullptr);

    void SetTitle(const QString& title);
    const QString& Title() const { return title_; }

    void SetLegendVisible(bool visible);
    bool LegendVisible() const { return legend_visible_; }

    // 工厂：创建系列并由图表接管所有权，返回裸指针供外部保存
    ChartSeries* AddSeries(const QString& name, ChartSeriesKind kind, const QColor& color);
    ChartSeries* AddLineSeries(const QString& name, const QColor& color = QColor("#42a5f5"));
    ChartSeries* AddScatterSeries(const QString& name, const QColor& color = QColor("#e53935"));
    ChartSeries* AddBarSeries(const QString& name, const QColor& color = QColor("#42a5f5"));

    // 清空所有系列的数据点（保留系列本身与配色）
    void ClearSeriesData();

    ChartAxis* AxisX() { return axis_x_.get(); }
    ChartAxis* AxisY() { return axis_y_.get(); }
    ChartAxis* AxisY2() { return axis_y2_.get(); }
    void SetAxisY2Visible(bool visible);

    // 当一个图表里有多个 Y 轴时，用它把系列绑到右轴
    void AttachAxis(ChartSeries* series, ChartAxis* axis);

    void SetCategories(const QStringList& categories) { axis_x_->SetCategories(categories); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // 鼠标悬停命中某个数据点时发出；移出时 state=false
    void PointHovered(const QPointF& point, bool state);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Geometry {
        QRect plot;
        double x_min = 0.0;
        double x_max = 1.0;
        double y_min = 0.0;
        double y_max = 1.0;
        double y2_min = 0.0;
        double y2_max = 1.0;
    };

    Geometry ComputeGeometry() const;
    void DrawAxisAndGrid(QPainter& painter, const Geometry& geo);
    void DrawSeries(QPainter& painter, const Geometry& geo);
    void DrawLegend(QPainter& painter, const Geometry& geo);
    void DrawTitle(QPainter& painter, const Geometry& geo);
    int  LegendHeight(int width) const;
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
};

}  // namespace ui
}  // namespace videoeye
