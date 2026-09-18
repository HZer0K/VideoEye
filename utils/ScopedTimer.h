#pragma once

// 轻量耗时埋点：析构时把一段代码的耗时打到日志（毫秒）。
// 仅用于定位卡顿/性能瓶颈，默认通过 VE_PERF 宏控制，未定义时在 Release 下零成本。

#include <chrono>
#include <string>

#include "utils/Logger.h"

namespace videoeye {
namespace utils {

class ScopedTimer {
public:
    explicit ScopedTimer(std::string name)
        : name_(std::move(name)),
          start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        const double ms = ElapsedMs();
        // 低于 1ms 的片段不记录，避免刷屏
        if (ms >= 1.0) {
            LOG_INFO("[perf] " + name_ + " 耗时 " + std::to_string(static_cast<long long>(ms)) + " ms");
        }
    }

    double ElapsedMs() const {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace utils
}  // namespace videoeye

// 用法: { VE_PERF("MediaInfo 解析"); ... }
#define VE_PERF_CONCAT_IMPL(a, b) a##b
#define VE_PERF_CONCAT(a, b) VE_PERF_CONCAT_IMPL(a, b)
#define VE_PERF(name) ::videoeye::utils::ScopedTimer VE_PERF_CONCAT(_ve_perf_timer_, __LINE__)(name)
