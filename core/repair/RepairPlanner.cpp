#include "core/repair/RepairPlanner.h"

#include <algorithm>

#include "core/repair/RepairUtils.h"

namespace videoeye {
namespace repair {
namespace {

model::RepairActionPlan MakeAction(model::RepairAction action, const std::string& description,
                                   const std::string& expected, const std::string& risk) {
    model::RepairActionPlan plan;
    plan.action = action;
    plan.description = description;
    plan.expected_effect = expected;
    plan.risk = risk;
    return plan;
}

void AddUnavailable(model::RepairPlan& plan, model::RepairAction action, const std::string& description,
                    const std::string& reason) {
    model::RepairActionPlan entry = MakeAction(action, description, "（当前版本未实现）", "无法评估");
    entry.available = false;
    entry.unavailable_reason = reason;
    plan.actions.push_back(std::move(entry));
}

} // namespace

RepairPlanOutcome RepairPlanner::MakePlan(const model::RepairRequest& request, const model::RepairDiagnosis& diagnosis,
                                          const std::optional<double>& salvage_end_seconds, int target_stream_index,
                                          const RepairInitSegment* reference_init) {
    RepairPlanOutcome outcome;
    model::RepairPlan& plan = outcome.plan;
    plan.diagnosis_id = diagnosis.diagnosis_id;
    plan.plan_id = "plan-" + FormatLocalRunId();
    plan.mode = request.mode;
    plan.expected_output_extension = "mp4";

    // moov 缺失分两种形态：
    // 1) fMP4 媒体分片序列缺初始化段（styp/moof/mdat）：初始化段是独立传输的，
    //    拿到同源 ftyp+moov 就能前置补全并重封装 —— 这是真实可执行能力；
    // 2) 普通 MP4 的 moov 丢失：当前版本不具备结构重建能力，只能要求参考样片。
    // 必须在"有没有媒体轨"之前判 —— moov 丢失时 FFmpeg 也探测不到任何流，
    // 若先按"无轨道"收尾，会把"需要参考样片"误报成"没有可恢复内容"。
    const bool init_segment_missing =
        std::any_of(diagnosis.findings.begin(), diagnosis.findings.end(),
                    [](const model::DamageFinding& f) { return f.code == "container.init_segment_missing"; });
    const bool moov_missing = init_segment_missing || std::any_of(diagnosis.findings.begin(), diagnosis.findings.end(),
                                                                  [](const model::DamageFinding& f) {
                                                                      return f.code == "container.moov_missing";
                                                                  });
    if (moov_missing && diagnosis.container_known) {
        if (init_segment_missing && reference_init != nullptr && reference_init->ok) {
            if (request.mode == model::RepairMode::RecoverContent) {
                plan.global_preconditions.push_back("当前版本未提供转码路径，救内容模式按保码流执行");
            }
            plan.global_preconditions.push_back("使用参考文件 " + FileName(reference_init->reference_path) +
                                                " 的初始化段（" + std::to_string(reference_init->init_size) +
                                                " 字节）补全 ftyp+moov");
            plan.actions.push_back(
                MakeAction(model::RepairAction::S01_Remux,
                           "前置参考初始化段补全 ftyp+moov，解析源媒体分片并重建索引，重新封装为 MP4",
                           "产物具备完整 moov/样本表，时间轴连续",
                           "参考初始化段与源分片编码参数不匹配时产物无法解码；无法恢复编码层损坏的帧"));
            plan.feasible = true;
            outcome.can_execute = true;
            outcome.early_status = model::RepairBusinessStatus::Repaired;
            return outcome;
        }

        plan.feasible = false;
        plan.infeasible_reason = init_segment_missing
                                     ? "源文件是 fMP4 媒体分片序列，缺少初始化段（ftyp+moov）；"
                                       "需要同源的初始化段或参考样片才能重建索引"
                                     : "缺少 moov，需要参考样片/初始化段才能重建索引（当前版本未实现）";
        outcome.early_status = model::RepairBusinessStatus::NeedsReference;
        return outcome;
    }

    if (target_stream_index < 0) {
        plan.feasible = false;
        plan.infeasible_reason = "没有可用的视频/音频轨";
        outcome.early_status = model::RepairBusinessStatus::NoRecoverableContent;
        return outcome;
    }

    if (!diagnosis.container_known) {
        plan.feasible = false;
        plan.infeasible_reason = "当前版本的结构级修复只支持 ISOBMFF（MP4/MOV/M4V/3GP）";
        outcome.early_status = model::RepairBusinessStatus::Unsupported;
        return outcome;
    }

    if (request.mode == model::RepairMode::RecoverContent) {
        // M1 的救内容仍走"保留码流"这条真实能力，但必须把预期写清楚：
        // 没有转码路径时，它救不出编码层已损坏的内容。
        plan.global_preconditions.push_back("当前版本未提供转码路径，救内容模式按保码流执行");
    }

    plan.actions.push_back(
        MakeAction(model::RepairAction::S01_Remux, "解析源文件的全部可读包并重建索引，重新封装为 MP4",
                   "产物具备完整 moov/样本表，时间轴连续", "无法恢复编码层损坏的帧；索引重建会丢弃不可读的尾部"));

    if (salvage_end_seconds.has_value()) {
        plan.salvage_end_seconds = salvage_end_seconds;
        model::RepairActionPlan salvage =
            MakeAction(model::RepairAction::S02_TailSalvage,
                       "在首个越界样本之前截断，抢救可读前缀（边界 " + FormatSeconds(*salvage_end_seconds) + " s）",
                       "产物在截断点前完整可解码", "截断点之后的内容被放弃；截断点附近的包可能仍不可解码");
        salvage.depends_on.push_back(model::RepairAction::S01_Remux);
        plan.actions.push_back(std::move(salvage));
    }

    AddUnavailable(plan, model::RepairAction::S03_TimelineRepair, "时间戳重复/回退/断裂的显式重写",
                   "当前版本按容器时间基原样复制时间戳，尚未实现时间轴重写");
    AddUnavailable(plan, model::RepairAction::S08_TolerantTranscode, "容忍式转码（解码可读帧后重新编码）",
                   "需要解码-编码链与画质评估，当前版本未实现");
    AddUnavailable(plan, model::RepairAction::S09_SingleTrack, "多轨文件降级为单轨输出",
                   "当前版本按轨道原样复制，尚未实现选择性单轨降级");

    plan.feasible = true;
    outcome.can_execute = true;
    outcome.early_status = model::RepairBusinessStatus::Repaired;
    return outcome;
}

} // namespace repair
} // namespace videoeye
