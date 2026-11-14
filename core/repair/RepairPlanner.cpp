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
    // 「本次未启用」≠「当前版本未实现」：S08 已有解码-编码链，保码流模式下只是
    // 用户没选。写成「未实现」会让报告一边执行了 S08、一边声称没有这个能力。
    model::RepairActionPlan entry =
        MakeAction(action, description, "（本次未启用此策略，不产生效果）", "不适用：本次未执行该策略");
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
                                          const RepairInitSegment* reference_init, const RepairRunBudget* budget) {
    RepairPlanOutcome outcome;
    model::RepairPlan& plan = outcome.plan;
    plan.diagnosis_id = diagnosis.diagnosis_id;
    plan.plan_id = "plan-" + FormatLocalRunId();
    plan.mode = request.mode;

    // 规划阶段本身很短，但它是「诊断已耗时」之后的第一道门：预算在这里就该生效，
    // 否则一次已经超时的修复仍会拿到可执行计划继续写产物。
    if (const std::string stop = BudgetStopMessage(budget, "规划阶段"); !stop.empty()) {
        outcome.can_execute = false;
        outcome.early_status =
            budget->Canceled() ? model::RepairBusinessStatus::Canceled : model::RepairBusinessStatus::TimedOut;
        plan.feasible = false;
        plan.infeasible_reason = stop;
        return outcome;
    }
    switch (request.output_container) {
    case model::RepairOutputContainer::Mkv:
        plan.expected_output_extension = "mkv";
        break;
    case model::RepairOutputContainer::Ts:
        plan.expected_output_extension = "ts";
        break;
    case model::RepairOutputContainer::Mp4:
        plan.expected_output_extension = "mp4";
        break;
    case model::RepairOutputContainer::Auto:
        plan.expected_output_extension = LowerExtension(request.source_path) == "mov" ? "mov" : "mp4";
        break;
    }
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
        const bool mdat_unindexed =
            std::any_of(diagnosis.findings.begin(), diagnosis.findings.end(),
                        [](const model::DamageFinding& f) { return f.code == "container.mdat_unindexed"; });
        if (mdat_unindexed) {
            // P2-12：普通 MP4 缺 moov，但 mdat 载荷可无参考扫描重建。
            plan.global_preconditions.push_back(
                "普通 MP4 缺少 moov：执行前扫描单个 mdat 重建 ftyp+moov/样本表（仅支持纯 H.264 或纯 ADTS AAC 单轨）");
            plan.actions.push_back(MakeAction(
                model::RepairAction::S01_Remux, "无参考扫描 mdat 载荷，重建 ftyp+moov/stbl 后再重新封装",
                "产物具备可读索引；样本边界由 NAL/ADTS 帧结构实测得出",
                "样本边界是推断值；音视频交织的 mdat 无法拆分；不重建 edts/ctts，含 B 帧时显示顺序可能不符"));
            AddFirstStageActions(plan, request, diagnosis);
            if (request.mode == model::RepairMode::RecoverContent) {
                plan.actions.push_back(
                    MakeAction(model::RepairAction::S08_TolerantTranscode,
                               "解码可读视频帧后重新编码为 H.264（恒定帧率），音频按目标容器的实测接受性复制或重编码",
                               "编码层已损坏但仍有可解码帧的视频可以重新生成可播放产物",
                               "重编码有代际画质损失；时间轴按推定帧率重写"));
            } else {
                AddUnavailable(plan, model::RepairAction::S08_TolerantTranscode, "容忍式转码（解码可读帧后重新编码）",
                               "保码流模式不重编码；切换到救内容模式才启用");
            }
            plan.feasible = true;
            outcome.can_execute = true;
            outcome.early_status = model::RepairBusinessStatus::Repaired;
            return outcome;
        }
        if (init_segment_missing && reference_init != nullptr && reference_init->ok) {
            if (request.mode == model::RepairMode::RecoverContent) {
                plan.global_preconditions.push_back("源分片缺初始化段，无法直接打开解码；本策略仍按保码流执行");
            }
            const bool synthesized = reference_init->synthesized;
            if (synthesized) {
                plan.global_preconditions.push_back("未提供参考样片：由源分片 in-band SPS/PPS 自建初始化段（" +
                                                    std::to_string(reference_init->init_size) + " 字节），时基证据：" +
                                                    reference_init->timescale_check);
                plan.actions.push_back(
                    MakeAction(model::RepairAction::S01_Remux,
                               "用 in-band 参数集自建 ftyp+moov，解析源媒体分片并重建索引，重新封装为 MP4",
                               "产物具备完整 moov/样本表；时基在自建时已按 sidx/分片声明交叉核对",
                               "自建初始化段覆盖 H.264/H.265 视频轨；时基推断错误会静默改变播放速度；"
                               "音频轨因缺少 ASC 不参与自建"));
            } else {
                plan.global_preconditions.push_back("使用参考文件 " + FileName(reference_init->reference_path) +
                                                    " 的初始化段（" + std::to_string(reference_init->init_size) +
                                                    " 字节）补全 ftyp+moov");
                plan.actions.push_back(
                    MakeAction(model::RepairAction::S01_Remux,
                               "前置参考初始化段补全 ftyp+moov，解析源媒体分片并重建索引，重新封装为 MP4",
                               "产物具备完整 moov/样本表，时间轴连续",
                               "参考初始化段与源分片编码参数不匹配时产物无法解码；无法恢复编码层损坏的帧"));
            }
            AddFirstStageActions(plan, request, diagnosis);
            AddUnavailable(plan, model::RepairAction::S08_TolerantTranscode, "容忍式转码（解码可读帧后重新编码）",
                           "源分片缺初始化段时无法直接解码，需要先补全初始化段才能转码");
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
        plan.global_preconditions.push_back(
            "救内容模式：视频轨将解码可读帧后重新编码（S08），产物存在代际画质损失，时间轴按推定帧率重写");
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
    if (request.mode == model::RepairMode::RecoverContent) {
        plan.actions.push_back(
            MakeAction(model::RepairAction::S08_TolerantTranscode,
                       "解码可读视频帧后重新编码为 H.264（恒定帧率），音频按目标容器的实测接受性复制或重编码，"
                       "并对产物做抽帧 PSNR 画质评估",
                       "编码层已损坏但仍有可解码帧的视频可以重新生成可播放产物；解码失败的帧会被记账",
                       "重编码存在代际画质损失；时间轴按推定帧率重写，可能与源不一致；被解码失败的帧内容无法恢复"));
    } else {
        AddUnavailable(plan, model::RepairAction::S08_TolerantTranscode, "容忍式转码（解码可读帧后重新编码）",
                       "保码流模式不重编码；切换到救内容模式才启用");
    }

    plan.feasible = true;
    outcome.can_execute = true;
    outcome.early_status = model::RepairBusinessStatus::Repaired;
    return outcome;
}

} // namespace repair
} // namespace videoeye
