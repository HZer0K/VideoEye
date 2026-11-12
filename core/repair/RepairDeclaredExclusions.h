#pragma once

// 「本次明确未恢复项」的静态目录与命中评估。
//
// 路线图里的「明确不做」不能只写在文档里：报告必须能回答「这次修复有意
// 没做什么」。这里是纯函数（只读 RepairResult 的结构化字段，不做 IO、不反推
// 自由文本），由 RepairEngine 在**所有**返回路径上统一调用。
//
// 与 RepairCapabilityBoundary 的分工：
//   * Boundary 挂在诊断层，回答「这类损坏有哪些物理上拿不回来的东西」；
//   * DeclaredExclusion 挂在结果层，回答「本次采用的策略明确没做哪些恢复」。
// 两者可以引用同一份路线图结论，但不能互相冒充。

#include <vector>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

// 静态模板（trigger_facts 为空）。覆盖守护用例会检查每条的六个维度都非空，
// 防止以后新增只有 code 的空壳条目。
const std::vector<model::RepairDeclaredExclusion>& AllDeclaredExclusions();

// 只返回本次执行确实命中的条目，把命中的结构化事实逐条写进 trigger_facts。
// available 恒为 true：评估过（items 可能为空）与根本没评估是两种状态。
// 没有事实依据时不得输出「当前不支持」的空壳。
model::RepairDeclaredExclusions BuildDeclaredExclusions(const model::RepairResult& result);

} // namespace repair
} // namespace videoeye
