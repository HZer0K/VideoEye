#include "core/repair/RepairEngine.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

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

std::string BuildSummary(const model::RepairResult& result) {
    std::ostringstream summary;
    summary << model::DisplayName(result.status);
    if (!result.output_path.empty())
        summary << "；产物: " << result.output_path;
    if (result.executions.size() > 0) {
        summary << "；写入 " << result.executions.front().packets_written << " 个包";
    }
    if (result.verification.status != model::RepairCheckStatus::NotRun)
        summary << "；" << model::DisplayName(result.verification.level) << " " << ToString(result.verification.status);
    return summary.str();
}

} // namespace

model::RepairResult RepairEngine::Run(const model::RepairRequest& request, const std::atomic<bool>* cancel,
                                      const RepairProgress& progress) {
    model::RepairResult result;
    result.request_id = request.request_id.empty() ? "req-" + FormatLocalRunId() : request.request_id;
    result.result_id = "result-" + FormatLocalRunId();
    result.created_at_iso8601 = FormatIso8601Now();
    result.ffmpeg_version.clear();
    result.fidelity.perceptual_fidelity = "unknown_without_reference";

    // ---- 0. 请求预检 + 产物路径解析 ----
    Emit(progress, 0, stage::Inspecting(), "检查请求与产物路径");
    const std::string run_id = result.result_id.substr(std::string("result-").size());
    RepairOutputPaths paths = RepairOutputWriter::Resolve(request, run_id);
    result.report_json_path = paths.report_json_path;
    result.report_text_path = paths.report_text_path;
    result.report_html_path = paths.report_html_path;
    if (!paths.valid) {
        result.status = model::RepairBusinessStatus::Failed;
        result.error = paths.error;
        result.summary = "请求无效: " + paths.error;
        FinishEarly(result, model::RepairBusinessStatus::Failed, result.summary);
        result.error = paths.error;
        return result;
    }

    // ---- 1. 诊断 ----
    RepairInspectOutcome inspect = RepairInspector::Inspect(request, cancel, progress);
    result.diagnosis = inspect.diagnosis;
    result.diagnosis_id = inspect.diagnosis.diagnosis_id;
    result.input = inspect.diagnosis.input;

    if (inspect.canceled) {
        result.summary = "诊断阶段被取消";
        FinishEarly(result, model::RepairBusinessStatus::Canceled, result.summary);
        return result;
    }

    // ---- 2. 规划 ----
    Emit(progress, 72, stage::Planning(), "制定修复计划");
    RepairPlanOutcome planning =
        RepairPlanner::MakePlan(request, inspect.diagnosis, inspect.salvage_end_seconds, inspect.target_stream_index,
                                inspect.reference_init ? &*inspect.reference_init : nullptr);
    result.plan = planning.plan;
    result.plan_id = planning.plan.plan_id;

    if (!planning.can_execute) {
        const std::string reason =
            planning.plan.infeasible_reason.empty() ? "没有可执行的修复策略" : planning.plan.infeasible_reason;
        result.error = reason;
        // 分片取证的实测结论也要进结果摘要：否则界面只看到一句"需要参考样片"，
        // 看不到已经查明了多少段、什么编码、缺哪几个参数集。
        result.summary = reason;
        if (!result.diagnosis.summary.empty())
            result.summary += "；" + result.diagnosis.summary;
        FinishEarly(result, planning.early_status, result.summary);
        return result;
    }

    // ---- 3. 执行 ----
    Emit(progress, 75, stage::Executing(), "开始重封装");
    std::string source_error;
    if (!RepairOutputWriter::SourceUnchanged(inspect.diagnosis.input, &source_error)) {
        result.summary = source_error;
        FinishEarly(result, model::RepairBusinessStatus::Failed, source_error);
        result.error = source_error;
        return result;
    }

    RepairExecutionOutcome execution =
        RepairExecutor::Execute(request, inspect.diagnosis, planning.plan, paths.part_path,
                                inspect.reference_init ? &*inspect.reference_init : nullptr, cancel, progress);
    result.executions = execution.executions;
    result.ffmpeg_version = execution.ffmpeg_version;
    result.residual_findings = execution.residual_findings;

    if (execution.canceled) {
        RepairOutputWriter::Discard(paths.part_path);
        result.summary = "执行阶段被取消，未发布产物";
        FinishEarly(result, model::RepairBusinessStatus::Canceled, result.summary);
        return result;
    }
    if (execution.timed_out) {
        RepairOutputWriter::Discard(paths.part_path);
        result.summary = "超出本次修复预算，未发布产物";
        FinishEarly(result, model::RepairBusinessStatus::TimedOut, result.summary);
        result.error = execution.error;
        return result;
    }
    if (!execution.ok) {
        RepairOutputWriter::Discard(paths.part_path);
        result.error = execution.error.empty() ? "执行失败" : execution.error;
        result.summary = result.error;
        if (result.error.find("没有写入任何数据包") != std::string::npos ||
            result.error.find("没有可复制") != std::string::npos)
            FinishEarly(result, model::RepairBusinessStatus::NoRecoverableContent, result.summary);
        else if (result.error.find("无法打开源文件") != std::string::npos)
            FinishEarly(result, model::RepairBusinessStatus::Unsupported, result.summary);
        else
            FinishEarly(result, model::RepairBusinessStatus::Failed, result.summary);
        return result;
    }

    // ---- 4. 验证 ----
    std::optional<uint64_t> expected_hash;
    for (const auto& action : execution.executions) {
        if (action.action == model::RepairAction::S01_Remux && action.input_video_payload_hash.has_value()) {
            expected_hash = action.input_video_payload_hash;
            break;
        }
    }
    Emit(progress, 89, stage::Verifying(), "开始验证产物");
    RepairVerifyOutcome verified =
        RepairVerifier::Verify(paths.part_path, request.verification_level, expected_hash, cancel, progress);
    result.verification = verified.verification;
    result.fidelity = verified.fidelity;
    result.residual_findings.insert(result.residual_findings.end(), verified.verification.residual_findings.begin(),
                                    verified.verification.residual_findings.end());

    if (verified.canceled) {
        RepairOutputWriter::Discard(paths.part_path);
        result.summary = "验证阶段被取消，未发布产物";
        FinishEarly(result, model::RepairBusinessStatus::Canceled, result.summary);
        return result;
    }
    if (verified.verification.status == model::RepairCheckStatus::Failed) {
        RepairOutputWriter::Discard(paths.part_path);
        result.error = "产物验证未通过: " + verified.verification.evidence;
        result.summary = result.error;
        FinishEarly(result, model::RepairBusinessStatus::Failed, result.summary);
        return result;
    }

    const bool content_lost =
        execution.read_error || execution.salvage_truncated || planning.plan.salvage_end_seconds.has_value();
    const bool inconclusive = verified.verification.status == model::RepairCheckStatus::Inconclusive ||
                              verified.verification.decode_errors > 0;

    // ---- 5. 发布 ----
    Emit(progress, 97, stage::Publishing(), "发布产物");
    if (!RepairOutputWriter::SourceUnchanged(inspect.diagnosis.input, &source_error)) {
        RepairOutputWriter::Discard(paths.part_path);
        result.summary = source_error;
        FinishEarly(result, model::RepairBusinessStatus::Failed, source_error);
        result.error = source_error;
        return result;
    }
    if (!RepairOutputWriter::Publish(paths.part_path, paths.final_path, &source_error)) {
        result.summary = source_error;
        FinishEarly(result, model::RepairBusinessStatus::Failed, source_error);
        result.error = source_error;
        return result;
    }

    result.output_path = paths.final_path;
    result.output_published = true;
    result.artifacts.push_back(
        model::RepairArtifact{"output", paths.final_path, "video/mp4",
                              "bytes [0, " + std::to_string(std::filesystem::file_size(paths.final_path)) + ")"});

    // ---- 6. 保真度 / 业务状态 ----
    model::RepairFidelity& fidelity = result.fidelity;
    fidelity.video_reencoded = false;
    fidelity.audio_reencoded = false;
    fidelity.synthetic_frames_inserted = false;
    fidelity.synthetic_frame_count = 0;
    fidelity.timeline_modified = false;
    fidelity.timeline_note = "时间戳仅按输出容器时间基缩放，未补齐 / 未合成";
    if (content_lost || execution.timed_out)
        fidelity.content_completeness = "partial";
    else
        fidelity.content_completeness = "complete_for_selected_checks";
    if (!content_lost)
        fidelity.limitations.push_back("未与原始完整内容比对（输入本身已是损坏文件）");
    if (inconclusive)
        fidelity.limitations.push_back("验证未完全通过，不能断言全部内容可解码");

    model::RepairBusinessStatus status = model::RepairBusinessStatus::Repaired;
    if (inconclusive || content_lost)
        status = model::RepairBusinessStatus::PartiallyRecovered;
    else if (execution.read_error || execution.salvage_truncated)
        status = model::RepairBusinessStatus::PartiallyRecovered;

    // ---- 源侧样本守恒（只覆盖"前置参考初始化段"这条路径）----
    // 该路径的源侧样本数是从 moof/trun 逐段数出来的精确值。参考段与源分片
    // 解码参数不匹配时，FFmpeg 会在 demuxer 层静默丢弃访问单元：av_read_frame
    // 根本不返回它们，于是载荷哈希也照样"一致"，单靠 V2 验证看不出内容缺失。
    // 这里用源侧声明数挡住这种"看起来修好了、其实丢了大半"的结果。
    if (inspect.reference_init.has_value() && !result.diagnosis.expected_tracks.empty()) {
        uint64_t expected = 0;
        for (const auto& track : result.diagnosis.expected_tracks)
            expected += track.sample_count;
        const uint64_t written = execution.packets_written;
        if (SampleLossExceedsTolerance(expected, written)) {
            model::DamageFinding finding;
            finding.code = "container.samples_dropped_by_demuxer";
            finding.detector = "repair.engine";
            finding.layer = model::DamageLayer::Container;
            finding.severity = model::IssueSeverity::Error;
            finding.confidence = model::DamageConfidence::Confirmed;
            finding.location.precision = model::DamagePrecision::Bounded;
            finding.location.provenance = "moof/trun declared samples vs av_interleaved_write_frame count";
            finding.evidence = "源容器声明 " + std::to_string(expected) + " 个样本，实际只写入 " +
                               std::to_string(written) + " 个包（少 " + std::to_string(expected - written) +
                               " 个）；参考初始化段的解码参数很可能与源分片不匹配";
            finding.limitations.push_back("只能证明包数不足，不能定位丢失的是哪一段或哪一轨");
            finding.limitations.push_back("载荷哈希只覆盖了实际写入的包，不能证明源内容完整");
            result.residual_findings.push_back(std::move(finding));
            fidelity.content_completeness = "partial";
            fidelity.video = model::RepairFidelityStatus::PartiallyVerified;
            fidelity.limitations.push_back("源侧样本数守恒未通过，产物只包含部分访问单元");
            status = model::RepairBusinessStatus::PartiallyRecovered;
        }
    }

    result.status = status;
    result.summary = BuildSummary(result);
    result.error.clear();
    result.created_at_iso8601 = FormatIso8601Now();

    Emit(progress, 100, stage::Publishing(), "修复完成");
    return result;
}

} // namespace repair
} // namespace videoeye
