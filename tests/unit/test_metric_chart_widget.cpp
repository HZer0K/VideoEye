// MetricChartWidget 的交互回归。
//
// 这里守住三条与性能直接相关的口径：
//   1) 系列自己维护 x 是否单调 —— 命中测试据此二分缩小搜索范围，
//      错序数据必须退回线性扫描，不能为了快而漏点；
//   2) 单击绘图区发出 PlotClicked(x)，点在绘图区外不触发（避免误跳播）；
//   3) 滚轮缩放进入局部视图，双击复位。
//
// 布局缓存本身不单独断言（属于实现细节），但它一旦算错，上面三条交互会先坏。
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>

#include <vector>

#include "ui/charts/MetricChartWidget.h"

using videoeye::ui::ChartSeries;
using videoeye::ui::ChartSeriesKind;
using videoeye::ui::MetricChartWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_metric_chart_widget";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

void ClickAt(QWidget* widget, const QPoint& pos) {
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

void PrepareChart(MetricChartWidget& chart) {
    chart.resize(600, 400);
    chart.AxisX()->SetRange(0.0, 100.0);
    chart.AxisY()->SetRange(0.0, 100.0);
    ChartSeries* series = chart.AddLineSeries(QStringLiteral("v"), QColor("#1e88e5"));
    series->Append(0.0, 0.0);
    series->Append(50.0, 50.0);
    series->Append(100.0, 100.0);
}

} // namespace

TEST(MetricChartWidgetTests, SeriesTracksMonotonicX) {
    ChartSeries series(ChartSeriesKind::Line, QStringLiteral("t"), QColor("#42a5f5"));
    EXPECT_TRUE(series.XSorted());

    series.Append(0.0, 1.0);
    series.Append(1.0, 2.0);
    series.Append(2.0, 3.0);
    EXPECT_TRUE(series.XSorted());

    // 时间倒流：必须标记为无序，命中测试改走线性扫描
    series.Append(1.5, 9.0);
    EXPECT_FALSE(series.XSorted());
}

TEST(MetricChartWidgetTests, ReplaceRecomputesMonotonicX) {
    ChartSeries series(ChartSeriesKind::Line, QStringLiteral("t"), QColor("#42a5f5"));
    series.Append(5.0, 0.0);
    series.Append(1.0, 0.0);
    ASSERT_FALSE(series.XSorted());

    // 整批替换会重新扫一遍顺序
    series.Replace({QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(2.0, 0.0)});
    EXPECT_TRUE(series.XSorted());

    series.Replace({QPointF(1.0, 0.0), QPointF(0.0, 0.0)});
    EXPECT_FALSE(series.XSorted());
}

TEST(MetricChartWidgetTests, ClickInsidePlotEmitsXAndOutsideDoesNot) {
    EnsureApp();
    MetricChartWidget chart;
    PrepareChart(chart);

    std::vector<double> clicks;
    QObject::connect(&chart, &MetricChartWidget::PlotClicked, [&clicks](double x) { clicks.push_back(x); });

    // 控件中心必在绘图区内；x 轴 0~100，中心大致落在中段
    ClickAt(&chart, chart.rect().center());
    ASSERT_EQ(clicks.size(), 1u);
    EXPECT_GT(clicks[0], 20.0);
    EXPECT_LT(clicks[0], 70.0);

    // 左上角在左边距/图例带内，不属于绘图区
    ClickAt(&chart, QPoint(2, 2));
    EXPECT_EQ(clicks.size(), 1u);
}

TEST(MetricChartWidgetTests, WheelZoomsAndDoubleClickResetsView) {
    EnsureApp();
    MetricChartWidget chart;
    PrepareChart(chart);

    EXPECT_FALSE(chart.HasActiveView());

    const QPoint center = chart.rect().center();
    QWheelEvent wheel(QPointF(center), QPointF(chart.mapToGlobal(center)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&chart, &wheel);
    EXPECT_TRUE(chart.HasActiveView());

    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(center), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&chart, &dbl);
    EXPECT_FALSE(chart.HasActiveView());
}

TEST(MetricChartWidgetTests, ClickAfterDragDoesNotSeek) {
    EnsureApp();
    MetricChartWidget chart;
    PrepareChart(chart);

    std::vector<double> clicks;
    QObject::connect(&chart, &MetricChartWidget::PlotClicked, [&clicks](double x) { clicks.push_back(x); });

    // 先放大，拖动才有效
    const QPoint center = chart.rect().center();
    QWheelEvent wheel(QPointF(center), QPointF(chart.mapToGlobal(center)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&chart, &wheel);
    ASSERT_TRUE(chart.HasActiveView());

    // 按下后移动超过阈值再松开：这是平移，不是单击
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(center), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&chart, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(center + QPoint(20, 0)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&chart, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(center + QPoint(20, 0)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&chart, &release);

    EXPECT_TRUE(clicks.empty());
    EXPECT_TRUE(chart.HasActiveView());
}
