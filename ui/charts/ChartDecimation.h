#pragma once

// 曲线抽稀的统一策略。
//
// 为什么要有这个文件：抽稀原本散在各页里，每页一把尺子 ——
//   码率 GOP 页 4000（每组取最大值）、诊断页 4000、画面质量页 600（stride）、
//   流概览页 300、事件时间轴 300、音频 QC 页 4000（stride）。
// 六处四种口径，而且其中三种是"每组取最大值"或"等间隔取点"：这两种都会
// **把谷值抹掉**。码率塌陷、响度掉底、亮度骤降恰恰是 QC 最关心的那类缺陷，
// 抽完稀曲线上却是一条平线 —— 不是没检测出来，是被抽稀吃掉了。
//
// 统一之后只有一个实现、一个上限（kMaxChartPoints），默认 MinMax：
// 每个桶保留最小值与最大值两点，按原始时间序输出，峰谷都不丢。
//
// 另一个统一点是提交方式：一律先在内存里攒好再一次性 Replace()，
// 不许逐点 Append —— 见下面 SubmitDecimated 的注释。

#include <QPointF>
#include <QVector>

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "ui/charts/MetricChartWidget.h"

namespace videoeye {
namespace ui {

// 提交到图表控件的点数上限。
//
// 定 4000 的依据不是"看起来够"，而是控件宽度：一张图最宽也就一千多像素，
// 4000 点已经是 2~4 倍过采样。再往上加不会更精细，只会拖慢两件事 ——
// 绘制时每个点都要走一次坐标变换，以及鼠标悬停的命中测试是 O(总点数) 线性扫描
// （MetricChartWidget::HitTest），鼠标一动就扫一遍全部点。
inline constexpr int kMaxChartPoints = 4000;

// 散点 / 标记类（I 帧位置、场景切换标记）最多绘制多少个。
//
// 比折线上限低是因为成本结构不同：折线的一个点只是一次 moveTo/lineTo，
// 而一个标记要画矩形或圆 + 描边，几倍于折线点。
inline constexpr int kMaxChartMarkers = 1500;

// "只保留最近 N 个"的滚动窗口大小（场景切换柱、流概览的实时曲线、时间轴图）。
//
// 注意它与 kMaxChartPoints 的区别：抽稀是**压缩**（全时段都在，只是点变少），
// 这里是**丢掉更早的**。语义完全不同，别互相套用 ——
// 拿滚动窗口当抽稀用会让"全片前 5 分钟"从图上看不见，反之则实时曲线会越画越密。
// 取值 300：这三个地方原本各自写 200 / 300 / 300，取整到同一把尺子。
inline constexpr int kMaxRecentChartItems = 300;

// 抽稀模式。
enum class DecimationMode {
    // 每个桶保留 min / max 两点（默认）。峰谷都留下，适合会突然塌陷或冲高的指标。
    MinMax,
    // 等间隔取点。最省，但会漏掉桶内的尖峰；只适合本来就平滑、且尖峰无意义的曲线。
    Stride,
};

// 抽稀结果。带上源点数是为了让调用方能告诉用户"这条曲线被压缩过"，
// 而不是悄悄丢数据 —— 丢了多少必须可观察。
struct DecimatedPoints {
    QVector<QPointF> points;
    int source_count = 0;
    int output_count = 0;

    bool decimated() const { return output_count < source_count; }
};

namespace detail {

// NaN / Inf 一律跳过：指标算不出来时写的是 NaN（kQualityNoValue），
// 混进曲线里会让 QPainter 画出一段飞到无穷远的线，剩下的正常点全被压成一条平线。
inline bool Usable(double x, double y) { return std::isfinite(x) && std::isfinite(y); }

}  // namespace detail

// 抽稀的核心：输入已过滤掉非有限值的 (x, y) 序列。
//
// 抽成独立函数是为了让"从数组来"和"从任意容器来"两条路共用同一段桶划分逻辑 ——
// 抽稀算法只有一份，改口径只改这一处。
inline DecimatedPoints DecimatePairs(const std::vector<std::pair<double, double>>& usable,
                                     int source_count, int limit = kMaxChartPoints,
                                     DecimationMode mode = DecimationMode::MinMax) {
    DecimatedPoints out;
    out.source_count = source_count;
    if (usable.empty()) return out;
    if (limit < 2) limit = 2;

    const std::size_t m = usable.size();
    if (m <= static_cast<std::size_t>(limit)) {
        out.points.reserve(static_cast<int>(m));
        for (const auto& p : usable) out.points.append(QPointF(p.first, p.second));
        out.output_count = out.points.size();
        return out;
    }

    if (mode == DecimationMode::Stride) {
        const std::size_t step = (m + limit - 1) / limit;
        out.points.reserve(limit);
        for (std::size_t i = 0; i < m; i += step) {
            out.points.append(QPointF(usable[i].first, usable[i].second));
        }
        out.output_count = out.points.size();
        return out;
    }

    // MinMax: 每桶最多输出两点，所以桶数取 limit/2，保证总输出 <= limit。
    const std::size_t buckets = static_cast<std::size_t>(limit) / 2;
    const std::size_t group = (m + buckets - 1) / buckets;
    out.points.reserve(limit);
    for (std::size_t start = 0; start < m; start += group) {
        const std::size_t end = std::min(start + group, m);
        std::size_t min_i = start;
        std::size_t max_i = start;
        for (std::size_t i = start + 1; i < end; ++i) {
            if (usable[i].second < usable[min_i].second) min_i = i;
            if (usable[i].second > usable[max_i].second) max_i = i;
        }
        // 按原始时间序输出，否则折线会在桶内来回折返，画出不存在的锯齿
        if (min_i <= max_i) {
            out.points.append(QPointF(usable[min_i].first, usable[min_i].second));
            if (max_i != min_i) {
                out.points.append(QPointF(usable[max_i].first, usable[max_i].second));
            }
        } else {
            out.points.append(QPointF(usable[max_i].first, usable[max_i].second));
            out.points.append(QPointF(usable[min_i].first, usable[min_i].second));
        }
    }
    out.output_count = out.points.size();
    return out;
}

// 从两个等长的数组抽稀。xs 通常是时间戳（秒），ys 是指标值。
inline DecimatedPoints DecimateXY(const std::vector<double>& xs, const std::vector<double>& ys,
                                  int limit = kMaxChartPoints,
                                  DecimationMode mode = DecimationMode::MinMax) {
    const std::size_t n = std::min(xs.size(), ys.size());
    // 先按"可用点"收集：源里可能混着 NaN，抽稀的桶必须按**可用点**均分，
    // 否则一个 NaN 密集的区间会吃掉了整整几个桶的配额，曲线在那段变秃。
    std::vector<std::pair<double, double>> usable;
    usable.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (detail::Usable(xs[i], ys[i])) usable.emplace_back(xs[i], ys[i]);
    }
    return DecimatePairs(usable, static_cast<int>(n), limit, mode);
}

// 从任意容器抽稀：gx / gy 分别取出 x 与 y。
//
// 用模板而不是"先拷成两个 vector"：音频 QC 的响度点是一个结构体数组，
// 七条曲线各取一个成员 —— 为了抽稀先拷七份 double 数组，等于每帧复制
// 七份大内存，正是要避免的那类开销。这里一步直接攒成 (x, y) 对，
// 中间不再落一层数组。
template <typename Container, typename XGetter, typename YGetter>
DecimatedPoints DecimateFrom(const Container& items, XGetter gx, YGetter gy,
                             int limit = kMaxChartPoints,
                             DecimationMode mode = DecimationMode::MinMax) {
    std::vector<std::pair<double, double>> usable;
    usable.reserve(items.size());
    for (const auto& item : items) {
        const double x = static_cast<double>(gx(item));
        const double y = static_cast<double>(gy(item));
        if (detail::Usable(x, y)) usable.emplace_back(x, y);
    }
    return DecimatePairs(usable, static_cast<int>(items.size()), limit, mode);
}

// 一次性提交到系列：攒好再 Replace()，全程只触发一次重绘。
//
// 为什么不能逐点 Append：ChartSeries::Append 每加一点就 NotifyUpdate()，
// 等价于给控件投一个 paint 事件。几千点就是几千次投递，主线程在填曲线的
// 那段时间里基本不干别的（实测 7 条曲线 × 4000 点 ≈ 3.5 s）。
inline void SubmitDecimated(ChartSeries* series, const DecimatedPoints& data) {
    if (series == nullptr) return;
    series->Replace(data.points);
}

}  // namespace ui
}  // namespace videoeye
