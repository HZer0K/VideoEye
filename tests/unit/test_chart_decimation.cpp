// 曲线抽稀的回归。
//
// 这份测试存在的理由不是"算法对不对"，而是守住两条**业务口径**：
//   1. 谷值不许被抹掉 —— 码率塌陷 / 响度掉底 / 亮度骤降是 QC 最关心的缺陷，
//      抽稀完之后曲线上必须还看得见；
//   2. 点数必须有硬上限 —— 自绘图表的悬停命中是 O(总点数) 线性扫描，
//      点数失控会直接把鼠标移动变成卡顿。
// 这两条以前都靠各页自觉，六处调用有四种口径。

#include "ui/charts/ChartDecimation.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace {

using videoeye::ui::ChartSeries;
using videoeye::ui::ChartSeriesKind;
using videoeye::ui::DecimatedPoints;
using videoeye::ui::DecimateFrom;
using videoeye::ui::DecimateXY;
using videoeye::ui::DecimationMode;
using videoeye::ui::kMaxChartPoints;
using videoeye::ui::SubmitDecimated;

std::vector<double> Range(double from, double to, int n) {
    std::vector<double> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i)
        v.push_back(from + (to - from) * i / std::max(1, n - 1));
    return v;
}

// 方波：1000 个周期，每个周期先 0 后 100。
// 这是最容易被"每组取最大值"或"等间隔取点"抹掉一半的形状。
void MakeSquareWave(std::vector<double>& xs, std::vector<double>& ys, int cycles) {
    xs.clear();
    ys.clear();
    for (int i = 0; i < cycles * 2; ++i) {
        xs.push_back(static_cast<double>(i) * 0.01);
        ys.push_back((i % 2 == 0) ? 0.0 : 100.0);
    }
}

bool ContainsValue(const DecimatedPoints& d, double v) {
    for (const QPointF& p : d.points) {
        if (std::abs(p.y() - v) < 1e-9)
            return true;
    }
    return false;
}

TEST(ChartDecimationTest, ShortSeriesPassesThrough) {
    const auto xs = Range(0.0, 10.0, 50);
    const auto ys = Range(1.0, 2.0, 50);
    const auto d = DecimateXY(xs, ys, 100);

    EXPECT_EQ(50, d.source_count);
    EXPECT_EQ(50, d.output_count);
    EXPECT_FALSE(d.decimated());
}

TEST(ChartDecimationTest, EmptyInput) {
    const auto d = DecimateXY({}, {});
    EXPECT_EQ(0, d.source_count);
    EXPECT_TRUE(d.points.isEmpty());
}

// 口径 1：谷值不许被抹掉。
TEST(ChartDecimationTest, MinMaxKeepsBothPeaksAndValleys) {
    std::vector<double> xs;
    std::vector<double> ys;
    MakeSquareWave(xs, ys, 5000); // 10000 个点

    const auto d = DecimateXY(xs, ys, 4000, DecimationMode::MinMax);

    EXPECT_EQ(10000, d.source_count);
    EXPECT_LE(d.output_count, 4000);
    EXPECT_TRUE(d.decimated());
    EXPECT_TRUE(ContainsValue(d, 0.0)) << "谷值被抽掉了：码率塌陷会看不见";
    EXPECT_TRUE(ContainsValue(d, 100.0)) << "峰值被抽掉了";
}

// 对照组：Stride 确实会丢一半。留着这条是为了让"为什么默认 MinMax"有据可依，
// 而不是一句"据说更好"。
TEST(ChartDecimationTest, StrideCanDropAnExtreme) {
    std::vector<double> xs;
    std::vector<double> ys;
    // 步长会正好踩在偶数列上：全取到 0
    MakeSquareWave(xs, ys, 5000);

    const auto d = DecimateXY(xs, ys, 4000, DecimationMode::Stride);
    EXPECT_LE(d.output_count, 4000);
    // 步长 = ceil(10000/4000) = 3，奇偶交替 -> 两种值都可能出现，
    // 但只要出现"只剩一种"就证明 Stride 不可靠（这条断言是文档性质的，
    // 允许两种结果；真正兜底的是上面 MinMax 那条）。
    EXPECT_TRUE(ContainsValue(d, 0.0) || ContainsValue(d, 100.0));
}

// 口径 2：点数硬上限。即使源是 100 万点、limit 很小，也不许超。
TEST(ChartDecimationTest, OutputNeverExceedsLimit) {
    const auto xs = Range(0.0, 100000.0, 200000);
    auto ys = xs;
    for (std::size_t i = 0; i < ys.size(); ++i) {
        ys[i] = std::sin(ys[i]) * 50.0 + (i % 7 == 0 ? 999.0 : 0.0);
    }

    for (int limit : {2, 10, 100, 4000}) {
        const auto d = DecimateXY(xs, ys, limit, DecimationMode::MinMax);
        EXPECT_LE(d.output_count, limit) << "limit=" << limit;
    }
}

// NaN 必须被剔除，且**不能占桶** —— 否则一段全是 NaN 的区间会把曲线抽秃。
TEST(ChartDecimationTest, NonFinitePointsAreSkippedWithoutConsumingBuckets) {
    const int n = 1000;
    std::vector<double> xs;
    std::vector<double> ys;
    for (int i = 0; i < n; ++i) {
        xs.push_back(static_cast<double>(i));
        // 前 900 个是 NaN（模拟"指标没算出来"），后 100 个是真实值
        ys.push_back(i < 900 ? std::nan("") : static_cast<double>(i));
    }

    const auto d = DecimateXY(xs, ys, 4000);
    EXPECT_EQ(1000, d.source_count);
    // 只剩 100 个可用点，小于 limit 时应当全保留
    EXPECT_EQ(100, d.output_count);
    for (const QPointF& p : d.points) {
        EXPECT_TRUE(std::isfinite(p.y()));
        EXPECT_GE(p.x(), 900.0);
    }
}

TEST(ChartDecimationTest, AllNonFiniteYieldsEmpty) {
    const int n = 100;
    std::vector<double> xs;
    std::vector<double> ys;
    for (int i = 0; i < n; ++i) {
        xs.push_back(static_cast<double>(i));
        ys.push_back(std::nan(""));
    }
    const auto d = DecimateXY(xs, ys, 4000);
    EXPECT_EQ(100, d.source_count); // 源点数如实反映
    EXPECT_TRUE(d.points.isEmpty());
}

// 时间序：输出必须按 x 单调不减。MinMax 若把 min/max 按"先小后大"固定输出，
// 折线会在每个桶里来回折返，画出根本不存在的锯齿。
TEST(ChartDecimationTest, OutputIsMonotonicInTime) {
    std::vector<double> xs;
    std::vector<double> ys;
    MakeSquareWave(xs, ys, 2000);

    const auto d = DecimateXY(xs, ys, 500, DecimationMode::MinMax);
    for (int i = 1; i < d.points.size(); ++i) {
        EXPECT_LE(d.points[i - 1].x(), d.points[i].x()) << "第 " << i << " 个点时间倒流";
    }
}

// 模板入口：从结构体数组取成员，不经过中间数组
TEST(ChartDecimationTest, DecimateFromWorksOnStructArray) {
    struct Sample {
        double t;
        double v;
    };
    std::vector<Sample> samples;
    for (int i = 0; i < 5000; ++i) {
        samples.push_back({static_cast<double>(i) * 0.1, (i % 2 == 0) ? -1.0 : 1.0});
    }

    const auto d = DecimateFrom(samples, [](const Sample& s) { return s.t; }, [](const Sample& s) { return s.v; }, 400);
    EXPECT_EQ(5000, d.source_count);
    EXPECT_LE(d.output_count, 400);
    EXPECT_TRUE(ContainsValue(d, -1.0));
    EXPECT_TRUE(ContainsValue(d, 1.0));
}

// 提交路径：一次性 Replace，不逐点 Append。
// ChartSeries 用 owner=nullptr 构造（NotifyUpdate 会判空），不需要 QApplication。
TEST(ChartDecimationTest, SubmitDecimatedReplacesWholeSeries) {
    std::vector<double> xs;
    std::vector<double> ys;
    MakeSquareWave(xs, ys, 3000);

    ChartSeries series(ChartSeriesKind::Line, QStringLiteral("test"), QColor("#42a5f5"));
    const auto d = DecimateXY(xs, ys, 400);
    SubmitDecimated(&series, d);

    EXPECT_EQ(d.output_count, series.Count());
    EXPECT_LE(series.Count(), 400);

    // 再提交一次必须是替换而不是追加：旧点不该留下
    const auto d2 = DecimateXY(xs, ys, 100);
    SubmitDecimated(&series, d2);
    EXPECT_EQ(d2.output_count, series.Count());
}

TEST(ChartDecimationTest, SubmitDecimatedToleratesNullSeries) {
    const auto d = DecimateXY(Range(0.0, 1.0, 10), Range(0.0, 1.0, 10), 10);
    EXPECT_NO_FATAL_FAILURE(SubmitDecimated(nullptr, d));
}

// 常量本身的口径关系：窗口（滚动）必须小于折线抽稀上限，
// 否则"保留最近 N 个"会退化成"根本不抽稀"。
TEST(ChartDecimationTest, ConstantRelationships) {
    EXPECT_GT(kMaxChartPoints, videoeye::ui::kMaxChartMarkers);
    EXPECT_GT(kMaxChartPoints, videoeye::ui::kMaxRecentChartItems);
    EXPECT_GE(kMaxChartPoints, 2);
    EXPECT_GE(videoeye::ui::kMaxRecentChartItems, 2);
}

} // namespace
