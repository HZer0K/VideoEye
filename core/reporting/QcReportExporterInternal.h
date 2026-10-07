#pragma once

// QcReportExporter 家族跨编译单元共享的小工具。
//
// 这些 helper 原本都挤在 QcReportExporter.cpp 的匿名 namespace 里，物理拆分后
// 有 2 个以上 TU 要用，所以提取到这里。全部 inline（头内定义），不会有 ODR 问题。
//
// 只被单个格式用到的 helper（Html / CsvField / kRowGroupSeparator / JSON 的
// JNumber / JText 及各种 builder）继续留在各自 TU 的匿名 namespace 里，不进这里。

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace reporting {
namespace detail {

// JSON schema 的版本号。只要字段语义有破坏性变化就加一，
// CI 侧据此决定要不要兼容旧结构。
inline constexpr int kReportSchemaVersion = 1;

inline std::string Fixed(double value, int precision) {
    if (!std::isfinite(value)) return std::string();
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(precision) + "f").c_str(), value);
    return std::string(buf);
}

inline std::string Int64Text(long long value) { return std::to_string(value); }

// CSV/PDF 里换行会把一行拆成两行，而 detail/suggestion 里有换行（如 FFmpeg 命令行示例）。
inline std::string SingleLine(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

inline bool WriteUtf8File(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("无法写入报告文件: " + path);
        return false;
    }
    file << content;
    return file.good();
}

}  // namespace detail
}  // namespace reporting
}  // namespace videoeye