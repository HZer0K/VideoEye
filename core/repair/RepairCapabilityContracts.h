#pragma once

// 恢复能力的边界契约（已识别能力：做到哪、没做哪、哪些根本做不到）。
//
// 这些能力不能用一句「请提供参考样片」打发：用户必须知道哪些内容物理上
// 拿不回来、时间轴会不会被改写、画质要付什么代价。契约是纯证据/纯文案，
// 不做任何 IO，命中判定只看诊断里已经落下的 finding。
//
// ❗ 三条契约现在**都有实现**（受限范围），所以 status 统一是 partially_supported。
//    过去一律写 unsupported，而 evidence 里又写着「已实现…」——同一份报告自相矛盾，
//    读者只看到状态词就会读成「这次没做到」。

#include <string>
#include <vector>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

// 状态词表（唯一事实源）：partially_supported / unsupported / deferred。
// 新增取值必须同时扩 CapabilityStatusLabel() 与守护用例；词表外的取值会被渲染成
// 「未知状态…不能据此判断支持程度」，不允许静默落成「支持」或「不支持」。
const std::vector<std::string>& CapabilityStatusVocabulary();
bool IsKnownCapabilityStatus(const std::string& status);

// 状态词 → 人类可读标签。只是词，不含内容判断。
std::string CapabilityStatusLabel(const std::string& status);

// 一条契约的支持程度总述：唯一渲染入口。报告层、限制项、JSON 都从这里取，
// 不许各自按 status 拼文案（拼出来的第二份必然与第一份漂移）。
std::string CapabilityStatusHeadline(const model::RepairCapabilityBoundary& boundary);

// 三条契约的静态模板（trigger_findings 为空）。覆盖守护用例会检查每条的
// 五个维度都非空、且状态词与「已覆盖 / 未覆盖」两张表互不矛盾。
const std::vector<model::RepairCapabilityBoundary>& AllCapabilityBoundaries();

// 只返回本次诊断确实命中的契约，并把命中的 finding code 逐条写进 trigger_findings。
// 没有事实依据时返回空 —— 不允许「无事实却宣称当前不支持」。
std::vector<model::RepairCapabilityBoundary> BuildCapabilityBoundaries(const model::RepairDiagnosis& diagnosis);

// 规划层把「不可用策略」关联到契约，让 unavailable_reason 可追溯。
const char* BoundaryCodeForAction(model::RepairAction action);

} // namespace repair
} // namespace videoeye
