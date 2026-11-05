#include "core/reporting/RepairReportExporter.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "core/reporting/QcReportExporterInternal.h"
#include "infrastructure/serialization/Json.h"

namespace videoeye {
namespace reporting {
namespace {

using JsonValue = videoeye::JsonValue;

constexpr const char* kRepairSchemaVersion = "1.0";

JsonValue JText(const std::string& value) {
    return JsonValue(value);
}

// 64 位整数统一写字符串：字节偏移 / 样本序号 / 包计数超过 2^53 时 JSON number 会丢精度。
JsonValue J64(uint64_t value) {
    return JsonValue(std::to_string(value));
}

JsonValue J64(int64_t value) {
    return JsonValue(std::to_string(value));
}

JsonValue JNum(double value) {
    if (!std::isfinite(value))
        return JsonValue();
    return JsonValue(value);
}

JsonValue JSeconds(const std::optional<double>& value) {
    return value.has_value() ? JNum(*value) : JsonValue();
}

JsonValue JOptString(const std::string& value) {
    return value.empty() ? JsonValue() : JsonValue(value);
}

JsonValue JOptInt(const std::optional<int>& value) {
    return value.has_value() ? JsonValue(static_cast<int>(*value)) : JsonValue();
}

JsonValue JOptU32(const std::optional<uint32_t>& value) {
    return value.has_value() ? JsonValue(static_cast<double>(*value)) : JsonValue();
}

JsonValue JOptU64(const std::optional<uint64_t>& value) {
    return value.has_value() ? J64(*value) : JsonValue();
}

JsonValue BuildByteExtent(const std::optional<model::RepairByteExtent>& extent) {
    if (!extent.has_value())
        return JsonValue();
    JsonValue object = JsonValue::MakeObject();
    object.Set("begin", J64(extent->begin));
    object.Set("end", extent->known_end ? J64(extent->end) : JsonValue());
    object.Set("known_end", JsonValue(extent->known_end));
    return object;
}

JsonValue BuildLocation(const model::DamageLocation& location) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("bytes", BuildByteExtent(location.bytes));
    object.Set("stream_index", JOptInt(location.stream_index));
    object.Set("track_id", JOptU32(location.track_id));
    object.Set("packet_ordinal", JOptU64(location.packet_ordinal));
    object.Set("sample_ordinal", JOptU64(location.sample_ordinal));
    object.Set("frame_ordinal", JOptU64(location.frame_ordinal));
    object.Set("item_count", JOptU64(location.item_count));
    object.Set("structure_path", JOptString(location.structure_path));
    object.Set("field", JOptString(location.field_name));
    object.Set("pts", location.pts.has_value() ? J64(*location.pts) : JsonValue());
    object.Set("dts", location.dts.has_value() ? J64(*location.dts) : JsonValue());
    if (location.time_base_num.has_value() && location.time_base_den.has_value())
        object.Set("time_base",
                   JsonValue(std::to_string(*location.time_base_num) + "/" + std::to_string(*location.time_base_den)));
    object.Set("source_seconds", JSeconds(location.source_seconds));
    object.Set("estimated_seconds", JSeconds(location.estimated_seconds));
    object.Set("output_seconds", JSeconds(location.output_seconds));
    object.Set("precision", JText(model::ToString(location.precision)));
    object.Set("provenance", JOptString(location.provenance));
    return object;
}

JsonValue BuildFinding(const model::DamageFinding& finding) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("code", JText(finding.code));
    object.Set("detector", JText(finding.detector));
    object.Set("layer", JText(model::ToString(finding.layer)));
    object.Set("severity", JText(model::ToString(finding.severity)));
    object.Set("confidence", JText(model::ToString(finding.confidence)));
    object.Set("location", BuildLocation(finding.location));
    object.Set("evidence", JText(finding.evidence));
    object.Set("hypothesis", JOptString(finding.hypothesis));
    object.Set("cause_issue_id", JOptString(finding.cause_issue_id));
    JsonValue actions = JsonValue::MakeArray();
    for (model::RepairAction action : finding.candidate_actions)
        actions.PushBack(JText(model::ToString(action)));
    object.Set("candidate_actions", actions);
    JsonValue limitations = JsonValue::MakeArray();
    for (const auto& item : finding.limitations)
        limitations.PushBack(JText(item));
    object.Set("limitations", limitations);
    return object;
}

JsonValue BuildFindings(const std::vector<model::DamageFinding>& findings) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& finding : findings)
        array.PushBack(BuildFinding(finding));
    return array;
}

JsonValue BuildCheck(const model::RepairCheck& check) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("name", JText(check.name));
    object.Set("layer", JText(model::ToString(check.layer)));
    object.Set("status", JText(model::ToString(check.status)));
    object.Set("detail", JText(check.detail));
    object.Set("cost_ms", J64(check.cost_ms));
    return object;
}

JsonValue BuildChecks(const std::vector<model::RepairCheck>& checks) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& check : checks)
        array.PushBack(BuildCheck(check));
    return array;
}

JsonValue BuildStringArray(const std::vector<std::string>& items) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& item : items)
        array.PushBack(JText(item));
    return array;
}

JsonValue BuildCoverage(const model::RepairCoverage& coverage) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("bytes_scanned", J64(coverage.bytes_scanned));
    object.Set("packets_scanned", J64(coverage.packets_scanned));
    object.Set("frames_scanned", J64(coverage.frames_scanned));
    object.Set("time_scanned_seconds", JNum(coverage.time_scanned_seconds));
    object.Set("reached_eof", JsonValue(coverage.reached_eof));
    object.Set("truncated_by_budget", JsonValue(coverage.truncated_by_budget));
    object.Set("skipped_ranges", BuildStringArray(coverage.skipped_ranges));
    object.Set("unsupported_items", BuildStringArray(coverage.unsupported_items));
    object.Set("limitations", BuildStringArray(coverage.limitations));
    return object;
}

// 源侧样本数基线：让报告读者能自己核对"实际写入包数"有没有少。
JsonValue BuildExpectedTracks(const std::vector<model::RepairExpectedTrack>& tracks) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& track : tracks) {
        JsonValue object = JsonValue::MakeObject();
        object.Set("track_id", JsonValue(static_cast<int>(track.track_id)));
        object.Set("sample_count", J64(track.sample_count));
        object.Set("codec_hint", JOptString(track.codec_hint));
        object.Set("provenance", JText(track.provenance));
        array.PushBack(std::move(object));
    }
    return array;
}

JsonValue BuildInput(const model::RepairInputSnapshot& input) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("path", JText(input.path));
    object.Set("size_bytes", J64(input.size_bytes));
    object.Set("mtime_epoch_seconds", J64(input.mtime_epoch_seconds));
    object.Set("signature", JOptString(input.signature));
    return object;
}

JsonValue BuildDiagnosis(const model::RepairDiagnosis& diagnosis) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("diagnosis_id", JText(diagnosis.diagnosis_id));
    object.Set("openable", JsonValue(diagnosis.openable));
    object.Set("container_known", JsonValue(diagnosis.container_known));
    object.Set("moov_present", JsonValue(diagnosis.moov_present));
    object.Set("fragmented", JsonValue(diagnosis.fragmented));
    object.Set("summary", JText(diagnosis.summary));
    object.Set("blocking_reasons", BuildStringArray(diagnosis.blocking_reasons));
    object.Set("checks", BuildChecks(diagnosis.checks));
    object.Set("findings", BuildFindings(diagnosis.findings));
    object.Set("coverage", BuildCoverage(diagnosis.coverage));
    object.Set("expected_tracks", BuildExpectedTracks(diagnosis.expected_tracks));
    JsonValue candidates = JsonValue::MakeArray();
    for (model::RepairAction action : diagnosis.candidate_actions)
        candidates.PushBack(JText(model::ToString(action)));
    object.Set("candidate_actions", candidates);
    return object;
}

JsonValue BuildPlan(const model::RepairPlan& plan) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("plan_id", JText(plan.plan_id));
    object.Set("diagnosis_id", JText(plan.diagnosis_id));
    object.Set("mode", JText(model::ToString(plan.mode)));
    object.Set("feasible", JsonValue(plan.feasible));
    object.Set("infeasible_reason", JOptString(plan.infeasible_reason));
    object.Set("expected_output_extension", JText(plan.expected_output_extension));
    object.Set("salvage_end_seconds", JSeconds(plan.salvage_end_seconds));
    object.Set("preconditions", BuildStringArray(plan.global_preconditions));

    JsonValue actions = JsonValue::MakeArray();
    for (const auto& action : plan.actions) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("action", JText(model::ToString(action.action)));
        node.Set("display_name", JText(model::DisplayName(action.action)));
        node.Set("description", JText(action.description));
        node.Set("available", JsonValue(action.available));
        node.Set("unavailable_reason", JOptString(action.unavailable_reason));
        node.Set("expected_effect", JText(action.expected_effect));
        node.Set("risk", JText(action.risk));
        JsonValue deps = JsonValue::MakeArray();
        for (model::RepairAction dependency : action.depends_on)
            deps.PushBack(JText(model::ToString(dependency)));
        node.Set("depends_on", deps);
        actions.PushBack(node);
    }
    object.Set("actions", actions);
    return object;
}

JsonValue BuildExecution(const model::RepairActionExecution& execution) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("action", JText(model::ToString(execution.action)));
    object.Set("attempted", JsonValue(execution.attempted));
    object.Set("succeeded", JsonValue(execution.succeeded));
    object.Set("detail", JText(execution.detail));
    object.Set("error", JOptString(execution.error));
    object.Set("packets_read", J64(execution.packets_read));
    object.Set("packets_written", J64(execution.packets_written));
    object.Set("bytes_written", J64(execution.bytes_written));
    object.Set("output_start_seconds", JSeconds(execution.output_start_seconds));
    object.Set("output_end_seconds", JSeconds(execution.output_end_seconds));
    object.Set("input_video_payload_hash", JOptU64(execution.input_video_payload_hash));
    object.Set("input_video_packet_count", J64(execution.input_video_packet_count));
    return object;
}

JsonValue BuildVerification(const model::RepairVerification& verification) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("level", JText(model::ToString(verification.level)));
    object.Set("status", JText(model::ToString(verification.status)));
    object.Set("checks", BuildChecks(verification.checks));
    object.Set("decoded_frames", J64(verification.decoded_frames));
    object.Set("decode_errors", J64(verification.decode_errors));
    object.Set("seek_checks", J64(verification.seek_checks));
    object.Set("seek_passes", J64(verification.seek_passes));
    object.Set("decode_scope", JText(verification.decode_scope));
    object.Set("residual_findings", BuildFindings(verification.residual_findings));
    object.Set("evidence", JText(verification.evidence));
    return object;
}

JsonValue BuildFidelity(const model::RepairFidelity& fidelity) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("video", JText(model::ToString(fidelity.video)));
    object.Set("audio", JText(model::ToString(fidelity.audio)));
    object.Set("video_reencoded", JsonValue(fidelity.video_reencoded));
    object.Set("audio_reencoded", JsonValue(fidelity.audio_reencoded));
    object.Set("synthetic_frames_inserted", JsonValue(fidelity.synthetic_frames_inserted));
    object.Set("synthetic_frame_count", J64(fidelity.synthetic_frame_count));
    object.Set("timeline_modified", JsonValue(fidelity.timeline_modified));
    object.Set("timeline_note", JText(fidelity.timeline_note));
    object.Set("content_completeness", JText(fidelity.content_completeness));
    object.Set("original_content_recovery_ratio", JSeconds(fidelity.original_content_recovery_ratio));
    object.Set("perceptual_fidelity", JText(fidelity.perceptual_fidelity));
    object.Set("limitations", BuildStringArray(fidelity.limitations));
    return object;
}

JsonValue BuildArtifacts(const std::vector<model::RepairArtifact>& artifacts) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& artifact : artifacts) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("role", JText(artifact.role));
        node.Set("path", JText(artifact.path));
        node.Set("mime", JText(artifact.mime));
        node.Set("stored_range", JOptString(artifact.stored_range));
        array.PushBack(node);
    }
    return array;
}

JsonValue BuildResultJson(const model::RepairResult& result) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("schema_version", JText(kRepairSchemaVersion));
    root.Set("generator", JText("VideoEye repair"));
    root.Set("result_id", JText(result.result_id));
    root.Set("request_id", JText(result.request_id));
    root.Set("plan_id", JOptString(result.plan_id));
    root.Set("diagnosis_id", JOptString(result.diagnosis_id));
    root.Set("created_at", JText(result.created_at_iso8601));
    root.Set("status", JText(model::ToString(result.status)));
    root.Set("status_text", JText(model::DisplayName(result.status)));
    root.Set("summary", JText(result.summary));
    root.Set("error", JOptString(result.error));
    root.Set("ffmpeg_version", JOptString(result.ffmpeg_version));

    JsonValue output = JsonValue::MakeObject();
    output.Set("path", JOptString(result.output_path));
    output.Set("published", JsonValue(result.output_published));
    root.Set("output", output);

    root.Set("input", BuildInput(result.input));
    root.Set("diagnosis", BuildDiagnosis(result.diagnosis));
    root.Set("plan", BuildPlan(result.plan));

    JsonValue executions = JsonValue::MakeArray();
    for (const auto& execution : result.executions)
        executions.PushBack(BuildExecution(execution));
    root.Set("executions", executions);

    root.Set("verification", BuildVerification(result.verification));
    root.Set("fidelity", BuildFidelity(result.fidelity));
    root.Set("artifacts", BuildArtifacts(result.artifacts));
    root.Set("residual_findings", BuildFindings(result.residual_findings));
    return root;
}

std::string LocationText(const model::DamageLocation& location) {
    std::ostringstream out;
    if (location.bytes.has_value()) {
        out << "bytes [" << location.bytes->begin << ", ";
        if (location.bytes->known_end)
            out << location.bytes->end;
        else
            out << "?";
        out << ")";
    }
    if (location.track_id.has_value())
        out << " track=" << *location.track_id;
    if (location.stream_index.has_value())
        out << " stream=" << *location.stream_index;
    if (location.sample_ordinal.has_value())
        out << " sample#" << *location.sample_ordinal;
    if (location.item_count.has_value())
        out << " count=" << *location.item_count;
    if (location.estimated_seconds.has_value())
        out << " est=" << detail::Fixed(*location.estimated_seconds, 3) << "s";
    if (!location.structure_path.empty())
        out << " path=" << location.structure_path;
    return out.str();
}

std::string BuildTextReport(const model::RepairResult& result) {
    std::ostringstream out;
    out << "VideoEye 视频修复报告\n";
    out << "=====================\n";
    out << "结果 ID: " << result.result_id << "\n";
    out << "请求 ID: " << result.request_id << "\n";
    out << "生成时间: " << result.created_at_iso8601 << "\n";
    out << "FFmpeg: " << result.ffmpeg_version << "\n\n";

    out << "结论\n----\n";
    out << "状态: " << model::DisplayName(result.status) << " (" << model::ToString(result.status) << ")\n";
    out << "摘要: " << result.summary << "\n";
    if (!result.error.empty())
        out << "错误: " << result.error << "\n";
    out << "产物: " << (result.output_path.empty() ? "（未发布）" : result.output_path) << "\n\n";

    out << "输入\n----\n";
    out << "路径: " << result.input.path << "\n";
    out << "大小: " << result.input.size_bytes << " bytes\n";
    out << "签名: " << (result.input.signature.empty() ? "unknown" : result.input.signature) << "\n\n";

    out << "诊断（" << result.diagnosis.checks.size() << " 项检查 / " << result.diagnosis.findings.size()
        << " 条证据）\n--------------------------------\n";
    for (const auto& check : result.diagnosis.checks) {
        out << "[" << model::ToString(check.status) << "] " << check.name << " — " << check.detail << "\n";
    }
    for (const auto& finding : result.diagnosis.findings) {
        out << "- " << finding.code << " [" << model::ToString(finding.layer) << "/"
            << model::ToString(finding.severity) << "] " << finding.evidence;
        const std::string location = LocationText(finding.location);
        if (!location.empty())
            out << " @ " << location;
        out << "\n";
        if (!finding.limitations.empty()) {
            for (const auto& limitation : finding.limitations)
                out << "    限制: " << limitation << "\n";
        }
    }
    out << "\n覆盖: bytes=" << result.diagnosis.coverage.bytes_scanned
        << " packets=" << result.diagnosis.coverage.packets_scanned
        << " reached_eof=" << (result.diagnosis.coverage.reached_eof ? "yes" : "no") << "\n";
    if (!result.diagnosis.expected_tracks.empty()) {
        out << "源侧样本数基线: ";
        for (const auto& expected : result.diagnosis.expected_tracks) {
            out << "轨道 " << expected.track_id << "=" << expected.sample_count << " (" << expected.provenance << ") ";
        }
        out << "\n";
    }
    for (const auto& limitation : result.diagnosis.coverage.limitations)
        out << "  限制: " << limitation << "\n";
    out << "\n";

    out << "方案与执行\n----------\n";
    for (const auto& action : result.plan.actions) {
        out << "- " << model::ToString(action.action) << " (" << model::DisplayName(action.action) << ") "
            << (action.available ? "可用" : "不可用: " + action.unavailable_reason) << "\n";
    }
    for (const auto& execution : result.executions) {
        out << "  执行 " << model::ToString(execution.action) << ": attempted=" << (execution.attempted ? "yes" : "no")
            << " succeeded=" << (execution.succeeded ? "yes" : "no") << " packets=" << execution.packets_written
            << " bytes=" << execution.bytes_written << "\n";
        if (!execution.detail.empty())
            out << "    " << execution.detail << "\n";
    }
    out << "\n";

    out << "验证（" << model::DisplayName(result.verification.level) << "）\n------------------------\n";
    out << "状态: " << model::ToString(result.verification.status) << "；帧=" << result.verification.decoded_frames
        << " 错误=" << result.verification.decode_errors << " 跳转=" << result.verification.seek_passes << "/"
        << result.verification.seek_checks << "\n";
    out << result.verification.evidence << "\n";
    for (const auto& check : result.verification.checks)
        out << "[" << model::ToString(check.status) << "] " << check.name << " — " << check.detail << "\n";
    out << "\n";

    out << "保真度\n------\n";
    out << "视频: " << model::ToString(result.fidelity.video) << "；音频: " << model::ToString(result.fidelity.audio)
        << "\n";
    out << "重编码: video=" << (result.fidelity.video_reencoded ? "yes" : "no")
        << " audio=" << (result.fidelity.audio_reencoded ? "yes" : "no")
        << "；合成帧: " << (result.fidelity.synthetic_frames_inserted ? "yes" : "no")
        << "；时间轴改写: " << (result.fidelity.timeline_modified ? "yes" : "no") << "\n";
    out << "内容完整性: " << result.fidelity.content_completeness
        << "；感知保真度: " << result.fidelity.perceptual_fidelity << "\n";
    for (const auto& limitation : result.fidelity.limitations)
        out << "  限制: " << limitation << "\n";
    return out.str();
}

std::string EscapeHtml(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string BuildHtmlReport(const model::RepairResult& result) {
    std::ostringstream html;
    html << "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
         << "<title>VideoEye 修复报告</title><style>"
         << "body{font-family:-apple-system,'Segoe UI',sans-serif;background:#f6f7f9;color:#1f2328;margin:24px}"
         << ".card{background:#fff;border-radius:10px;padding:20px 24px;margin-bottom:16px;box-shadow:0 1px 4px "
            "rgba(0,0,0,.08)}"
         << "h1{font-size:22px;margin:0 0 8px}h2{font-size:16px;margin:0 0 12px;color:#57606a}"
         << "table{border-collapse:collapse;width:100%;font-size:13px}td,th{border-bottom:1px solid "
            "#eaeef2;padding:6px 8px;text-align:left;vertical-align:top}"
         << ".ok{color:#1a7f37}.fail{color:#cf222e}.warn{color:#9a6700}.muted{color:#57606a}"
         << "code{background:#f0f2f4;border-radius:4px;padding:1px 4px}"
         << "</style></head><body>";
    html << "<div class=\"card\"><h1>VideoEye 视频修复报告</h1>"
         << "<p class=\"muted\">" << EscapeHtml(result.result_id) << " · " << EscapeHtml(result.created_at_iso8601)
         << "</p><h2>结论</h2><p><strong>" << EscapeHtml(model::DisplayName(result.status)) << "</strong> ("
         << EscapeHtml(model::ToString(result.status)) << ")</p><p>" << EscapeHtml(result.summary) << "</p>";
    if (!result.output_path.empty())
        html << "<p>产物: <code>" << EscapeHtml(result.output_path) << "</code></p>";
    html << "</div>";

    html << "<div "
            "class=\"card\"><h2>诊断证据</h2><table><tr><th>代码</th><th>层/严重度</th><th>证据</th><th>定位</th></tr>";
    for (const auto& finding : result.diagnosis.findings) {
        const char* cls =
            finding.severity == model::IssueSeverity::Critical || finding.severity == model::IssueSeverity::Error
                ? "fail"
                : "warn";
        html << "<tr><td><code>" << EscapeHtml(finding.code) << "</code></td><td class=\"" << cls << "\">"
             << EscapeHtml(model::ToString(finding.layer)) << " / " << EscapeHtml(model::ToString(finding.severity))
             << "</td><td>" << EscapeHtml(finding.evidence) << "</td><td class=\"muted\">"
             << EscapeHtml(LocationText(finding.location)) << "</td></tr>";
    }
    html << "</table></div>";

    html << "<div class=\"card\"><h2>验证（" << EscapeHtml(model::DisplayName(result.verification.level)) << "）</h2>"
         << "<p class=\"" << (result.verification.status == model::RepairCheckStatus::Passed ? "ok" : "warn") << "\">"
         << EscapeHtml(result.verification.evidence) << "</p><table><tr><th>检查</th><th>状态</th><th>说明</th></tr>";
    for (const auto& check : result.verification.checks)
        html << "<tr><td>" << EscapeHtml(check.name) << "</td><td>" << EscapeHtml(model::ToString(check.status))
             << "</td><td class=\"muted\">" << EscapeHtml(check.detail) << "</td></tr>";
    html << "</table></div>";

    html << "<div class=\"card\"><h2>保真度</h2><p>视频: " << EscapeHtml(model::ToString(result.fidelity.video))
         << "；音频: " << EscapeHtml(model::ToString(result.fidelity.audio))
         << "；内容完整性: " << EscapeHtml(result.fidelity.content_completeness)
         << "；感知保真度: " << EscapeHtml(result.fidelity.perceptual_fidelity) << "</p><p class=\"muted\">"
         << EscapeHtml(result.fidelity.timeline_note) << "</p></div>";

    html << "</body></html>";
    return html.str();
}

} // namespace

RepairReportWriteResult RepairReportExporter::WriteAll(const model::RepairResult& result, const std::string& json_path,
                                                       const std::string& text_path, const std::string& html_path) {
    RepairReportWriteResult outcome;
    if (!json_path.empty()) {
        const std::string json = BuildResultJson(result).ToPrettyString(2) + "\n";
        outcome.json_ok = detail::WriteUtf8File(json_path, json);
    }
    if (!text_path.empty()) {
        outcome.text_ok = detail::WriteUtf8File(text_path, BuildTextReport(result));
    }
    if (!html_path.empty()) {
        outcome.html_ok = detail::WriteUtf8File(html_path, BuildHtmlReport(result));
    }
    if (!outcome.AllOk())
        outcome.error = "报告写入失败";
    return outcome;
}

} // namespace reporting
} // namespace videoeye
