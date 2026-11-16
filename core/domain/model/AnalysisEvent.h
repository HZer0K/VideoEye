#pragma once

#include <cstdint>
#include <string>

namespace videoeye {
namespace model {

/// 异常事件级别。
///
/// 采集侧与消费侧统一按枚举判断/归档，显示文本只在 UI 层做一次映射。
/// 不要把本地化字符串存进结构体再比较：切换语言或接入其它产源（"error"/"warning"）
/// 时，统计会静默把错误记成"信息"。
enum class AnalysisEventSeverity {
    Info = 0,
    Warning = 1,
    Error = 2,
};

struct AnalysisEvent {
    int index = 0;
    AnalysisEventSeverity severity = AnalysisEventSeverity::Info;
    std::string type;
    int stream_index = -1;
    int64_t pts = 0;
    double timestamp_seconds = 0.0;
    std::string summary;
    std::string detail;
};

} // namespace model
} // namespace videoeye
