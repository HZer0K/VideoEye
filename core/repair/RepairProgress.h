#pragma once

// 修复流程的进度回调契约（纯 std，不依赖 Qt / FFmpeg）。
//
// percent 允许在同一阶段内回退（例如诊断阶段先报 30% 完成解析，随后验证阶段重新
// 从 60% 开始），调用方只应把它当作"当前阶段的完成度"，不要拿它做单调性断言。

#include <functional>
#include <string>

namespace videoeye {
namespace repair {

using RepairProgress = std::function<void(int percent, const std::string& stage, const std::string& detail)>;

// 阶段名唯一来源：引擎、Qt 控制器、界面都从这里取，避免三处各写一套中文。
namespace stage {
inline const char* Inspecting() {
    return "诊断";
}
inline const char* Planning() {
    return "制定方案";
}
inline const char* Executing() {
    return "执行修复";
}
inline const char* Verifying() {
    return "验证产物";
}
inline const char* Publishing() {
    return "发布产物";
}
} // namespace stage

} // namespace repair
} // namespace videoeye
