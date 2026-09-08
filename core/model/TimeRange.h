#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace videoeye {
namespace model {

// 时间区间（秒）。用于把诊断问题、异常事件定位到具体时间轴位置。
// 约定:
//   - start == end 表示"瞬时点"（如某个异常包）
//   - start < 0 表示"全局/无时间位置"（如容器级问题、整文件统计类问题）
// 秒 -> "HH:MM:SS.mmm"（供诊断文案/报告复用，UI 侧如需本地化格式请自行转换）
inline std::string FormatTimestamp(double seconds) {
    if (seconds < 0) seconds = 0.0;
    const long long total_ms = static_cast<long long>(seconds * 1000.0 + 0.5);
    const long long ms = total_ms % 1000;
    const long long total_sec = total_ms / 1000;
    const long long h = total_sec / 3600;
    const long long m = (total_sec % 3600) / 60;
    const long long s = total_sec % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld", h, m, s, ms);
    return std::string(buf);
}

struct TimeRange {
    double start_seconds = -1.0;
    double end_seconds = -1.0;

    // 构造一个全局（无位置）区间
    static TimeRange Global() { return TimeRange{}; }

    // 构造一个瞬时点
    static TimeRange At(double seconds) { return TimeRange{seconds, seconds}; }

    // 构造一个区间
    static TimeRange Between(double start, double end) { return TimeRange{start, end}; }

    bool IsGlobal() const { return start_seconds < 0.0; }
    bool IsPoint() const { return !IsGlobal() && end_seconds <= start_seconds; }
    bool IsValid() const { return IsGlobal() || end_seconds >= start_seconds; }
    double Duration() const {
        if (IsGlobal()) return 0.0;
        return (end_seconds > start_seconds) ? (end_seconds - start_seconds) : 0.0;
    }
    bool Contains(double seconds) const {
        if (IsGlobal()) return false;
        if (IsPoint()) return std::abs(seconds - start_seconds) < 1e-6;
        return seconds >= start_seconds && seconds <= end_seconds;
    }
    bool Overlaps(const TimeRange& other) const {
        if (IsGlobal() || other.IsGlobal()) return false;
        return start_seconds <= other.end_seconds && other.start_seconds <= end_seconds;
    }

    // 显示用: "00:01:23.456" 或 "00:01:23.456 - 00:01:25.000"，全局返回 "全局"
    std::string ToString() const {
        if (IsGlobal()) return "全局";
        if (IsPoint()) return FormatTimestamp(start_seconds);
        return FormatTimestamp(start_seconds) + " - " + FormatTimestamp(end_seconds);
    }
};

} // namespace model
} // namespace videoeye
