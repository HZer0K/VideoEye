#include "infrastructure/logging/PerfMetrics.h"

#include "infrastructure/logging/Logger.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
// NOMINMAX 必须在 windows.h **之前**：本文件用 std::min/std::max 维护指标的
// min/max，而 windows.h 会把 min/max 定义成宏，于是 std::min(a, b) 被展开成
// 一个语法错误（MSVC 报的是 C2589 ":: 右边的非法标记"，看不出跟宏有关）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// 这两个系统头有顺序要求：psapi.h 依赖 windows.h 先定义 DWORD/WINAPI，
// 按字母序重排会编译失败，所以这一块不能让 clang-format 的 SortIncludes 动。
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
// psapi 的函数实际由 kernel32 转发，但导入库仍要显式链。
// 写在源码里而不是 CMake 里：这个头只被本 TU 用，扩散到构建脚本反而让人
// 以为"整个工程都需要 psapi"。
#pragma comment(lib, "Psapi.lib")
#endif

namespace videoeye {
namespace {

// 三位小数足够：指标是给人看量级的，不是给 profiler 做差分。
std::string FormatValue(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

} // namespace

// ---------------------------------------------------------------------------
// PerfMetrics
// ---------------------------------------------------------------------------

PerfMetrics& PerfMetrics::GetInstance() {
    static PerfMetrics instance;
    return instance;
}

void PerfMetrics::RecordTiming(const std::string& name, double ms, double budget_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Stat& s = stats_[name];
        if (s.count == 0) {
            s.min = ms;
            s.max = ms;
        } else {
            s.min = std::min(s.min, ms);
            s.max = std::max(s.max, ms);
        }
        s.last = ms;
        s.sum += ms;
        ++s.count;
    }

    // 日志在锁外写：WriteLog 自己有锁，且会 flush，别把统计的临界区拖长。
    if (budget_ms > 0.0 && ms > budget_ms) {
        LOG_WARN("[metric][超预算] " + name + "=" + FormatValue(ms) + " ms (budget " + FormatValue(budget_ms) + " ms)");
    } else {
        LOG_INFO("[metric] " + name + "=" + FormatValue(ms) + " ms");
    }
}

void PerfMetrics::RecordGauge(const std::string& name, double value, const std::string& unit) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Stat& s = stats_[name];
        if (s.count == 0) {
            s.min = value;
            s.max = value;
        } else {
            s.min = std::min(s.min, value);
            s.max = std::max(s.max, value);
        }
        s.last = value;
        s.sum += value;
        ++s.count;
    }
    LOG_INFO("[metric] " + name + "=" + FormatValue(value) + " " + unit);
}

void PerfMetrics::RecordCount(const std::string& name, long long delta) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Stat& s = stats_[name];
        s.last = static_cast<double>(delta);
        s.sum += static_cast<double>(delta);
        s.count += delta;
        s.min = std::min(s.min, s.last);
        s.max = std::max(s.max, s.last);
    }
    LOG_INFO("[metric] " + name + " +" + std::to_string(delta));
}

PerfMetrics::Stat PerfMetrics::Snapshot(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = stats_.find(name);
    return it == stats_.end() ? Stat{} : it->second;
}

std::vector<std::string> PerfMetrics::Names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(stats_.size());
    for (const auto& kv : stats_)
        names.push_back(kv.first);
    return names;
}

void PerfMetrics::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.clear();
}

double PerfMetrics::CurrentRssMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return 0.0;
    }
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#else
    std::ifstream in("/proc/self/status");
    if (!in.is_open())
        return 0.0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            // "VmRSS:\t  12345 kB"
            double kb = 0.0;
            try {
                kb = std::stod(line.substr(6));
            } catch (...) {
                return 0.0;
            }
            return kb / 1024.0;
        }
    }
    return 0.0;
#endif
}

double PerfMetrics::PeakRssMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return 0.0;
    }
    // PeakWorkingSetSize 是 OS 维护的高水位，进程启动以来只增不减 ——
    // 不需要自己开线程轮询，也就不会因为轮询本身把内存曲线带歪。
    return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
    std::ifstream in("/proc/self/status");
    if (!in.is_open())
        return 0.0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 6, "VmHWM:") == 0) {
            double kb = 0.0;
            try {
                kb = std::stod(line.substr(6));
            } catch (...) {
                return 0.0;
            }
            return kb / 1024.0;
        }
    }
    return 0.0;
#endif
}

// ---------------------------------------------------------------------------
// PerfScope
// ---------------------------------------------------------------------------

PerfScope::PerfScope(std::string name, double budget_ms)
    : name_(std::move(name)), budget_ms_(budget_ms), start_(std::chrono::steady_clock::now()) {
}

PerfScope::~PerfScope() {
    if (discarded_)
        return;
    PerfMetrics::GetInstance().RecordTiming(name_, ElapsedMs(), budget_ms_);
}

double PerfScope::ElapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
}

// ---------------------------------------------------------------------------
// CancelLatencyProbe
// ---------------------------------------------------------------------------

void CancelLatencyProbe::Arm() {
    armed_ = true;
    settled_ = false;
    armed_at_ = std::chrono::steady_clock::now();
}

double CancelLatencyProbe::ElapsedMs() const {
    if (!armed_)
        return -1.0;
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - armed_at_).count();
}

double CancelLatencyProbe::Settle() {
    if (!armed_ || settled_)
        return -1.0;
    settled_ = true;
    const double ms = ElapsedMs();
    PerfMetrics::GetInstance().RecordTiming(kMetricCancelSettleMs, ms, kCancelSettleBudgetMs);
    return ms;
}

} // namespace videoeye
