#pragma once

// 当前版本「已识别但尚未实现」的恢复能力边界。
//
// 这些能力不能用一句「请提供参考样片」打发：用户必须知道哪些内容物理上
// 拿不回来、时间轴会不会被改写、画质要付什么代价。契约是纯证据/纯文案，
// 不做任何 IO，命中判定只看诊断里已经落下的 finding。

#include <vector>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

// 三条契约的静态模板（trigger_findings 为空）。覆盖守护用例会检查每条的
// 四个维度都非空，防止以后出现只有 code 的空壳契约。
const std::vector<model::RepairCapabilityBoundary>& AllCapabilityBoundaries();

// 只返回本次诊断确实命中的契约，并把命中的 finding code 逐条写进 trigger_findings。
// 没有事实依据时返回空 —— 不允许「无事实却宣称当前不支持」。
std::vector<model::RepairCapabilityBoundary> BuildCapabilityBoundaries(const model::RepairDiagnosis& diagnosis);

// 规划层把「不可用策略」关联到契约，让 unavailable_reason 可追溯。
const char* BoundaryCodeForAction(model::RepairAction action);

} // namespace repair
} // namespace videoeye
