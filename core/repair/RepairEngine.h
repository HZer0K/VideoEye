#pragma once

// 修复引擎：诊断 → 规划 → 执行 → 验证 → 发布的唯一编排入口。
//
// 引擎内部没有任何 Qt / UI 依赖；界面通过 core/qt/RepairController 调用它。
// 报告由表现层（core/qt 控制器或调用方）用 core/reporting/RepairReportExporter 生成，
// 这样"修复正本"与"报告表现"保持分离。

#include <atomic>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairProgress.h"

namespace videoeye {
namespace repair {

class RepairEngine {
public:
    static model::RepairResult Run(const model::RepairRequest& request, const std::atomic<bool>* cancel,
                                   const RepairProgress& progress);
};

} // namespace repair
} // namespace videoeye
