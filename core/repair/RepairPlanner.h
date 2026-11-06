#pragma once

// 策略规划：把"诊断看到了什么"翻译成"这次允许做什么"。
//
// 规划器不执行任何 IO。它只为当前版本真正具备执行能力的策略发可执行计划；
// S08（容忍式转码）仍作为"尚未实现"的能力清单出现，不允许被选为执行动作；
// S03 / S04 / S09 已具备执行能力，但必须由诊断证据或用户开关授权。

#include <optional>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairInitSegment.h"

namespace videoeye {
namespace repair {

struct RepairPlanOutcome {
    model::RepairPlan plan;
    model::RepairBusinessStatus early_status = model::RepairBusinessStatus::Repaired;
    bool can_execute = false;
};

class RepairPlanner {
public:
    // reference_init：诊断阶段成功从参考样片里取出的初始化段（可空）。
    // 只有源文件是缺少初始化段的 fMP4 媒体分片序列时它才会被使用。
    static RepairPlanOutcome MakePlan(const model::RepairRequest& request, const model::RepairDiagnosis& diagnosis,
                                      const std::optional<double>& salvage_end_seconds, int target_stream_index,
                                      const RepairInitSegment* reference_init);
};

} // namespace repair
} // namespace videoeye
