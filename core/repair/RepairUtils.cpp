#include "core/repair/RepairUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <random>

namespace videoeye {
namespace repair {

std::string FormatIso8601Now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", tm_local.tm_year + 1900, tm_local.tm_mon + 1,
                  tm_local.tm_mday, tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    return std::string(buffer);
}

std::string FormatLocalRunId() {
    // 时间戳 + 4 位随机后缀：同一秒内连续发起两次修复也不会撞名。
    const std::string stamp = FormatIso8601Now();
    std::string digits;
    digits.reserve(stamp.size());
    for (char c : stamp) {
        if (c >= '0' && c <= '9')
            digits.push_back(c);
    }
    std::random_device rd;
    const unsigned suffix = rd() % 10000u;
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u", suffix);
    return digits + "-" + std::string(buffer);
}

uint64_t Fnv1a64Begin() {
    return 1469598103934665603ULL;
}

uint64_t Fnv1a64Update(uint64_t state, const std::uint8_t* data, std::size_t size) {
    if (!data)
        return state;
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (std::size_t i = 0; i < size; ++i) {
        state ^= static_cast<uint64_t>(data[i]);
        state *= kPrime;
    }
    return state;
}

std::string LowerExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size())
        return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string FileStem(const std::string& path) {
    const std::string name = FileName(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return name;
    return name.substr(0, dot);
}

std::string FileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool SamePath(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty())
        return false;
    std::error_code ec_a;
    std::error_code ec_b;
    if (std::filesystem::exists(a, ec_a) && std::filesystem::exists(b, ec_b)) {
        return std::filesystem::equivalent(a, b, ec_a);
    }
    return std::filesystem::path(a).lexically_normal() == std::filesystem::path(b).lexically_normal();
}

std::string FormatSeconds(double seconds) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
    return std::string(buffer);
}

bool SampleLossExceedsTolerance(std::uint64_t expected_samples, std::uint64_t packets_written) {
    if (expected_samples == 0 || packets_written >= expected_samples)
        return false;
    // 写成加法而不是 expected*0.95，避免大数乘浮点、也不必引入 <cmath>。
    return packets_written + expected_samples / 20 < expected_samples;
}

} // namespace repair
} // namespace videoeye
