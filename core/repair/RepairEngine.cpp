#include "core/repair/RepairEngine.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "core/domain/io/Utf8Path.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairDeclaredExclusions.h"
#include "core/repair/RepairExecutor.h"
#include "core/repair/RepairInspector.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairPlanner.h"
#include "core/repair/RepairUtils.h"
#include "core/repair/RepairVerifier.h"
#include "infrastructure/concurrency/Cancellation.h"

namespace videoeye {
namespace repair {
namespace {

void Emit(const RepairProgress& progress, int percent, const std::string& stage_name, const std::string& detail) {
    if (progress)
        progress(percent, stage_name, detail);
}

void FinishEarly(model::RepairResult& result, model::RepairBusinessStatus status, const std::string& summary) {
    result.status = status;
    result.summary = summary;
    result.created_at_iso8601 = FormatIso8601Now();
    result.fidelity.perceptual_fidelity = "unknown_without_reference";
}

// 损失区间是 recovered_segments 的派生视图，不是第二份真值表：
// 只有「跳段恢复且确实跳过了一段时间」的段才产生一条，正向 / 反向都由实测值决定。
std::vector<model::RepairLostRange> BuildLostRanges(const std::vector<model::RepairRecoveredSegment>& segments) {
    std::vector<model::RepairLostRange> out;
    for (const auto& segment : segments) {
        if (!segment.resumed_after_damage || segment.skipped_before_seconds <= 0.0)
            continue;
        model::RepairLostRange range;
        range.input_end_seconds = segment.input_start_seconds;
        range.input_start_seconds = std::max(0.0, segment.input_start_seconds - segment.skipped_before_seconds);
        range.reason = "container.damaged_segment_skipped";
        range.evidence = "跳段恢复实测：本段前跳过 " + FormatSeconds(segment.skipped_before_seconds) +
                         " s；恢复点来源 " + std::string(model::ToString(segment.recovery_evidence)) + "；恢复点 " +
                         (segment.recovery_point.empty() ? "未记录" : segment.recovery_point);
        range.recoverable = false;
        out.push_back(std::move(range));
    }
    return out;
}

std::string BuildSummary(const model::RepairResult& result) {
    std::ostringstream summary;
    summary << model::DisplayName(result.status);
    if (!result.output_path.empty())
        summary << "；产物: " << result.output_path;
    if (!result.executions.empty())
        summary << "；写入 " << result.executions.front().packets_written << " 个包";
    if (result.verification.status != model::RepairCheckStatus::NotRun)
        summary << "；" << model::DisplayName(result.verification.level) << " " << ToString(result.verification.status);
    if (result.verification.structure_only)
        summary << "（仅结构验证，尚未验证播放）";
    if (result.fidelity.timeline_status == "unknown")
        summary << "；时间轴未核验";
    else if (result.fidelity.timeline_status == "modified")
        summary << "；时间轴实测不符";
    else if (result.fidelity.timeline_status == "adjusted")
        summary << "；时间轴已调整";
    if (!result.recovered_segments.empty()) {
        summary << "；恢复区间 " << result.recovered_segments.size() << " 段";
        double skipped = 0.0;
        for (const auto& segment : result.recovered_segments)
            skipped += std::max(0.0, segment.skipped_before_seconds);
        if (skipped > 0.0) {
            summary << "（跳过约 " << FormatSeconds(skipped) << " s）";
        }
    }
    if (!result.lost_ranges.empty())
        summary << "；损失区间 " << result.lost_ranges.size() << " 段";
    if (!result.plan.dropped_input_streams.empty())
        summary << "；单轨抢救省略 " << result.plan.dropped_input_streams.size() << " 条轨";
    if (!result.verification.per_track.empty())
        summary << "；逐轨 " << result.verification.per_track.size() << " 条";
    if (!result.residual_findings.empty())
        summary << "；残留问题 " << result.residual_findings.size() << " 条";
    return summary.str();
}

// 临时产物的所有权守卫：Reserve 成功后，任何失败 / 取消 / 异常路径都会清理它。
// 只有 Publish 成功才解除，避免"失败清理"误删用户文件（它一定是我们自己创建的）。
class PartGuard {
public:
    explicit PartGuard(std::string path) : path_(std::move(path)) {
    }
    ~PartGuard() {
        if (!released_)
            RepairOutputWriter::Discard(path_);
    }
    PartGuard(const PartGuard&) = delete;
    PartGuard& operator=(const PartGuard&) = delete;
    void Release() {
        released_ = true;
    }

private:
    std::string path_;
    bool released_ = false;
};

// 预算命中时区分"用户取消"与"超出时间预算"。
model::RepairBusinessStatus BudgetTerminal(const RepairRunBudget& budget) {
    return budget.Canceled() ? model::RepairBusinessStatus::Canceled : model::RepairBusinessStatus::TimedOut;
}

model::RepairResult RunCore(const model::RepairRequest& request, const std::atomic<bool>* cancel,
                            const RepairProgress& progress) {
    model::RepairResult result;
    result.request_id = request.request_id.empty() ? "req-" + FormatLocalRunId() : request.request_id;
    result.result_id = "result-" + FormatLocalRunId();
    result.created_at_iso8601 = FormatIso8601Now();
    result.ffmpeg_version.clear();
    result.fidelity.perceptual_fidelity = "unknown_without_reference";

    // R10：一次修复只有一个共享截止时间，从入口一直贯穿到发布提交。
    // max_seconds 是「提示阈值」而非绝对上限：用户已确认可超出时，不再设置硬超时。
    const bool overrun_confirmed = request.limits.user_confirmed_exceed && request.limits.max_seconds > 0;
    const int64_t budget_seconds = overrun_confirmed ? 0 : request.limits.max_seconds;
    const RepairRunBudget budget(cancel, budget_seconds);
    result.input_check_method = "size+mtime+内容摘要（方法在发布前复核时回填）";

    // ---- 0. 请求预检 + 产物路径解析 + 独占预留（R01）----
    Emit(progress, 0, stage::Inspecting(), "检查请求与产物路径");
    // 切片 F：显式帧率只接受完整分数。只给分子/分母是调用方写错了 ——
    // 静默当成"没指定"会让用户以为指定生效了，然后拿到一个按别的帧率建的产物。
    if (request.AssumedFrameRateIncomplete()) {
        FinishEarly(result, model::RepairBusinessStatus::Failed,
                    "显式帧率参数不完整（num=" + std::to_string(request.assumed_frame_rate_num) + "，den=" +
                        std::to_string(request.assumed_frame_rate_den) + "）；分子与分母必须同时给出，或都不给");
        result.error = result.summary;
        return result;
    }
    const std::string run_id = result.result_id.substr(std::string("result-").size());
    const RepairOutputPaths paths = RepairOutputWriter::Resolve(request, run_id);
    result.report_json_path = paths.report_json_path;
    result.report_text_path = paths.report_text_path;
    result.report_html_path = paths.report_html_path;
    if (!paths.valid) {
        FinishEarly(result, model::RepairBusinessStatus::Failed, "请求无效: " + paths.error);
        result.error = paths.error;
        return result;
    }
    std::string reserve_error;
    if (!RepairOutputWriter::Reserve(paths, &reserve_error)) {
        FinishEarly(result, model::RepairBusinessStatus::Failed, "无法预留临时产物: " + reserve_error);
        result.error = reserve_error;
        return result;
    }
    PartGuard part_guard(paths.part_path);

    auto finish_failed = [&](model::RepairBusinessStatus status, const std::string& message) {
        FinishEarly(result, status, message);
        if (status == model::RepairBusinessStatus::Failed)
            result.error = message;
        return result;
    };

    // ---- 1. 诊断 ----
    const RepairInspectOutcome inspect = RepairInspector::Inspect(request, cancel, progress, &budget);
    result.diagnosis = inspect.diagnosis;
    result.diagnosis_id = inspect.diagnosis.diagnosis_id;
    result.input = inspect.diagnosis.input;
    result.reference_input = inspect.reference_snapshot;
    result.reference_snapshot_valid = inspect.reference_snapshot_valid;

    if (inspect.canceled || budget.Checkpoint())
        return finish_failed(BudgetTerminal(budget), budget.Canceled() ? "诊断阶段被取消" : "诊断阶段超出本次修复预算");

    // ---- 2. 规划 ----
    Emit(progress, 72, stage::Planning(), "制定修复计划");
    RepairPlanOutcome planning =
        RepairPlanner::MakePlan(request, inspect.diagnosis, inspect.salvage_end_seconds, inspect.target_stream_index,
                                inspect.reference_init ? &*inspect.reference_init : nullptr, &budget);
    result.plan = planning.plan;
    result.plan_id = planning.plan.plan_id;

    if (!planning.can_execute) {
        const std::string reason =
            planning.plan.infeasible_reason.empty() ? "没有可执行的修复策略" : planning.plan.infeasible_reason;
        result.error = reason;
        std::string summary = reason;
        if (!result.diagnosis.summary.empty())
            summary += "；" + result.diagnosis.summary;
        return finish_failed(planning.early_status, summary);
    }
    if (budget.Checkpoint())
        return finish_failed(BudgetTerminal(budget), "规划阶段被取消或超出本次修复预算");

    // ---- 3. 执行前复核输入快照（R08：源 + 参考）----
    Emit(progress, 75, stage::Executing(), "开始重封装");
    std::string source_error;
    std::string source_method;
    if (!RepairOutputWriter::SourceUnchanged(inspect.diagnosis.input, &source_error, &source_method, &budget))
        return finish_failed(model::RepairBusinessStatus::Failed, source_error);
    result.input_check_method = "源文件: " + source_method;
    if (inspect.reference_snapshot_valid) {
        std::string reference_error;
        std::string reference_method;
        if (!RepairOutputWriter::SourceUnchanged(inspect.reference_snapshot, &reference_error, &reference_method,
                                                 &budget))
            return finish_failed(model::RepairBusinessStatus::Failed, reference_error);
        result.input_check_method += "；参考文件: " + reference_method;
    }
    result.input_check_method += "；摘要是发现改动的证据，不是密码学完整性证明；单次解码调用可能越过时间预算";

    RepairExecutionOutcome execution =
        RepairExecutor::Execute(request, inspect.diagnosis, planning.plan, paths,
                                inspect.reference_init ? &*inspect.reference_init : nullptr, cancel, progress, &budget);
    result.executions = execution.executions;
    result.ffmpeg_version = execution.ffmpeg_version;
    result.residual_findings = execution.residual_findings;
    result.track_conservation = execution.track_conservation;
    result.sample_ledger = execution.sample_ledger;
    result.sample_conservation_problems = execution.sample_conservation_problems;
    result.recovered_segments = execution.recovered_segments;
    result.lost_ranges = BuildLostRanges(result.recovered_segments);
    // 切片 G：逐轨覆盖 / 逐轨缺口 / 音画共同区间，由执行期样本账本流式导出。
    result.track_coverage = execution.track_coverage;
    result.timeline_repair = execution.timeline_repair;
    result.reference_assist = execution.reference_assist;
    result.display_order = execution.display_order;
    result.timeline_basis = execution.timeline_basis;
    result.fidelity.omitted_items = execution.omitted_items;

    // P2-14b：取消 / 超预算时按需保留已经写入的产物前缀。
    // 该前缀未完成验证，绝不能当作"修复完成"：状态仍是 Canceled/TimedOut，
    // 产物以 *.partial.<ext> 另存，报告标注为未验证前缀。
    auto finish_with_partial = [&](model::RepairBusinessStatus status, const std::string& message) {
        std::string summary = message;
        if (request.keep_partial_on_cancel && execution.packets_written > 0) {
            const std::string partial_path = RepairOutputWriter::PartialPath(paths.final_path);
            const std::string output_mime = paths.extension == "mkv"  ? "video/x-matroska"
                                            : paths.extension == "ts" ? "video/mp2t"
                                                                      : "video/mp4";
            std::string publish_error;
            if (RepairOutputWriter::Publish(paths.part_path, partial_path, &publish_error)) {
                part_guard.Release();
                result.output_path = partial_path;
                result.output_published = true;
                result.output_container = paths.extension;
                std::error_code partial_ec;
                // UTF-8 路径必须先转原生：file_size(utf8 std::string) 在 Windows 上恒失败
                // （ec = path not found），产物大小会被记成 0。
                const uint64_t partial_size = std::filesystem::file_size(ToNativePath(partial_path), partial_ec);
                result.artifacts.push_back(
                    model::RepairArtifact{"output_partial", partial_path, output_mime,
                                          "bytes [0, " + std::to_string(partial_ec ? 0 : partial_size) + ")"});
                result.fidelity.limitations.push_back("产物是取消 / 超预算时保留的已写前缀（" +
                                                      std::to_string(execution.packets_written) +
                                                      " 个包），未完成验证，不能当作修复完成；请按需人工复核");
                result.fidelity.content_completeness = "partial";
                result.fidelity.timeline_status = "unknown";
                result.fidelity.timeline_note = "产物前缀未完成验证，时间轴未核验";
                summary += "；已保留未验证的已写前缀: " + partial_path;
            } else {
                result.fidelity.limitations.push_back("取消时未能保留已写前缀: " + publish_error);
                summary += "；未能保留已写前缀: " + publish_error;
            }
        }
        return finish_failed(status, summary);
    };

    if (execution.canceled || (budget.Checkpoint() && budget.Canceled()))
        return finish_with_partial(model::RepairBusinessStatus::Canceled, "执行阶段被取消，未发布完整产物");
    if (execution.timed_out || (budget.Checkpoint() && !budget.Canceled()))
        return finish_with_partial(model::RepairBusinessStatus::TimedOut, "超出本次修复预算，未发布完整产物");
    if (!execution.ok) {
        result.error = execution.error.empty() ? "执行失败" : execution.error;
        std::string summary = result.error;
        if (result.error.find("没有写入任何数据包") != std::string::npos ||
            result.error.find("没有可复制") != std::string::npos)
            return finish_failed(model::RepairBusinessStatus::NoRecoverableContent, summary);
        if (result.error.find("无法打开源文件") != std::string::npos)
            return finish_failed(model::RepairBusinessStatus::Unsupported, summary);
        if (result.error.find("无参考 moov 重建失败") != std::string::npos ||
            result.error.find("moov 重建临时文件") != std::string::npos)
            return finish_failed(model::RepairBusinessStatus::NeedsReference, summary);
        return finish_failed(model::RepairBusinessStatus::Failed, summary);
    }

    // ---- 3b. 逐轨样本守恒（R07）----
    // 不再用「全轨合计 + 5% 容差」：一条大轨会把小轨整轨丢失掩盖掉。
    bool conservation_loss = false;
    bool conservation_unknown = false;
    if (inspect.reference_init.has_value() && !result.diagnosis.expected_tracks.empty()) {
        for (auto& record : result.track_conservation) {
            const model::RepairExpectedTrack* expected = nullptr;
            for (const auto& candidate : result.diagnosis.expected_tracks) {
                if (candidate.track_id != 0 && candidate.track_id == record.track_id) {
                    expected = &candidate;
                    break;
                }
            }
            if (expected == nullptr && result.diagnosis.expected_tracks.size() == 1 &&
                result.track_conservation.size() == 1)
                expected = &result.diagnosis.expected_tracks.front();
            if (expected == nullptr) {
                record.status = "unmapped";
                record.note = "无法把输出轨映射到源侧 track_id，本轨样本守恒无法判定";
                conservation_unknown = true;
                continue;
            }
            record.expected_samples = expected->sample_count;
            record.status = EvaluateTrackConservation(record.expected_samples, record.packets_written);
            record.relation = ConservationRelation(record.expected_samples, record.packets_written);
            if (record.status == "loss") {
                conservation_loss = true;
                model::DamageFinding finding;
                finding.code = "container.track_samples_dropped_by_demuxer";
                finding.detector = "repair.engine";
                finding.layer = model::DamageLayer::Container;
                finding.severity = model::IssueSeverity::Error;
                finding.confidence = model::DamageConfidence::Confirmed;
                finding.location.precision = model::DamagePrecision::Bounded;
                finding.location.track_id = record.track_id;
                finding.location.stream_index = record.output_stream_index;
                finding.location.provenance = "moof/trun declared samples vs per-track av_interleaved_write_frame";
                finding.evidence = "轨道 " + std::to_string(record.track_id) + " 源侧声明 " +
                                   std::to_string(record.expected_samples) + " 个样本，实际只写入 " +
                                   std::to_string(record.packets_written) + " 个包（少 " +
                                   std::to_string(record.expected_samples - record.packets_written) + " 个）";
                finding.limitations.push_back("只能证明包数不足，不能定位丢失的是哪一段");
                finding.limitations.push_back("载荷哈希只覆盖实际写入的包，不能证明源内容完整");
                result.residual_findings.push_back(std::move(finding));
            } else if (record.status == "excess" || record.status == "unmapped") {
                conservation_unknown = true;
                record.note = "写入包数多于源侧声明，可能是访问单元拆分，也可能是重复写入；无法判定完整";
            }
        }
    }

    // ---- 4. 验证（逐目标轨，R04 / R05 / R09）----
    // MPEG-TS 需要 Annex B 转换、S08 转码会重建视频帧：两者都让**视频**载荷与
    // 输入必然不同，此时不能再用视频载荷哈希断言"码流未被改动"，否则会把一次
    // 合法的容器转换 / 画质有损的重编码误判成保真度失败。
    // 音频不同：TS 的 AAC 只是给 MP4 的裸访问单元套了 ADTS 头，两侧都先做
    // 访问单元规范化再累计哈希，仍可正常核对（见 RepairPayloadHash.h）。
    // 没有规范化能力的编码由验证器明确降级为「原始载荷未验证」，不判 Failed。
    // 输入是 TS 时同样会做 Annex B → AVCC 转换（TS→MP4），视频包字节也必然变。
    // 判定必须由 demuxer 的 format name 给出，不能看扩展名（fMP4 分片常叫 *.ts）。
    const bool input_is_ts = execution.input_is_mpegts;
    const bool payload_transformed = paths.extension == "ts" || input_is_ts || execution.video_reencoded;
    std::optional<uint64_t> expected_hash;
    if (!payload_transformed) {
        for (const auto& action : execution.executions) {
            if (action.action == model::RepairAction::S01_Remux && action.input_video_payload_hash.has_value()) {
                expected_hash = action.input_video_payload_hash;
                break;
            }
        }
    }
    Emit(progress, 89, stage::Verifying(), "开始验证产物");
    RepairVerifyOutcome verified = RepairVerifier::Verify(
        paths.part_path, request.verification_level, expected_hash, cancel, progress, execution.verification_targets,
        &budget, inspect.source_evidence.estimated_duration_seconds, execution.input_audio_payload_hash);
    result.verification = verified.verification;
    result.fidelity.video = verified.fidelity.video;
    result.fidelity.audio = verified.fidelity.audio;
    for (const auto& limitation : verified.fidelity.limitations)
        result.fidelity.limitations.push_back(limitation);
    result.residual_findings.insert(result.residual_findings.end(), verified.verification.residual_findings.begin(),
                                    verified.verification.residual_findings.end());

    if (verified.canceled || (budget.Checkpoint() && budget.Canceled()))
        return finish_with_partial(model::RepairBusinessStatus::Canceled, "验证阶段被取消，未发布完整产物");
    if (verified.timed_out || (budget.Checkpoint() && !budget.Canceled()))
        return finish_with_partial(model::RepairBusinessStatus::TimedOut, "超出本次修复预算，未发布完整产物");
    if (verified.verification.status == model::RepairCheckStatus::Failed)
        return finish_failed(model::RepairBusinessStatus::Failed, "产物验证未通过: " + verified.verification.evidence);

    // 多段恢复 / 单轨抢救本身就是"有内容损失"的证据：产物虽然能播放，
    // 但至少跳过了损坏区间，或省略了一整条轨。不能因为可播放就宣称完整。
    const bool segment_split = execution.recovered_segments.size() > 1;
    const bool track_dropped = !planning.plan.dropped_input_streams.empty();
    // 非 ISOBMFF：整条结构级检查链路未参与，完整性只能靠运行时读取与产物解码，
    // 不能宣称"已按结构核对完整"。
    const bool container_limits = !inspect.diagnosis.container_known;
    const bool content_lost = execution.read_error || execution.salvage_truncated ||
                              execution.premature_end_unrecovered || planning.plan.salvage_end_seconds.has_value() ||
                              segment_split || track_dropped || execution.output_size_capped ||
                              execution.video_reencoded;
    const bool inconclusive = verified.verification.status == model::RepairCheckStatus::Inconclusive ||
                              verified.verification.decode_errors > 0 || verified.verification.corrupt_frames > 0 ||
                              verified.verification.error_recovery_frames > 0;

    // R03：借用参考初始化段就是借用了 mdhd 时基。只有「源侧有独立时基证据 +
    // 产物时长可比对 + 没有偏差」才能说时间轴未变；否则必须标不确定。
    const bool synthesized_init = inspect.reference_init.has_value() && inspect.reference_init->synthesized;
    const bool borrowed_reference = inspect.reference_init.has_value() && !synthesized_init;
    const bool reference_unverified =
        inspect.reference_init.has_value() && inspect.reference_init->compatibility != "verified";
    const bool timeline_mismatch = verified.verification.timeline_mismatch;
    // 切片 M：普通 MP4 缺 moov 走「参考样片辅助重建」时，同样借用了参考文件的
    // mdhd timescale / stts，而源侧 moov 已丢失，根本没有可核验的独立时基基线。
    // R03 这条红线原先只覆盖 fMP4 初始化段借用（reference_init），C/H/I 路径
    // 绕过它把推断写成 unchanged，还与同一份报告的能力契约
    // 「timeline_status 只能是 modified / unknown」自相矛盾。
    const bool reference_assist_used = execution.reference_assist.used;
    // 切片 S：自建初始化段这条路同样是「时基是我们选定的」——mdhd timescale 取自
    // 源侧 sidx 声明，源片原始 mdhd 已丢失。产物时长与 sidx 估算「一致」是自证：
    // 两侧都用同一个 sidx timescale 换算，比对结果恒成立，不能当独立证据。
    // 所以这里与 reference_assist_used 同档：不许写 unchanged。
    const bool timeline_unknown =
        !timeline_mismatch &&
        ((borrowed_reference && (reference_unverified || !verified.verification.timeline_comparable)) ||
         reference_assist_used || synthesized_init);

    // ---- 5. 发布（R06：提交前的最后一个取消/预算检查点）----
    Emit(progress, 97, stage::Publishing(), "发布产物");
    if (budget.Checkpoint()) {
        const model::RepairBusinessStatus status = BudgetTerminal(budget);
        return finish_with_partial(status, status == model::RepairBusinessStatus::Canceled
                                               ? "发布前收到取消，未发布完整产物"
                                               : "发布前超出本次修复预算，未发布完整产物");
    }
    std::string publish_error;
    if (!RepairOutputWriter::SourceUnchanged(inspect.diagnosis.input, &publish_error, &source_method, &budget))
        return finish_failed(model::RepairBusinessStatus::Failed, publish_error);
    result.input_check_method = "源文件: " + source_method;
    if (inspect.reference_snapshot_valid) {
        std::string reference_method;
        if (!RepairOutputWriter::SourceUnchanged(inspect.reference_snapshot, &publish_error, &reference_method,
                                                 &budget))
            return finish_failed(model::RepairBusinessStatus::Failed, publish_error);
        result.input_check_method += "；参考文件: " + reference_method;
    }
    result.input_check_method += "；摘要是发现改动的证据，不是密码学完整性证明；单次解码调用可能越过时间预算";

    // 提交边界：从这里起不再检查取消 —— rename 之后产物已经对外可见，
    // 再撤回会留下"界面说取消、磁盘上却有成品"的不一致状态。
    if (!RepairOutputWriter::Publish(paths.part_path, paths.final_path, &publish_error))
        return finish_failed(model::RepairBusinessStatus::Failed, publish_error);
    part_guard.Release();

    result.output_path = paths.final_path;
    result.output_published = true;
    result.output_container = paths.extension;
    std::error_code size_ec;
    const uint64_t output_size = std::filesystem::file_size(ToNativePath(paths.final_path), size_ec);
    const std::string output_mime = paths.extension == "mkv"  ? "video/x-matroska"
                                    : paths.extension == "ts" ? "video/mp2t"
                                                              : "video/mp4";
    result.artifacts.push_back(model::RepairArtifact{"output", paths.final_path, output_mime,
                                                     "bytes [0, " + std::to_string(size_ec ? 0 : output_size) + ")"});

    // ---- 6. 保真度 / 业务状态 ----
    model::RepairFidelity& fidelity = result.fidelity;
    fidelity.video_reencoded = execution.video_reencoded;
    fidelity.audio_reencoded = execution.audio_reencoded;
    fidelity.synthetic_frames_inserted = false;
    fidelity.synthetic_frame_count = 0;
    // timeline_modified 只表示「引擎主动重写了时间戳」。旧实现在这里恒写 false，
    // 于是借用未核验参考时基导致播放时长改变时，报告仍声称「时间轴改写: no」。
    const bool timeline_adjusted = execution.timeline_repair.gap_removed ||
                                   execution.timeline_repair.rewritten_timestamps > 0 ||
                                   execution.timeline_repair.synthesized_timestamps > 0;
    // 切片 F：时基本身不是源侧证据时，播放速度就是"推出来的"而不是"源本来就有的"。
    // 这两种情况必须写 modified —— 顺着后面的分支走下去会落到 "unchanged"，
    // 等于对用户声称"时间轴没改"，而实际上每一秒都是按推定帧率算出来的。
    const bool timeline_basis_inferred = execution.timeline_basis.mode == "user_input";
    const bool timeline_vfr_declared = execution.timeline_basis.vfr_declared;
    fidelity.timeline_modified = timeline_mismatch || timeline_adjusted || execution.video_reencoded ||
                                 timeline_basis_inferred || timeline_vfr_declared;
    if (execution.video_reencoded) {
        fidelity.timeline_status = "modified";
        fidelity.timeline_note = "视频轨经过解码后重新编码，输出时间戳按推定帧率从 0 起重新生成；" +
                                 std::string("源时间戳不再保留，帧序与源侧可解码帧一一对应");
    } else if (timeline_mismatch) {
        fidelity.timeline_status = "modified";
        fidelity.timeline_note = "产物实测时长 " + FormatSeconds(verified.verification.observed_duration_seconds) +
                                 "s 与源侧估算 " + FormatSeconds(verified.verification.expected_duration_seconds) +
                                 "s 不符；播放时长 / 速度可能已被借用的参考时基改变";
    } else if (timeline_basis_inferred || timeline_vfr_declared) {
        fidelity.timeline_status = "modified";
        fidelity.timeline_note =
            timeline_basis_inferred
                ? "时基来自显式帧率 " + execution.timeline_basis.frame_rate +
                      "（用户输入，不是源侧证据）；产物时间戳按该帧率生成，实际帧率不同会整体偏快或偏慢"
                : "源码流声明非恒定帧率（SPS fixed_frame_rate_flag=0）；产物按名义帧率 " +
                      execution.timeline_basis.frame_rate + " 重建，逐帧时长与源不同";
        if (timeline_adjusted && !execution.timeline_repair.note.empty())
            fidelity.timeline_note += "；" + execution.timeline_repair.note;
    } else if (timeline_adjusted) {
        // 「adjusted」与「modified」区分开：这里是逐流单调重写 / 缺口合并造成的主动调整，
        // 不是全区段播放速度改变。报告必须能解释改动的来源。
        fidelity.timeline_status = "adjusted";
        fidelity.timeline_note = execution.timeline_repair.note;
        if (execution.timeline_repair.gap_removed)
            fidelity.timeline_note += "；本次按「" + std::string(model::DisplayName(request.gap_policy)) +
                                      "」生成产物，片段内部时间关系保持不变";
    } else if (timeline_unknown) {
        fidelity.timeline_status = "unknown";
        if (synthesized_init) {
            fidelity.timeline_note =
                "初始化段由 in-band 参数集自建，mdhd timescale 采用源侧 sidx 声明；逐样本 tick 原样来自源侧 moof/trun，"
                "但 tick→秒的换算依赖这条声明，源片原始 mdhd 已丢失，不能断言时间轴与源片一致";
        } else {
            fidelity.timeline_note =
                reference_assist_used
                    ? "产物时间轴的 mdhd timescale / stts 借自同源参考样片；源侧 moov "
                      "已丢失，没有可核验的独立时基基线，"
                      "不能断言时间轴与源片一致"
                    : "时间戳仅按输出容器时间基缩放，未主动补齐 / 合成；但本次借用了参考初始化段的时基，"
                      "源侧缺少可核验的独立证据，不能断言时间轴未改变";
        }
    } else {
        fidelity.timeline_status = "unchanged";
        fidelity.timeline_note = "时间戳仅按输出容器时间基缩放，未补齐 / 未合成";
        if (borrowed_reference)
            fidelity.timeline_note += "；参考段时基已与源侧独立证据核对，产物实测时长与源侧估算一致";
    }
    if (execution.video_reencoded) {
        fidelity.video = model::RepairFidelityStatus::PartiallyVerified;
        // 切片 J：源侧属性清单必须原样上抛，报告层不重算、不从文本反推。
        fidelity.transcode = execution.transcode_fidelity;
        if (!fidelity.transcode.inspected) {
            fidelity.limitations.push_back(
                "转码源侧保真属性未探测（首帧或流参数不可用），不能声称位深 / 色彩 / 旋转 / HDR 均已保留");
        } else {
            for (const auto& lost : fidelity.transcode.lost_properties)
                fidelity.limitations.push_back("转码属性损失：" + lost);
            for (const auto& kept : fidelity.transcode.preserved_properties)
                fidelity.limitations.push_back("转码属性继承：" + kept);
            // 切片 Q：「没实测」是第三种状态，不能并进损失表，也不能悄悄不提。
            for (const auto& unverified : fidelity.transcode.unverified_properties)
                fidelity.limitations.push_back("转码属性未实测：" + unverified);
        }
        // 切片 O：编码器事实单独一条。它是「选型取舍」，不是源侧属性损失，
        // 因此不塞进 lost_properties —— 那张表只放实测到的画质属性差异。
        if (!fidelity.transcode.encoder_note.empty()) {
            fidelity.limitations.push_back("重编码编码器: " + fidelity.transcode.encoder_note);
            if (fidelity.transcode.encoder_fallback) {
                fidelity.limitations.push_back(
                    "本项目 FFmpeg 为 LGPL 变体（--disable-libx264 / --disable-libx265），首选编码器不可用；"
                    "产物的画质与码率特性由实际编码器决定，不能按首选编码器的表现解读");
            }
        }
        if (!fidelity.transcode.profile_note.empty())
            fidelity.limitations.push_back("重编码 profile: " + fidelity.transcode.profile_note);
        // 切片 P：音画起点被截（有音频包早于统一原点）时，产物可能已经偏了，
        // 必须进「已知限制」，不能只在保真度章节当一个中性数字。
        if (fidelity.transcode.av_start_offset_seconds.has_value() && !fidelity.transcode.av_start_offset_preserved) {
            fidelity.limitations.push_back("音画同步: " + fidelity.transcode.av_sync_note);
        }
        if (execution.psnr_samples > 0) {
            std::ostringstream psnr_note;
            psnr_note.setf(std::ios::fixed);
            psnr_note.precision(2);
            psnr_note << "抽帧 PSNR 均值 " << execution.psnr_average_db << " dB（" << execution.psnr_samples
                      << " 个样本，亮度平面）；重编码不是原码流无损重封装";
            fidelity.perceptual_fidelity = "measured_psnr";
            fidelity.limitations.push_back(psnr_note.str());
        } else {
            fidelity.perceptual_fidelity = "unknown_without_reference";
            fidelity.limitations.push_back("视频轨已重编码，但抽帧 PSNR 未取得有效样本，画质损失无法量化");
        }
        if (execution.dropped_video_frames > 0)
            fidelity.limitations.push_back("转码丢弃 " + std::to_string(execution.dropped_video_frames) +
                                           " 个无法解码 / 编码的视频包或帧，对应画面内容不可恢复");
        if (!execution.transcode_note.empty())
            fidelity.limitations.push_back(execution.transcode_note);
    }
    if (content_lost || execution.timed_out || conservation_loss)
        fidelity.content_completeness = "partial";
    else if (conservation_unknown || verified.verification.structure_only || timeline_unknown || timeline_mismatch ||
             container_limits)
        fidelity.content_completeness = "unknown";
    else
        fidelity.content_completeness = "complete_for_selected_checks";

    if (execution.output_size_capped) {
        fidelity.limitations.push_back("产物超过配置的输出字节上限（" +
                                       std::to_string(request.limits.max_output_bytes) +
                                       " 字节），仅保留可播放前缀；上限之后的源内容未写入");
        model::DamageFinding finding;
        finding.code = "output.size_capped";
        finding.detector = "repair.engine";
        finding.layer = model::DamageLayer::Container;
        finding.severity = model::IssueSeverity::Warning;
        finding.confidence = model::DamageConfidence::Confirmed;
        finding.location.precision = model::DamagePrecision::Bounded;
        finding.location.provenance = "max_output_bytes vs av_interleaved_write_frame bytes";
        finding.evidence = "写入 " + std::to_string(execution.bytes_written) + " 字节后超过上限 " +
                           std::to_string(request.limits.max_output_bytes) + " 字节，产物已截断发布";
        finding.limitations.push_back("字节上限按输出包累计大小判定，实际产物可能略超上限；只保证前缀可播放");
        result.residual_findings.push_back(std::move(finding));
    }

    // 恢复比例：优先用逐轨样本守恒（参考初始化段有声明样本数），否则用有效区间时长覆盖率。
    // 没有源侧基线时保持 nullopt，不能用产物时长自证 100%。
    {
        std::optional<double> recovery_ratio;
        bool sample_based = false;
        double expected_total = 0.0;
        double written_total = 0.0;
        for (const auto& record : result.track_conservation) {
            if (record.expected_samples == 0)
                continue;
            expected_total += static_cast<double>(record.expected_samples);
            written_total += static_cast<double>(std::min(record.packets_written, record.expected_samples));
        }
        if (expected_total > 0.0) {
            recovery_ratio = written_total / expected_total;
            sample_based = true;
        } else {
            double source_duration = execution.source_duration_seconds;
            source_duration = std::max(source_duration, inspect.source_evidence.estimated_duration_seconds);
            if (source_duration > 0.0) {
                double covered = 0.0;
                if (!execution.recovered_segments.empty()) {
                    for (const auto& segment : execution.recovered_segments)
                        covered += std::max(0.0, segment.input_end_seconds - segment.input_start_seconds);
                } else if (execution.output_start_seconds.has_value() && execution.output_end_seconds.has_value()) {
                    covered = std::max(0.0, *execution.output_end_seconds - *execution.output_start_seconds);
                }
                if (covered > 0.0)
                    recovery_ratio = covered / source_duration;
                if (recovery_ratio.has_value())
                    fidelity.limitations.push_back(
                        "恢复比例按有效区间时长占源声明时长的比例估算，不是逐帧 / 逐字节比对");
            }
        }
        if (recovery_ratio.has_value()) {
            fidelity.original_content_recovery_ratio = std::min(1.0, std::max(0.0, *recovery_ratio));
            if (sample_based) {
                // 基线来自哪份证据要说清楚：fMP4 缺初始化段这条路是源侧 moof/trun 的
                // 逐轨声明，旧文案一律写「参考初始化段声明的逐轨样本数」——参考初始化段
                // 里根本没有样本数（只有 trex 默认值），那是把基线来源说反了。
                std::string baseline = "源侧声明";
                for (const auto& expected : result.diagnosis.expected_tracks) {
                    if (expected.sample_count > 0 && !expected.provenance.empty()) {
                        baseline = expected.provenance;
                        break;
                    }
                }
                fidelity.limitations.push_back("恢复比例按 " + baseline + " 声明的逐轨样本数估算，不是逐字节比对");
            }
        } else {
            fidelity.limitations.push_back("缺少源侧时长或样本基线，未给出恢复比例");
        }
    }

    if (container_limits)
        fidelity.limitations.push_back(
            "源容器未识别为 ISOBMFF，未做盒/样本表级结构校验；损伤定位与完整性结论仅覆盖运行时读取与产物解码");
    if (paths.extension == "ts")
        fidelity.limitations.push_back("产物封装为 MPEG-TS，H.264/HEVC 已转换为 Annex B 码流；载荷字节与源不一致，"
                                       "本次未做载荷哈希比对，保真度按解码可读性评估");
    if (execution.index_table_rebuilt)
        fidelity.limitations.push_back("源 moov 样本表索引不自洽，执行前已按原 chunk 顺序重建 stco/stts/ctts/stss；" +
                                       execution.index_rebuild_note);
    else if (!execution.index_rebuild_error.empty())
        fidelity.limitations.push_back("源 moov 样本表索引重建未成功（" + execution.index_rebuild_error +
                                       "），本次仍按原索引尝试重封装");
    if (execution.moov_rebuilt)
        fidelity.limitations.push_back("源文件缺少 moov，执行前扫描 mdat 无参考重建了单轨样本表；" +
                                       execution.moov_rebuild_note + "（样本边界为实测推断，未经原索引交叉验证）");
    if (!content_lost)
        fidelity.limitations.push_back("未与原始完整内容比对（输入本身已是损坏文件）");
    if (execution.premature_end_unrecovered)
        fidelity.limitations.push_back("检测到轨道提前结束且未找到可恢复的关键帧，中断点之后的内容未参与修复");
    if (inconclusive)
        fidelity.limitations.push_back("验证未完全通过，不能断言全部内容可解码或画面干净");
    if (verified.verification.structure_only)
        fidelity.limitations.push_back("V0 只验证容器结构，尚未验证播放与音画完整性");
    if (conservation_unknown)
        fidelity.limitations.push_back("至少一条轨道的样本守恒无法判定，完整性结论受限");
    if (timeline_mismatch)
        fidelity.limitations.push_back("时间轴实测与源侧估算不符，播放时长 / 速度可能已被改变");
    if (timeline_unknown)
        fidelity.limitations.push_back(
            reference_assist_used ? "参考样片辅助重建借用了参考文件的时基；源侧 moov 已丢失，时间轴是否改变无法确定"
                                  : "借用的参考时基未经源侧独立证据核验，时间轴是否改变无法确定");
    if (reference_unverified)
        fidelity.limitations.push_back("参考段兼容性未核验，完整性结论只覆盖实际写入并被验证的包");
    if (segment_split) {
        double skipped = 0.0;
        for (const auto& segment : execution.recovered_segments)
            skipped += std::max(0.0, segment.skipped_before_seconds);
        fidelity.limitations.push_back("产物由 " + std::to_string(execution.recovered_segments.size()) +
                                       " 段组成，中间约 " + FormatSeconds(skipped) +
                                       " s 损坏区间未参与修复；只证明跳过区间之外的内容可播放");
    }
    if (track_dropped) {
        fidelity.limitations.push_back("已按单轨抢救省略 " +
                                       std::to_string(planning.plan.dropped_input_streams.size()) +
                                       " 条无可用样本的轨道，产物不包含这部分内容");
        model::DamageFinding finding;
        finding.code = "track.salvaged_single_track";
        finding.detector = "repair.engine";
        finding.layer = model::DamageLayer::Container;
        finding.severity = model::IssueSeverity::Warning;
        finding.confidence = model::DamageConfidence::Confirmed;
        finding.location.precision = model::DamagePrecision::Bounded;
        finding.location.provenance = "sample bounds all out of range vs per-track output";
        finding.evidence = "诊断判定 " + std::to_string(planning.plan.dropped_input_streams.size()) +
                           " 条轨道的全部样本不可读，已从产物中省略以保证其余轨道可播放";
        finding.limitations.push_back("被省略轨道的内容无法从本文件找回；音画关系已改变");
        finding.candidate_actions.push_back(model::RepairAction::S09_SingleTrack);
        result.residual_findings.push_back(std::move(finding));
    }
    if (timeline_adjusted)
        fidelity.limitations.push_back("时间轴经主动调整（" + execution.timeline_repair.note +
                                       "）；片段内部时间关系未被改变");
    if (execution.timeline_repair.synthesized_timestamps > 0)
        fidelity.limitations.push_back(
            "合成了 " + std::to_string(execution.timeline_repair.synthesized_timestamps) +
            " 个完全缺失的时间戳（按上一帧 + 标称帧时长外推），播放时长可能与源侧真实时间存在偏差");
    for (const auto& omitted : execution.omitted_items)
        fidelity.limitations.push_back("未保留: " + omitted);
    if (!verified.verification.timeline_checks.empty()) {
        for (const auto& line : verified.verification.timeline_checks)
            fidelity.limitations.push_back("时间轴实测: " + line);
    }
    if (verified.verification.av_sync_status == "drift") {
        fidelity.limitations.push_back("音画首尾时间戳漂移 " +
                                       FormatSeconds(verified.verification.av_sync_drift_seconds) +
                                       " s，超过容差；只比较展示时间范围，不证明逐帧音画同步");
    }

    model::RepairBusinessStatus status = model::RepairBusinessStatus::Repaired;
    if (inconclusive || content_lost || conservation_loss || conservation_unknown ||
        verified.verification.structure_only || reference_unverified || timeline_unknown || timeline_mismatch ||
        timeline_adjusted)
        status = model::RepairBusinessStatus::PartiallyRecovered;

    // ---- R03：参考段借用参数与时间轴风险 ----
    if (inspect.reference_init.has_value()) {
        const RepairInitSegment& init = *inspect.reference_init;
        fidelity.reference_compatibility = init.compatibility;
        fidelity.borrowed_parameters = init.borrowed_parameters;
        if (init.synthesized) {
            // 「自建」这件事已经写进 timeline_note 的第一句，这里只补实测的时基证据，
            // 不重复同一句话（同一份报告里两句同义文案会让读者以为是两件事）。
            fidelity.timeline_note += "；时基证据：" + init.timescale_check;
            // 切片 S：这条路上的「源侧估算 = 产物实测」是自证 —— 两侧都用同一个
            // sidx timescale 换算，一致只能说明换算没算错，不能说明时基与原始 mdhd 一致。
            if (verified.verification.timeline_comparable)
                fidelity.limitations.push_back(
                    "时间轴实测的一致性是自证：源侧估算与产物实测都由同一个 sidx timescale 换算，"
                    "相等不能证明时基与源片原始 mdhd 一致");
        } else {
            fidelity.timeline_note += "；本次借用了参考初始化段的 ftyp+moov/mvex/trex 与 mdhd 时基";
        }
        if (init.compatibility != "verified") {
            fidelity.timeline_note += "；兼容性=" + init.compatibility + "（" + init.compatibility_reason + "）";
            fidelity.limitations.push_back((init.synthesized ? "自建初始化段" : "参考初始化段") +
                                           ("兼容性=" + init.compatibility + "：" + init.compatibility_reason));
            model::DamageFinding finding;
            finding.code = init.synthesized ? "init_segment.synthesized" : "reference.parameters_unverified";
            finding.detector = "repair.engine";
            finding.layer = model::DamageLayer::Timing;
            finding.severity = model::IssueSeverity::Warning;
            finding.confidence = model::DamageConfidence::Possible;
            finding.location.precision = model::DamagePrecision::Bounded;
            finding.location.provenance = init.synthesized ? "in-band SPS/PPS + sidx timescale"
                                                           : "sidx timescale / mdat NAL vs reference mdhd/stsd";
            finding.evidence = init.synthesized
                                   ? "初始化段由源分片 in-band 参数集自建（" + init.compatibility_reason +
                                         "）；时基=" + init.timescale_check
                                   : "产物借用了参考文件 " + FileName(init.reference_path) + " 的解码参数与时基（" +
                                         init.compatibility + "）: " + init.compatibility_reason;
            finding.limitations.push_back(init.synthesized
                                              ? "时基来自 sidx / 分片声明；推断错误会保留全部帧却改变播放时长"
                                              : "track_id 相同不能证明同源，错误时基会保留全部帧却改变播放时长");
            finding.limitations.push_back("没有原始完整视频，无法给出确定的无失真结论或恢复百分比");
            result.residual_findings.push_back(std::move(finding));
        }
    } else if (execution.reference_assist.used) {
        // 切片 M：参考样片辅助重建（C/H/I）借的是完整 MP4 的 moov，不是 fMP4
        // 初始化段。保真度结论必须与「恢复区间与时间轴」里的 reference_assist
        // 同源 —— 以前一个说 verified、另一个说 not_applicable。
        const model::RepairReferenceAssist& assist = execution.reference_assist;
        fidelity.reference_compatibility = assist.compatibility;
        fidelity.borrowed_parameters = assist.borrowed_parameters;
        fidelity.timeline_note +=
            "；本次由同源参考样片辅助重建 moov，借用了解码配置与 mdhd 时基（兼容性 " + assist.compatibility + "）";
        // 这里刻意不写「兼容性非 verified 时记账」的分支：`used == true` 时兼容性
        // 由重建器唯一写入点保证为 verified，分支永远不触发，留着就是假覆盖。
        // 「非 verified 的参考根本不会被 used」由 ForeignReferenceNeverClaimsVerifiedBasis
        // 守护；若哪天重建器允许 used && !verified，这条限制必须加回并配用例。
        // 有同源参考样片 ≠ 做过感知比对。默认 unknown_without_reference 会把
        // 「有参考但没测」写成「没有参考」，是事实错误。
        fidelity.perceptual_fidelity = "not_measured_with_reference";
        fidelity.limitations.push_back("本次有同源参考样片，但未做逐帧画质 / 感知保真度比对，不能声称画质无损");
    }

    // 逐轨守恒失败必须落到状态与保真度上，不能被大轨道掩盖。
    if (conservation_loss) {
        fidelity.video = model::RepairFidelityStatus::PartiallyVerified;
        fidelity.limitations.push_back("逐轨样本数守恒未通过，产物只包含部分访问单元");
        status = model::RepairBusinessStatus::PartiallyRecovered;
    }

    result.status = status;
    result.summary = BuildSummary(result);
    result.error.clear();
    result.created_at_iso8601 = FormatIso8601Now();

    Emit(progress, 100, stage::Publishing(), "修复完成");
    return result;
}

} // namespace

model::RepairResult RepairEngine::Run(const model::RepairRequest& request, const std::atomic<bool>* cancel,
                                      const RepairProgress& progress) {
    // 唯一收尾点：预检失败 / 诊断流产 / 取消 / 超时 / 成功发布都经过这里。
    // 用包装函数而不是逐出口手写；也不能放进 RAII 守卫的析构 —— `return result`
    // 若发生 move，析构时改的是已被搬走的局部对象，字段会静默丢失。
    model::RepairResult result = RunCore(request, cancel, progress);
    result.declared_exclusions = BuildDeclaredExclusions(result);
    return result;
}

} // namespace repair
} // namespace videoeye
