#include "core/repair/RepairPlanner.h"

#include <algorithm>

#include "core/repair/RepairCapabilityContracts.h"
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
    const char* boundary = BoundaryCodeForAction(action);
    if (boundary)
        entry.capability_boundary = boundary;
    plan.actions.push_back(std::move(entry));
}

// 第一阶段能力：多段恢复 + 时间轴修复 + 单轨抢救。
// 三者都可以和 S01 叠加执行，因此统一在这里追加，避免 fMP4 分支提前 return
// 把可用策略漏掉（那会让报告显示"只做重封装"，与执行实际行为不符）。
void AddFirstStageActions(model::RepairPlan& plan, const model::RepairRequest& request,
                          const model::RepairDiagnosis& diagnosis) {
    if (request.allow_segment_salvage) {
        model::RepairActionPlan segment =
            MakeAction(model::RepairAction::S04_SegmentSalvage,
                       "读到损坏区间后向后寻找可解码恢复点，跳过不可读区间继续提取后续内容",
                       "产物可能由多个连续片段组成；不完整的前半段与损坏区间会明确记账",
                       "跳段点之后的解码起点可能不是关键帧，恢复点附近可能有短时花屏；被跳过的区间无法找回");
        segment.depends_on.push_back(model::RepairAction::S01_Remux);
        plan.actions.push_back(std::move(segment));
        plan.global_preconditions.push_back(std::string("损坏区间时间轴处理: ") +
                                            model::DisplayName(request.gap_policy) +
                                            (request.gap_policy == model::RepairGapPolicy::PreserveOriginalTimeline
                                                 ? "（保留源时间间隔，中间缺口不补齐）"
                                                 : "（把后续片段平移到前一段末尾，产物时间轴会主动改写）"));
    }

    if (request.repair_timeline) {
        plan.actions.push_back(
            MakeAction(model::RepairAction::S03_TimelineRepair,
                       request.fill_missing_timestamps
                           ? "对重复 / 回退的时间戳做显式单调重写；完全缺失的时间戳按同流上一帧 + 标称帧时长外推合成"
                           : "对重复 / 回退的时间戳做显式单调重写；缺失时间戳只统计，不在无依据时伪造",
                       "产物逐流时间戳单调递增；改写 / 合成数量写入报告",
                       "重写会改变原始时间戳；合成的缺失时间戳是外推值，播放时长可能与源侧真实时间存在偏差"));
    }

    if (request.allow_single_track) {
        std::size_t salvageable = 0;
        for (const auto& track : diagnosis.track_salvage) {
            if (track.salvageable)
                ++salvageable;
        }
        // 证据要求：至少一条轨确认不可用、至少一条轨仍有可用样本、且能映射到输入流。
        std::vector<model::RepairTrackSalvageInfo> dropped;
        for (const auto& track : diagnosis.track_salvage) {
            if (!track.salvageable && track.input_stream_index >= 0)
                dropped.push_back(track);
        }
        if (!dropped.empty() && salvageable > 0) {
            model::RepairActionPlan single = MakeAction(model::RepairAction::S09_SingleTrack,
                                                        "把没有可交付样本的轨道从输出中剔除，只保留仍可恢复的轨道",
                                                        "产物只包含可恢复轨道，避免整份文件因单轨损坏而不可用",
                                                        "被剔除轨道的内容不可找回；音画同步与整体语义可能改变");
            single.depends_on.push_back(model::RepairAction::S01_Remux);
            plan.actions.push_back(std::move(single));
            for (const auto& track : dropped) {
                plan.dropped_input_streams.push_back(track.input_stream_index);
                plan.dropped_track_ids.push_back(track.track_id);
                plan.global_preconditions.push_back("单轨抢救: 剔除轨道 " + std::to_string(track.track_id) + "（" +
                                                    track.kind + "，" + track.evidence + "）");
            }
        }
    }
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
    if (request.limits.user_confirmed_exceed && request.limits.max_seconds > 0)
        plan.global_preconditions.push_back("用户已确认可超出时间预算，本次修复不设硬超时");

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
            AddFirstStageActions(plan, request, diagnosis);
            AddUnavailable(plan, model::RepairAction::S08_TolerantTranscode, "容忍式转码（解码可读帧后重新编码）",
                           "需要解码-编码链与画质评估，当前版本未实现");
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
        // 用户已经给了参考文件但被判定不可用时，不能只说「需要同源初始化段」——
        // 那会让人以为自己没提供文件，把真正的不兼容原因（例如借用的 mdhd 时基
        // 与源侧 trun/tfdt 时长不符）藏起来。诊断里已有 Reader 的具体拒绝理由。
        for (const auto& check : diagnosis.checks) {
            if (check.name == "reference.init_segment" && check.status == model::RepairCheckStatus::Failed &&
                !check.detail.empty()) {
                plan.infeasible_reason = check.detail + "；请改用与源分片同源且时基一致的初始化段";
                break;
            }
        }
        outcome.early_status = model::RepairBusinessStatus::NeedsReference;
        return outcome;
    }

    if (target_stream_index < 0) {
        plan.feasible = false;
        plan.infeasible_reason = "没有可用的视频/音频轨";
        outcome.early_status = model::RepairBusinessStatus::NoRecoverableContent;
        return outcome;
    }

    // 非 ISOBMFF（或盒结构无法解析）：结构级检查不可用，但 FFmpeg 已探测到可读流，
    // 仍可执行通用重封装 + 运行时跳段兜底。计划里必须显式写出这条能力边界，
    // 不能把"没有结构证据"说成"已经做过结构级校验"。
    const bool generic_remux_only = !diagnosis.container_known;
    if (generic_remux_only) {
        plan.global_preconditions.push_back(
            "源容器未识别为 ISOBMFF：跳过盒/样本表级修复，仅执行通用重封装（产物统一为 MP4）；"
            "损伤只能通过运行时读取错误暴露");
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

    AddFirstStageActions(plan, request, diagnosis);
    AddUnavailable(plan, model::RepairAction::S08_TolerantTranscode, "容忍式转码（解码可读帧后重新编码）",
                   "需要解码-编码链与画质评估，当前版本未实现");

    plan.feasible = true;
    outcome.can_execute = true;
    outcome.early_status = model::RepairBusinessStatus::Repaired;
    return outcome;
}

} // namespace repair
} // namespace videoeye
