#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 单个采样点
struct MetricSample {
    double timestamp_seconds = 0.0;
    double value = 0.0;
};

// 时间序列指标（码率/帧率/响度等通用载体）。
// 设计目标: 让"分析层产出 -> UI 图表/规则引擎消费"有统一口径，
// 避免每个分析器各自定义 std::vector<double> 造成的对齐与单位混乱。
struct MetricSeries {
    std::string name;   // 标识, 如 "video_bitrate"
    std::string unit;   // 单位, 如 "kbps" / "fps" / "LUFS"
    std::vector<MetricSample> samples;

    void Add(double timestamp_seconds, double value) {
        samples.push_back(MetricSample{timestamp_seconds, value});
    }
    void Reserve(size_t n) { samples.reserve(n); }
    void Clear() { samples.clear(); }
    bool IsEmpty() const { return samples.empty(); }
    size_t Size() const { return samples.size(); }

    double Min() const {
        if (samples.empty()) return 0.0;
        return std::min_element(samples.begin(), samples.end(),
                                [](const MetricSample& a, const MetricSample& b) {
                                    return a.value < b.value;
                                })->value;
    }

    double Max() const {
        if (samples.empty()) return 0.0;
        return std::max_element(samples.begin(), samples.end(),
                                [](const MetricSample& a, const MetricSample& b) {
                                    return a.value < b.value;
                                })->value;
    }

    double Mean() const {
        if (samples.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& s : samples) sum += s.value;
        return sum / static_cast<double>(samples.size());
    }

    double StdDev() const {
        if (samples.size() < 2) return 0.0;
        const double mean = Mean();
        double acc = 0.0;
        for (const auto& s : samples) {
            const double d = s.value - mean;
            acc += d * d;
        }
        return std::sqrt(acc / static_cast<double>(samples.size() - 1));
    }

    // p ∈ [0,1]，线性插值分位数
    double Percentile(double p) const {
        if (samples.empty()) return 0.0;
        std::vector<double> values;
        values.reserve(samples.size());
        for (const auto& s : samples) values.push_back(s.value);
        std::sort(values.begin(), values.end());
        if (p <= 0.0) return values.front();
        if (p >= 1.0) return values.back();
        const double pos = p * static_cast<double>(values.size() - 1);
        const size_t lo = static_cast<size_t>(std::floor(pos));
        const size_t hi = std::min(lo + 1, values.size() - 1);
        const double frac = pos - static_cast<double>(lo);
        return values[lo] + (values[hi] - values[lo]) * frac;
    }

    // 峰值/均值比（衡量波动程度，均值为 0 时返回 0）
    double PeakToMeanRatio() const {
        const double mean = Mean();
        if (std::abs(mean) < 1e-9) return 0.0;
        return Max() / mean;
    }

    // 最近邻取值（用于定位某个时间点上的指标值）
    double ValueAt(double timestamp_seconds) const {
        if (samples.empty()) return 0.0;
        double best = samples.front().value;
        double best_dist = std::abs(samples.front().timestamp_seconds - timestamp_seconds);
        for (const auto& s : samples) {
            const double d = std::abs(s.timestamp_seconds - timestamp_seconds);
            if (d < best_dist) { best_dist = d; best = s.value; }
        }
        return best;
    }

    // 采样间隔（秒），用于把"每秒一个点"这类序列换算回时长
    double DurationSeconds() const {
        if (samples.size() < 2) return 0.0;
        return samples.back().timestamp_seconds - samples.front().timestamp_seconds;
    }
};

} // namespace model
} // namespace videoeye
