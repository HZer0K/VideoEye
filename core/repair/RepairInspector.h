#pragma once

// 分层诊断：文件 → 容器 / 索引 → 轨道 / 样本表 → 编码 → 抽样解码 → 时间轴。
//
// 每一层独立记账（RepairCheck），证据写入 DamageFinding；任何一层只能"报告看到
// 什么"，不允许直接给出"可修 / 不可修"的业务结论 —— 那属于 RepairPlanner。

#include <atomic>
#include <optional>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairInitSegment.h"
#include "core/repair/RepairProgress.h"

namespace videoeye {
namespace repair {

struct RepairInspectOutcome {
    model::RepairDiagnosis diagnosis;

    // 诊断阶段选中的主轨（没有媒体轨时为 -1）。
    int target_stream_index = -1;

    // 由样本表推算出的可读内容终点（秒）。仅当存在明确的越界样本时有值，
    // 用于 S02 尾部抢救的截断边界；拿不到时保持空。
    std::optional<double> salvage_end_seconds;

    // 源文件是缺少初始化段的 fMP4 媒体分片、且参考样片成功提供了兼容的
    // ftyp+moov 时才有值；执行器据此在重封装前拼接初始化段。
    std::optional<RepairInitSegment> reference_init;

    // 源媒体分片的独立证据（sidx 时基 / 逐 NAL 码流 / 估算时长），用于核对
    // 参考初始化段，也供报告说明"借用了哪些参数、时间轴有什么风险"。
    RepairSourceCompatibilityEvidence source_evidence;

    // 参考文件的输入快照（R08）：执行时拼进去的必须是诊断时读到的那一份。
    model::RepairInputSnapshot reference_snapshot;
    bool reference_snapshot_valid = false;

    bool canceled = false;
    bool timed_out = false;
};

class RepairInspector {
public:
    static RepairInspectOutcome Inspect(const model::RepairRequest& request, const std::atomic<bool>* cancel,
                                        const RepairProgress& progress, const RepairRunBudget* budget = nullptr);
};

} // namespace repair
} // namespace videoeye
