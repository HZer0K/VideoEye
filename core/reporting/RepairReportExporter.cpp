#include "core/reporting/RepairReportExporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

#include "core/reporting/QcReportExporterInternal.h"
#include "infrastructure/serialization/Json.h"

namespace videoeye {
namespace reporting {
namespace {

using JsonValue = videoeye::JsonValue;

constexpr const char* kRepairSchemaVersion = "1.0";

JsonValue BuildTrackConservationArray(const std::vector<model::RepairTrackConservation>& records);
JsonValue BuildTrackVerificationArray(const std::vector<model::RepairTrackVerification>& tracks);
JsonValue BuildTrackSalvageArray(const std::vector<model::RepairTrackSalvageInfo>& tracks);
JsonValue BuildRecoveredSegments(const std::vector<model::RepairRecoveredSegment>& segments);
JsonValue BuildTimelineRepair(const model::RepairTimelineRepair& repair);

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

// TXT / HTML 里的秒数统一三位小数，避免不同格式把同一个实测值显示成不同精度。
std::string FixedSeconds(double value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return std::string(buffer);
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
    object.Set("content_hash", JOptString(input.content_hash));
    object.Set("hash_method", JText(input.hash_method));
    object.Set("hashed_bytes", J64(input.hashed_bytes));
    return object;
}

JsonValue BuildCapabilityBoundaryArray(const std::vector<model::RepairCapabilityBoundary>& boundaries) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& boundary : boundaries) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("code", JText(boundary.code));
        node.Set("status", JText(boundary.status));
        node.Set("status_label", JText(boundary.status_label));
        node.Set("status_headline", JText(boundary.status_headline));
        node.Set("covered_scope", BuildStringArray(boundary.covered_scope));
        node.Set("not_covered_scope", BuildStringArray(boundary.not_covered_scope));
        node.Set("preconditions", BuildStringArray(boundary.preconditions));
        node.Set("unrecoverable_scope", BuildStringArray(boundary.unrecoverable_scope));
        node.Set("timeline_impact", JText(boundary.timeline_impact));
        node.Set("quality_risks", BuildStringArray(boundary.quality_risks));
        node.Set("evidence", JText(boundary.evidence));
        node.Set("trigger_findings", BuildStringArray(boundary.trigger_findings));
        array.PushBack(node);
    }
    return array;
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
    object.Set("track_salvage", BuildTrackSalvageArray(diagnosis.track_salvage));
    object.Set("capability_boundaries", BuildCapabilityBoundaryArray(diagnosis.capability_boundaries));
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
        node.Set("capability_boundary", JOptString(action.capability_boundary));
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
    object.Set("corrupt_packets", J64(verification.corrupt_packets));
    object.Set("corrupt_frames", J64(verification.corrupt_frames));
    object.Set("error_recovery_frames", J64(verification.error_recovery_frames));
    object.Set("valid_frames", J64(verification.valid_frames));
    object.Set("frame_marker_evidence", JText(verification.frame_marker_evidence));
    object.Set("marked_frames", J64(verification.marked_frames));
    object.Set("frame_marker_limitations", BuildStringArray(verification.frame_marker_limitations));
    object.Set("structure_only", JsonValue(verification.structure_only));
    object.Set("expected_duration_seconds", JNum(verification.expected_duration_seconds));
    object.Set("observed_duration_seconds", JNum(verification.observed_duration_seconds));
    object.Set("duration_delta_seconds", JNum(verification.duration_delta_seconds));
    object.Set("timeline_comparable", JsonValue(verification.timeline_comparable));
    object.Set("timeline_mismatch", JsonValue(verification.timeline_mismatch));
    object.Set("timeline_checks", BuildStringArray(verification.timeline_checks));
    object.Set("av_sync_checked", JsonValue(verification.av_sync_checked));
    object.Set("av_sync_start_delta_seconds", JNum(verification.av_sync_start_delta_seconds));
    object.Set("av_sync_end_delta_seconds", JNum(verification.av_sync_end_delta_seconds));
    object.Set("av_sync_drift_seconds", JNum(verification.av_sync_drift_seconds));
    object.Set("av_sync_status", JText(verification.av_sync_status));
    object.Set("per_track", BuildTrackVerificationArray(verification.per_track));
    object.Set("seek_checks", J64(verification.seek_checks));
    object.Set("seek_passes", J64(verification.seek_passes));
    object.Set("decode_scope", JText(verification.decode_scope));
    object.Set("residual_findings", BuildFindings(verification.residual_findings));
    object.Set("evidence", JText(verification.evidence));
    return object;
}

// 阶段 6 / 切片 J：transcode.inspected=false 时所有明细必须为空，
// 让「没探测过」在 JSON 里也能和「探测过且无损失」区分开。
JsonValue BuildTranscodeFidelity(const model::RepairTranscodeFidelity& transcode) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("inspected", JsonValue(transcode.inspected));
    object.Set("source_pixel_format", JText(transcode.source_pixel_format));
    object.Set("output_pixel_format", JText(transcode.output_pixel_format));
    object.Set("source_bit_depth", JsonValue(transcode.source_bit_depth));
    object.Set("output_bit_depth", JsonValue(transcode.output_bit_depth));
    object.Set("source_width", JsonValue(transcode.source_width));
    object.Set("source_height", JsonValue(transcode.source_height));
    object.Set("color_primaries", JText(transcode.color_primaries));
    object.Set("color_transfer", JText(transcode.color_transfer));
    object.Set("color_space", JText(transcode.color_space));
    object.Set("color_range", JText(transcode.color_range));
    object.Set("chroma_location", JText(transcode.chroma_location));
    object.Set("sample_aspect_ratio", JText(transcode.sample_aspect_ratio));
    object.Set("rotation_degrees", JNum(transcode.rotation_degrees));
    object.Set("has_display_matrix", JsonValue(transcode.has_display_matrix));
    object.Set("has_hdr_metadata", JsonValue(transcode.has_hdr_metadata));
    object.Set("resolution_changed_frames", J64(transcode.resolution_changed_frames));
    object.Set("all_properties_preserved", JsonValue(transcode.all_properties_preserved));
    // 切片 O：编码器事实是机读字段，不是文案。LLM / 脚本判断「这份产物到底是谁编的」
    // 不该去正则匹配 limitations 里的自由文本。
    object.Set("encoder_preferred", JText(transcode.encoder_preferred));
    object.Set("encoder_used", JText(transcode.encoder_used));
    object.Set("encoder_fallback", JsonValue(transcode.encoder_fallback));
    object.Set("source_profile", JText(transcode.source_profile));
    object.Set("output_profile", JText(transcode.output_profile));
    object.Set("encoder_note", JText(transcode.encoder_note));
    object.Set("profile_note", JText(transcode.profile_note));
    // 切片 P：音画起点差是实测值，缺证据时留 null，不能用 0 冒充「已对齐」。
    object.Set("av_start_offset_seconds", JSeconds(transcode.av_start_offset_seconds));
    object.Set("av_start_offset_preserved", JsonValue(transcode.av_start_offset_preserved));
    object.Set("av_sync_note", JText(transcode.av_sync_note));
    // 切片 Q：容器级 side data 的三段事实分开落 —— 拷贝动作 / 产物实测 / 结论。
    // 合成一个布尔会让「没查」和「查了没有」长得一样。
    object.Set("display_matrix_attached", JsonValue(transcode.display_matrix_attached));
    object.Set("output_has_display_matrix", JsonValue(transcode.output_has_display_matrix));
    object.Set("output_rotation_degrees", JNum(transcode.output_rotation_degrees));
    object.Set("hdr_metadata_attached", JsonValue(transcode.hdr_metadata_attached));
    object.Set("output_has_hdr_metadata", JsonValue(transcode.output_has_hdr_metadata));
    object.Set("hdr_inherit_blocked_by_bit_depth", JsonValue(transcode.hdr_inherit_blocked_by_bit_depth));
    object.Set("side_data_output_measured", JsonValue(transcode.side_data_output_measured));
    object.Set("display_matrix_note", JText(transcode.display_matrix_note));
    object.Set("hdr_metadata_note", JText(transcode.hdr_metadata_note));
    object.Set("unverified_properties", BuildStringArray(transcode.unverified_properties));
    object.Set("preserved_properties", BuildStringArray(transcode.preserved_properties));
    object.Set("lost_properties", BuildStringArray(transcode.lost_properties));
    // 切片 T：轨级决策。机读字段是 action / reason_code（稳定标识），text 只给人看。
    // 覆盖自证靠 decision_count 与 input_stream_count 两个数，缺一不可 ——
    // 只给决策数组的话，「少了一条」在 JSON 里根本看不出来。
    {
        JsonValue decisions = JsonValue::MakeArray();
        for (const auto& decision : transcode.track_decisions) {
            JsonValue node = JsonValue::MakeObject();
            node.Set("input_stream_index", JsonValue(decision.input_stream_index));
            node.Set("kind", JText(decision.kind));
            node.Set("source_codec", JText(decision.source_codec));
            node.Set("action", JText(decision.action));
            node.Set("reason_code", JText(decision.reason_code));
            node.Set("reason", JText(decision.reason));
            node.Set("text", JText(decision.text));
            decisions.PushBack(node);
        }
        object.Set("track_decisions", decisions);
    }
    object.Set("track_decision_input_streams", JsonValue(transcode.track_decision_input_streams));
    object.Set("track_decision_count", JsonValue(static_cast<int>(transcode.track_decisions.size())));
    object.Set("track_decision_summary", JText(transcode.track_decision_summary));
    object.Set("track_decision_note", JText(transcode.track_decision_note));
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
    object.Set("timeline_status", JText(fidelity.timeline_status));
    object.Set("timeline_note", JText(fidelity.timeline_note));
    object.Set("content_completeness", JText(fidelity.content_completeness));
    object.Set("original_content_recovery_ratio", JSeconds(fidelity.original_content_recovery_ratio));
    object.Set("perceptual_fidelity", JText(fidelity.perceptual_fidelity));
    object.Set("reference_compatibility", JText(fidelity.reference_compatibility));
    object.Set("borrowed_parameters", BuildStringArray(fidelity.borrowed_parameters));
    object.Set("omitted_items", BuildStringArray(fidelity.omitted_items));
    object.Set("limitations", BuildStringArray(fidelity.limitations));
    object.Set("transcode", BuildTranscodeFidelity(fidelity.transcode));
    return object;
}

JsonValue BuildTrackSalvageArray(const std::vector<model::RepairTrackSalvageInfo>& tracks) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& track : tracks) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("track_id", JsonValue(static_cast<int>(track.track_id)));
        node.Set("input_stream_index", JsonValue(track.input_stream_index));
        node.Set("kind", JText(track.kind));
        node.Set("salvageable", JsonValue(track.salvageable));
        node.Set("evidence", JText(track.evidence));
        array.PushBack(node);
    }
    return array;
}

JsonValue BuildRecoveredSegments(const std::vector<model::RepairRecoveredSegment>& segments) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& segment : segments) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("index", JsonValue(static_cast<int>(segment.index)));
        node.Set("input_start_seconds", JNum(segment.input_start_seconds));
        node.Set("input_end_seconds", JNum(segment.input_end_seconds));
        node.Set("output_start_seconds", JNum(segment.output_start_seconds));
        node.Set("output_end_seconds", JNum(segment.output_end_seconds));
        node.Set("packets_written", J64(segment.packets_written));
        node.Set("resumed_after_damage", JsonValue(segment.resumed_after_damage));
        node.Set("skipped_before_seconds", JNum(segment.skipped_before_seconds));
        // 恢复点来源单独落机读码：TXT/HTML 的文案可以改，JSON 契约不能漂。
        node.Set("recovery_evidence", JText(model::ToString(segment.recovery_evidence)));
        node.Set("recovery_point", JOptString(segment.recovery_point));
        array.PushBack(node);
    }
    return array;
}

// 损失区间：与 recovered_segments 一一对应的「跳过段」视图。字节边界拿不到时不写
// 这两个键，绝不填 0 冒充起点。
JsonValue BuildLostRanges(const std::vector<model::RepairLostRange>& ranges) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& range : ranges) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("input_start_seconds", JNum(range.input_start_seconds));
        node.Set("input_end_seconds", JNum(range.input_end_seconds));
        node.Set("duration_seconds", JNum(range.DurationSeconds()));
        if (range.byte_start.has_value())
            node.Set("byte_start", J64(*range.byte_start));
        if (range.byte_end.has_value())
            node.Set("byte_end", J64(*range.byte_end));
        node.Set("reason", JText(range.reason));
        node.Set("evidence", JText(range.evidence));
        node.Set("recoverable", JsonValue(range.recoverable));
        array.PushBack(node);
    }
    return array;
}

// 样本层摘要：total_samples / total_bytes 是全量实测值，retained_detail 只是
// 明细缓存保留条数。available=false 表示本次执行没有接入账本，不能读成「零样本」。
JsonValue BuildSampleLedger(const model::RepairSampleLedgerSummary& ledger, const std::vector<std::string>& problems) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("available", JsonValue(ledger.available));
    object.Set("total_samples", J64(ledger.total_samples));
    object.Set("total_bytes", J64(ledger.total_bytes));
    object.Set("retained_detail", J64(ledger.retained_detail));
    object.Set("dropped_detail", J64(ledger.dropped_detail));
    object.Set("detail_truncated", JsonValue(ledger.detail_truncated));
    object.Set("by_track_count", J64(ledger.by_track_count));
    object.Set("by_segment_count", J64(ledger.by_segment_count));
    object.Set("verified_samples", J64(ledger.verified_samples));
    object.Set("inferred_samples", J64(ledger.inferred_samples));
    object.Set("decoded_ok_samples", J64(ledger.decoded_ok_samples));
    object.Set("damaged_samples", J64(ledger.damaged_samples));
    JsonValue limitations = JsonValue::MakeArray();
    for (const auto& text : ledger.limitations)
        limitations.PushBack(JText(text));
    object.Set("limitations", limitations);
    JsonValue issues = JsonValue::MakeArray();
    for (const auto& text : problems)
        issues.PushBack(JText(text));
    object.Set("conservation_problems", issues);
    return object;
}

// 切片 C：参考样片辅助重建的借用清单与核验结论。三格式必须同一结论，
// 不能只在 JSON 里说「借了参数」而 TXT/HTML 只报「已修复」。
JsonValue BuildReferenceAssist(const model::RepairReferenceAssist& assist) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("used", JsonValue(assist.used));
    object.Set("reference_name", JOptString(assist.reference_name));
    object.Set("compatibility", JOptString(assist.compatibility));
    object.Set("compatibility_reason", JOptString(assist.compatibility_reason));
    object.Set("layout_evidence", JOptString(assist.layout_evidence));
    object.Set("timescale_check", JOptString(assist.timescale_check));
    JsonValue borrowed = JsonValue::MakeArray();
    for (const auto& text : assist.borrowed_parameters)
        borrowed.PushBack(JText(text));
    object.Set("borrowed_parameters", borrowed);
    JsonValue limitations = JsonValue::MakeArray();
    for (const auto& text : assist.limitations)
        limitations.PushBack(JText(text));
    object.Set("limitations", limitations);
    return object;
}

JsonValue BuildTimelineRepair(const model::RepairTimelineRepair& repair) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("attempted", JsonValue(repair.attempted));
    object.Set("status", JText(repair.status));
    object.Set("gap_removed", JsonValue(repair.gap_removed));
    object.Set("removed_gap_seconds", JNum(repair.removed_gap_seconds));
    object.Set("timestamp_regressions", J64(repair.timestamp_regressions));
    object.Set("timestamp_duplicates", J64(repair.timestamp_duplicates));
    object.Set("missing_timestamps", J64(repair.missing_timestamps));
    object.Set("synthesized_timestamps", J64(repair.synthesized_timestamps));
    object.Set("rewritten_timestamps", J64(repair.rewritten_timestamps));
    object.Set("note", JText(repair.note));
    return object;
}

// 显示顺序（切片 E）：analyzed / available / ctts_written 三个标志分开落，
// 因为"跑过测量"、"测量给了结论"、"产物里真写了 ctts" 是三件不同的事。
JsonValue BuildDisplayOrder(const model::RepairDisplayOrderSummary& order) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("analyzed", JsonValue(order.analyzed));
    object.Set("available", JsonValue(order.available));
    object.Set("unavailable_reason", JOptString(order.unavailable_reason));
    object.Set("codec", JOptString(order.codec));
    object.Set("order_source", JOptString(order.order_source));
    object.Set("basis", JOptString(order.basis));
    object.Set("confidence", JOptString(order.confidence));
    object.Set("duration_source", JText(order.duration_source));
    object.Set("timescale", JsonValue(static_cast<int>(order.timescale)));
    object.Set("sample_delta", JsonValue(static_cast<int>(order.sample_delta)));
    object.Set("frames", J64(order.frames));
    object.Set("frames_observed", J64(order.frames_observed));
    object.Set("truncated", JsonValue(order.truncated));
    object.Set("rap_segments", J64(order.rap_segments));
    object.Set("reordered_frames", J64(order.reordered_frames));
    object.Set("max_reorder", J64(order.max_reorder));
    object.Set("ctts_required", JsonValue(order.ctts_required));
    object.Set("ctts_written", JsonValue(order.ctts_written));
    JsonValue limitations = JsonValue::MakeArray();
    for (const auto& text : order.limitations)
        limitations.PushBack(JText(text));
    object.Set("limitations", limitations);
    return object;
}

JsonValue BuildTimelineBasis(const model::RepairTimelineBasis& basis) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("mode", JText(basis.mode));
    object.Set("frame_rate", JText(basis.frame_rate));
    object.Set("constant", JsonValue(basis.constant));
    object.Set("vfr_declared", JsonValue(basis.vfr_declared));
    object.Set("detail", JText(basis.detail));
    JsonValue limitations = JsonValue::MakeArray();
    for (const auto& text : basis.limitations)
        limitations.PushBack(JText(text));
    object.Set("limitations", limitations);
    return object;
}

JsonValue BuildTrackCoverage(const model::RepairTrackCoverageSummary& coverage) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("available", JsonValue(coverage.available));
    object.Set("complete", JsonValue(coverage.complete));
    object.Set("unavailable_reason", JOptString(coverage.unavailable_reason));
    object.Set("common_span_known", JsonValue(coverage.common_span_known));
    object.Set("common_start_seconds", JNum(coverage.common_start_seconds));
    object.Set("common_end_seconds", JNum(coverage.common_end_seconds));
    object.Set("common_span_note", JOptString(coverage.common_span_note));
    JsonValue tracks = JsonValue::MakeArray();
    for (const auto& track : coverage.tracks) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("output_stream_index", JsonValue(track.output_stream_index));
        node.Set("track_id", JsonValue(static_cast<int>(track.track_id)));
        node.Set("kind", JText(track.kind));
        node.Set("codec", JOptString(track.codec));
        node.Set("samples", J64(track.samples));
        node.Set("samples_without_timestamp", J64(track.samples_without_timestamp));
        node.Set("span_known", JsonValue(track.span_known));
        node.Set("first_seconds", JNum(track.first_seconds));
        node.Set("last_seconds", JNum(track.last_seconds));
        node.Set("typical_interval_seconds", JNum(track.typical_interval_seconds));
        node.Set("gap_count", J64(track.gap_count));
        node.Set("gap_seconds", JNum(track.gap_seconds));
        node.Set("gaps_truncated", JsonValue(track.gaps_truncated));
        JsonValue gaps = JsonValue::MakeArray();
        for (const auto& gap : track.gaps) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("start_seconds", JNum(gap.start_seconds));
            item.Set("end_seconds", JNum(gap.end_seconds));
            item.Set("duration_seconds", JNum(gap.DurationSeconds()));
            gaps.PushBack(item);
        }
        node.Set("gaps", gaps);
        tracks.PushBack(node);
    }
    object.Set("tracks", tracks);
    JsonValue limitations = JsonValue::MakeArray();
    for (const auto& text : coverage.limitations)
        limitations.PushBack(JText(text));
    object.Set("limitations", limitations);
    return object;
}

JsonValue BuildTrackConservationArray(const std::vector<model::RepairTrackConservation>& records) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& record : records) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("track_id", JsonValue(static_cast<int>(record.track_id)));
        node.Set("input_stream_index", JsonValue(record.input_stream_index));
        node.Set("output_stream_index", JsonValue(record.output_stream_index));
        node.Set("kind", JText(record.kind));
        node.Set("expected_samples", J64(record.expected_samples));
        node.Set("packets_written", J64(record.packets_written));
        node.Set("status", JText(record.status));
        node.Set("relation", JText(record.relation));
        node.Set("note", JOptString(record.note));
        array.PushBack(node);
    }
    return array;
}

JsonValue BuildTrackVerificationArray(const std::vector<model::RepairTrackVerification>& tracks) {
    JsonValue array = JsonValue::MakeArray();
    for (const auto& track : tracks) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("track_id", JsonValue(static_cast<int>(track.track_id)));
        node.Set("output_stream_index", JsonValue(track.output_stream_index));
        node.Set("kind", JText(track.kind));
        node.Set("status", JText(model::ToString(track.status)));
        node.Set("verified", JsonValue(track.verified));
        node.Set("packets", J64(track.packets));
        node.Set("frames", J64(track.frames));
        node.Set("errors", J64(track.errors));
        node.Set("corrupt_packets", J64(track.corrupt_packets));
        node.Set("corrupt_frames", J64(track.corrupt_frames));
        node.Set("error_recovery_frames", J64(track.error_recovery_frames));
        node.Set("valid_frames", J64(track.valid_frames));
        node.Set("duration_seconds", JNum(track.duration_seconds));
        node.Set("scope", JOptString(track.scope));
        node.Set("detail", JOptString(track.detail));
        array.PushBack(node);
    }
    return array;
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

// 切片 L：本次明确未恢复项。JSON 与 TXT/HTML 走同一份 items，
// available=false / items 为空都要显式落字段，不许静默省略。
JsonValue BuildDeclaredExclusionsJson(const model::RepairDeclaredExclusions& declared) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("available", JsonValue(declared.available));
    object.Set("unavailable_reason", JOptString(declared.unavailable_reason));
    JsonValue items = JsonValue::MakeArray();
    for (const auto& item : declared.items) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("code", JText(item.code));
        node.Set("title", JText(item.title));
        node.Set("scope", JText(item.scope));
        node.Set("status", JText(item.status));
        node.Set("applies_to", JText(item.applies_to));
        node.Set("trigger_facts", BuildStringArray(item.trigger_facts));
        node.Set("consequence", JText(item.consequence));
        node.Set("evidence", JText(item.evidence));
        items.PushBack(node);
    }
    object.Set("items", items);
    object.Set("limitations", BuildStringArray(declared.limitations));
    return object;
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
    output.Set("container", JOptString(result.output_container));
    root.Set("output", output);

    root.Set("input", BuildInput(result.input));
    root.Set("reference_input", BuildInput(result.reference_input));
    root.Set("reference_snapshot_valid", JsonValue(result.reference_snapshot_valid));
    root.Set("input_check_method", JText(result.input_check_method));
    root.Set("diagnosis", BuildDiagnosis(result.diagnosis));
    root.Set("plan", BuildPlan(result.plan));

    JsonValue executions = JsonValue::MakeArray();
    for (const auto& execution : result.executions)
        executions.PushBack(BuildExecution(execution));
    root.Set("executions", executions);

    root.Set("verification", BuildVerification(result.verification));
    root.Set("fidelity", BuildFidelity(result.fidelity));
    root.Set("track_conservation", BuildTrackConservationArray(result.track_conservation));
    root.Set("sample_ledger", BuildSampleLedger(result.sample_ledger, result.sample_conservation_problems));
    root.Set("recovered_segments", BuildRecoveredSegments(result.recovered_segments));
    root.Set("lost_ranges", BuildLostRanges(result.lost_ranges));
    root.Set("track_coverage", BuildTrackCoverage(result.track_coverage));
    root.Set("reference_assist", BuildReferenceAssist(result.reference_assist));
    root.Set("timeline_repair", BuildTimelineRepair(result.timeline_repair));
    root.Set("display_order", BuildDisplayOrder(result.display_order));
    root.Set("timeline_basis", BuildTimelineBasis(result.timeline_basis));
    root.Set("declared_exclusions", BuildDeclaredExclusionsJson(result.declared_exclusions));
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

// ---- R13：统一的报告视图模型 ----
//
// 以前 JSON / TXT / HTML 各写各的：TXT 和 HTML 只遍历诊断阶段 findings，
// 结果级的 residual_findings（修复/验证后才发现的内容缺失、损坏帧、参考段风险）
// 只有 JSON 有；HTML 还少了保真度限制与执行明细。现在三种格式都从同一个
// sections 视图渲染，任何一条残留问题都不会在人类可读报告里凭空消失。
struct ReportSection {
    std::string title;
    std::vector<std::pair<std::string, std::string>> rows;
    std::vector<std::string> items;
};

std::string FindingLine(const model::DamageFinding& finding) {
    std::ostringstream line;
    line << finding.code << " [" << model::ToString(finding.layer) << "/" << model::ToString(finding.severity) << "] "
         << finding.evidence;
    const std::string location = LocationText(finding.location);
    if (!location.empty())
        line << " @ " << location;
    for (const auto& limitation : finding.limitations)
        line << " | 限制: " << limitation;
    return line.str();
}

std::vector<ReportSection> BuildSections(const model::RepairResult& result) {
    std::vector<ReportSection> sections;

    ReportSection conclusion;
    conclusion.title = "结论";
    conclusion.rows.emplace_back("状态", std::string(model::DisplayName(result.status)) + " (" +
                                             model::ToString(result.status) + ")");
    conclusion.rows.emplace_back("摘要", result.summary);
    if (!result.error.empty())
        conclusion.rows.emplace_back("错误", result.error);
    conclusion.rows.emplace_back("产物", result.output_path.empty() ? "（未发布）" : result.output_path);
    conclusion.rows.emplace_back("产物容器", result.output_container.empty() ? "（未知）" : result.output_container);
    conclusion.rows.emplace_back("已发布", result.output_published ? "yes" : "no");
    conclusion.rows.emplace_back("验证等级", model::DisplayName(result.verification.level));
    conclusion.rows.emplace_back("逐轨验证条数", std::to_string(result.verification.per_track.size()));
    conclusion.rows.emplace_back("残留问题条数", std::to_string(result.residual_findings.size()));

    // 四项结论：避免用单一"修复成功"掩盖内容、时间轴与画质三方面的差异。
    conclusion.rows.emplace_back("播放验证", std::string(model::ToString(result.verification.status)) +
                                                 (result.verification.structure_only ? "（仅结构验证，尚未验证播放）"
                                                                                     : "（已做播放 / 解码验证）"));
    conclusion.rows.emplace_back(
        "内容保留", result.fidelity.content_completeness +
                        (result.fidelity.original_content_recovery_ratio.has_value()
                             ? "（恢复比例 " + FixedSeconds(*result.fidelity.original_content_recovery_ratio) + "）"
                             : "（恢复比例未量化）"));
    conclusion.rows.emplace_back(
        "时间轴变化", result.fidelity.timeline_status + " — " +
                          (result.fidelity.timeline_note.empty() ? "无记录" : result.fidelity.timeline_note));
    conclusion.rows.emplace_back("画质风险", (result.fidelity.video_reencoded || result.fidelity.audio_reencoded
                                                  ? "存在重编码；画质风险需人工复核"
                                                  : "保码流重建；未观察到编码层损坏证据不等于无失真") +
                                                 std::string("（感知保真度: ") + result.fidelity.perceptual_fidelity +
                                                 "）");
    sections.push_back(std::move(conclusion));

    ReportSection input;
    input.title = "输入与快照";
    input.rows.emplace_back("源路径", result.input.path);
    input.rows.emplace_back("源大小", std::to_string(result.input.size_bytes) + " bytes");
    input.rows.emplace_back("源签名", result.input.signature.empty() ? "unknown" : result.input.signature);
    input.rows.emplace_back("源内容摘要", result.input.content_hash.empty()
                                              ? "unavailable"
                                              : result.input.content_hash + " (" + result.input.hash_method + ")");
    if (result.reference_snapshot_valid) {
        input.rows.emplace_back("参考路径", result.reference_input.path);
        input.rows.emplace_back("参考内容摘要", result.reference_input.content_hash.empty()
                                                    ? "unavailable"
                                                    : result.reference_input.content_hash + " (" +
                                                          result.reference_input.hash_method + ")");
    }
    input.rows.emplace_back("发布前核对方法", result.input_check_method);
    sections.push_back(std::move(input));

    ReportSection diagnosis;
    diagnosis.title = "诊断";
    for (const auto& check : result.diagnosis.checks)
        diagnosis.items.push_back("[" + std::string(model::ToString(check.status)) + "] " + check.name + " — " +
                                  check.detail);
    for (const auto& finding : result.diagnosis.findings)
        diagnosis.items.push_back(FindingLine(finding));
    diagnosis.items.push_back("覆盖: bytes=" + std::to_string(result.diagnosis.coverage.bytes_scanned) +
                              " packets=" + std::to_string(result.diagnosis.coverage.packets_scanned) +
                              " reached_eof=" + (result.diagnosis.coverage.reached_eof ? "yes" : "no"));
    for (const auto& expected : result.diagnosis.expected_tracks)
        diagnosis.items.push_back("源侧样本数基线: 轨道 " + std::to_string(expected.track_id) + "=" +
                                  std::to_string(expected.sample_count) + " (" + expected.provenance + ")");
    for (const auto& track : result.diagnosis.track_salvage)
        diagnosis.items.push_back("逐轨可抢救证据: 轨道 " + std::to_string(track.track_id) + " (" + track.kind + ") " +
                                  (track.salvageable ? "可用" : "不可用") + " — " + track.evidence);
    for (const auto& reason : result.diagnosis.blocking_reasons)
        diagnosis.items.push_back("能力阻断: " + reason);
    for (const auto& limitation : result.diagnosis.coverage.limitations)
        diagnosis.items.push_back("覆盖限制: " + limitation);
    sections.push_back(std::move(diagnosis));

    // 命中才输出的边界契约：支持程度 / 已覆盖 / 未覆盖 / 前提 / 不可恢复范围 /
    // 时间轴变化 / 质量风险逐条可见，三种格式都从这一份 sections 视图渲染。
    if (!result.diagnosis.capability_boundaries.empty()) {
        ReportSection boundaries;
        // 标题不再写「未实现」：三条能力现在都有受限实现，标题这么写会先把读者带偏。
        boundaries.title = "恢复能力边界（本次命中）";
        boundaries.rows.emplace_back("命中条数", std::to_string(result.diagnosis.capability_boundaries.size()));
        boundaries.rows.emplace_back("状态口径", "状态词描述本版本对这项能力的实现程度，不代表本次运行是否生效；"
                                                 "本次生效情况看规划层的策略状态与命中证据");
        for (const auto& boundary : result.diagnosis.capability_boundaries) {
            std::string triggers;
            for (const auto& code : boundary.trigger_findings) {
                if (!triggers.empty())
                    triggers += ", ";
                triggers += code;
            }
            boundaries.items.push_back(boundary.code + " [" +
                                       (boundary.status_label.empty() ? boundary.status : boundary.status_label) +
                                       "] 触发证据: " + (triggers.empty() ? "（无）" : triggers));
            boundaries.items.push_back("  支持程度: " + (boundary.status_headline.empty()
                                                             ? std::string("（未提供支持程度说明，不能据此判断做到哪）")
                                                             : boundary.status_headline));
            for (const auto& item : boundary.covered_scope)
                boundaries.items.push_back("  已覆盖范围: " + item);
            for (const auto& item : boundary.not_covered_scope)
                boundaries.items.push_back("  未覆盖范围: " + item);
            for (const auto& item : boundary.preconditions)
                boundaries.items.push_back("  前提: " + item);
            for (const auto& item : boundary.unrecoverable_scope)
                boundaries.items.push_back("  不可恢复范围: " + item);
            boundaries.items.push_back("  时间轴变化: " + boundary.timeline_impact);
            for (const auto& item : boundary.quality_risks)
                boundaries.items.push_back("  质量风险: " + item);
            boundaries.items.push_back("  判定依据: " + boundary.evidence);
        }
        sections.push_back(std::move(boundaries));
    }

    // 切片 L：「本次明确未恢复项」章节恒定输出。三种未测量/零命中态都要显式
    // 出行 —— 空章节被默认读成「没有问题」正是错误结论率红线禁止的。
    ReportSection declared;
    declared.title = "本次明确未恢复项";
    const auto& exclusions = result.declared_exclusions;
    if (!exclusions.available) {
        declared.rows.emplace_back("评估状态", "未评估");
        declared.rows.emplace_back("未评估原因", exclusions.unavailable_reason.empty() ? "（未记录）"
                                                                                       : exclusions.unavailable_reason);
        declared.items.push_back("未评估不等于没有明确未恢复项；本次结论层不完整");
    } else {
        declared.rows.emplace_back("评估状态", "已评估");
        declared.rows.emplace_back("命中条数", std::to_string(exclusions.items.size()));
        if (exclusions.items.empty())
            declared.rows.emplace_back("本次结论", "本次未触发任何明确未恢复项");
        for (const auto& item : exclusions.items) {
            declared.items.push_back(item.code + " [" + item.status + "] " + item.title +
                                     " — 适用: " + item.applies_to);
            for (const auto& fact : item.trigger_facts)
                declared.items.push_back("  触发事实: " + fact);
            declared.items.push_back("  对产物的影响: " + item.consequence);
            declared.items.push_back("  依据: " + item.evidence);
        }
    }
    for (const auto& limitation : exclusions.limitations)
        declared.items.push_back("清单限制: " + limitation);
    sections.push_back(std::move(declared));

    ReportSection execution;
    execution.title = "方案与执行";
    for (const auto& action : result.plan.actions) {
        std::string line = std::string(model::ToString(action.action)) + " (" + model::DisplayName(action.action) +
                           ") " + (action.available ? "可用" : "不可用: " + action.unavailable_reason);
        if (!action.capability_boundary.empty())
            line += " [边界契约: " + action.capability_boundary + "]";
        execution.items.push_back(line);
    }
    for (const auto& action : result.executions) {
        execution.items.push_back(
            std::string("执行 ") + model::ToString(action.action) + ": attempted=" + (action.attempted ? "yes" : "no") +
            " succeeded=" + (action.succeeded ? "yes" : "no") + " packets=" + std::to_string(action.packets_written) +
            " bytes=" + std::to_string(action.bytes_written));
        if (!action.detail.empty())
            execution.items.push_back("  " + action.detail);
    }
    sections.push_back(std::move(execution));

    // 恢复区间与时间轴：必须让读者看到"保留了哪些区间、改动了哪些时间"，而不是只看一个总时长。
    ReportSection recovery;
    recovery.title = "恢复区间与时间轴";
    recovery.rows.emplace_back("恢复区间数量", std::to_string(result.recovered_segments.size()));
    recovery.rows.emplace_back("样本层实测样本数", result.sample_ledger.available
                                                       ? std::to_string(result.sample_ledger.total_samples)
                                                       : "未接入");
    if (result.sample_ledger.available) {
        std::ostringstream ledger_line;
        ledger_line << "样本账本: 全量 " << result.sample_ledger.total_samples << " 个样本 / "
                    << result.sample_ledger.total_bytes << " 字节；明细保留 " << result.sample_ledger.retained_detail
                    << " 条";
        if (result.sample_ledger.detail_truncated)
            ledger_line << "（缓存达上限，聚合计数仍为全量实测值）";
        recovery.items.push_back(ledger_line.str());
        for (const auto& problem : result.sample_conservation_problems)
            recovery.items.push_back("样本守恒不一致: " + problem);
    }
    // 参考样片辅助重建：借了哪些解码配置、布局线索如何在损坏文件里自证、
    // 时基有没有独立证据。三格式同一结论，不能只在 JSON 里说明。
    if (result.reference_assist.used) {
        const model::RepairReferenceAssist& assist = result.reference_assist;
        recovery.rows.emplace_back("参考样片辅助重建",
                                   assist.reference_name + "（兼容性 " + assist.compatibility + "）");
        recovery.rows.emplace_back("借用的参数 / 布局线索", std::to_string(assist.borrowed_parameters.size()) + " 项");
        recovery.items.push_back("参考样片核验: " + assist.compatibility_reason);
        for (const auto& text : assist.borrowed_parameters)
            recovery.items.push_back("借用: " + text);
        if (!assist.layout_evidence.empty())
            recovery.items.push_back("源侧布局核验: " + assist.layout_evidence);
        if (!assist.timescale_check.empty())
            recovery.items.push_back("时基核对: " + assist.timescale_check);
        for (const auto& text : assist.limitations)
            recovery.items.push_back("限制: " + text);
    }
    // 显示顺序（切片 E）：只有真做过测量才出行；没做过不能默认"顺序没问题"。
    if (result.display_order.analyzed) {
        const model::RepairDisplayOrderSummary& order = result.display_order;
        if (order.available) {
            const std::string source = order.order_source.empty() ? std::string("poc") : order.order_source;
            recovery.rows.emplace_back("显示顺序", order.confidence + "（顺序值来源 " + source + "）");
            recovery.rows.emplace_back("随机接入点分段 / 重排帧数", std::to_string(order.rap_segments) + " 段 / " +
                                                                        std::to_string(order.reordered_frames) +
                                                                        " 帧（最大 " +
                                                                        std::to_string(order.max_reorder) + " 帧）");
            recovery.rows.emplace_back("合成时间偏移 (ctts)",
                                       order.ctts_written ? "已写入" : (order.ctts_required ? "应写但未写" : "不需要"));
            recovery.items.push_back("显示顺序依据: " + order.basis);
            recovery.items.push_back("帧时长证据: " + order.duration_source + "（timescale " +
                                     std::to_string(order.timescale) + "，每帧 " + std::to_string(order.sample_delta) +
                                     " tick）");
            if (order.truncated)
                recovery.items.push_back("显示顺序明细达到上限: 实测 " + std::to_string(order.frames_observed) +
                                         " 帧，参与排序 " + std::to_string(order.frames) + " 帧");
        } else {
            recovery.items.push_back("显示顺序未推导: " + order.unavailable_reason);
        }
        for (const auto& text : order.limitations)
            recovery.items.push_back("显示顺序限制: " + text);
    } else {
        // 切片 K：「没做过测量」也必须出行。整节静默省略时，读者无从区分
        // 「测过且顺序没问题」与「本策略根本没测」，等于默认给了一个乐观结论。
        recovery.rows.emplace_back("显示顺序", "未测量（本次未做 POC 推导）");
        recovery.items.push_back("显示顺序结论: 未独立核验，不能据此断言显示序与源片一致");
        if (!result.display_order.unavailable_reason.empty())
            recovery.items.push_back("显示顺序未测量原因: " + result.display_order.unavailable_reason);
        for (const auto& text : result.display_order.limitations)
            recovery.items.push_back("显示顺序限制: " + text);
    }
    // 时基依据（切片 F）：报告里每一个"秒"都来自这里。mode=unknown 表示没拿到
    // 证据，必须显式写「未取得」，不能反过来说"时间轴没问题"，也不能整节省略。
    if (result.timeline_basis.mode != "unknown") {
        const model::RepairTimelineBasis& basis = result.timeline_basis;
        std::string mode_text = basis.mode;
        if (basis.mode == "vui_timing")
            mode_text = "SPS VUI timing_info";
        else if (basis.mode == "user_input")
            mode_text = "用户显式帧率";
        else if (basis.mode == "container_pts")
            mode_text = "容器时间戳";
        else if (basis.mode == "adts_sample_rate")
            mode_text = "ADTS 采样率";
        else if (basis.mode == "reference_borrowed")
            mode_text = "参考样片时基（借用，源侧无独立证据）";
        else if (basis.mode == "reference_init_borrowed")
            mode_text = "参考初始化段时基（借用，源侧无独立时基证据）";
        else if (basis.mode == "synthesized_init_timescale")
            mode_text = "自建初始化段时基（源侧 sidx 声明，无原始 mdhd 可核对）";
        const bool has_rate = basis.frame_rate != "unknown";
        recovery.rows.emplace_back("时基依据", has_rate ? mode_text + "（" + basis.frame_rate + "）" : mode_text);
        if (basis.mode == "reference_borrowed") {
            // 切片 M：借用时基不是「拿到了证据」，而是「证据来自另一份文件」。
            // 只写一行 mode 会让读者把「有依据」误读成「源片时间轴已核验」。
            recovery.rows.emplace_back("时基证据边界", "源侧 moov 已丢失，无独立时基证据");
            recovery.items.push_back("时基结论: 借用的时基不能作为源片时间轴的证据，逐帧时长与源片可能不同");
        } else if (basis.mode == "reference_init_borrowed") {
            // 切片 N：fMP4 缺初始化段这条路借的是 mdhd timescale 与解码配置，
            // 逐样本时长仍来自源侧 moof/trun —— 与 C/H/I 借 stts 不同，边界必须分开写，
            // 否则读者会把「样本表也是借的」当成事实（或反过来以为时基已核验）。
            recovery.rows.emplace_back(
                "时基证据边界", "源侧 moov 已丢失；mdhd timescale 借自参考初始化段，逐样本时长来自源侧 moof/trun");
            recovery.items.push_back("时基结论: tick → 秒的换算依赖借来的 timescale，未做源侧独立核验，"
                                     "秒数不能作为源片时间轴的证据");
        } else if (basis.mode == "synthesized_init_timescale") {
            // 切片 S：自建初始化段既不是「源容器时间戳」（源片没有 moov），也不是「借来的」
            // （那份 moov 是我们自己写的）。timescale 只能取源侧 sidx 声明，证据边界必须单列。
            recovery.rows.emplace_back(
                "时基证据边界",
                "源侧 moov 已丢失；mdhd timescale 由自建初始化段按源侧 sidx 声明选定，不是源容器直接给出");
            recovery.items.push_back("时基结论: 逐样本 tick 原样来自源侧 moof/trun，但 tick → 秒的换算依赖"
                                     "自建时基，未与源片原始 mdhd 核对（已丢失），秒数不能作为源片时间轴的独立证据");
        }
        recovery.rows.emplace_back("帧时长是否恒定", basis.constant ? "是" : "否（逐帧时长无证据）");
        if (basis.vfr_declared)
            recovery.rows.emplace_back("码流声明非恒定帧率", "是（fixed_frame_rate_flag=0，按名义帧率重建）");
        if (!basis.detail.empty())
            recovery.items.push_back("时基细节: " + basis.detail);
        for (const auto& text : basis.limitations)
            recovery.items.push_back("时基限制: " + text);
    } else {
        // 切片 K：S08 等按推定帧率重建的路径就是这一态。以前整节消失，
        // 读者只能从别处猜「时间轴大概没问题」——现在如实写明没有证据。
        const model::RepairTimelineBasis& basis = result.timeline_basis;
        recovery.rows.emplace_back("时基依据", "未取得源侧时基证据（unknown）");
        recovery.items.push_back("时基结论: 报告里的秒数不能作为源片时间轴的证据；本次未取得帧时长 / 帧率依据");
        if (!basis.detail.empty())
            recovery.items.push_back("时基细节: " + basis.detail);
        for (const auto& text : basis.limitations)
            recovery.items.push_back("时基限制: " + text);
    }
    // 只写「确实损失」的区间：没有跳段时一行都不出，避免空表被读成「检查过且无损」。
    recovery.rows.emplace_back("损失区间数量", std::to_string(result.lost_ranges.size()));
    for (const auto& lost : result.lost_ranges) {
        std::ostringstream line;
        line << "损失: 输入 " << FixedSeconds(lost.input_start_seconds) << "–" << FixedSeconds(lost.input_end_seconds)
             << "s（" << FixedSeconds(lost.DurationSeconds()) << "s，" << lost.reason << "，"
             << (lost.recoverable ? "有其它副本可恢复" : "无其它副本") << "） — " << lost.evidence;
        recovery.items.push_back(line.str());
    }
    // 逐轨覆盖（切片 G）：全局损失区间说不清「是哪条轨缺的」，这里按轨逐个交代。
    // available=false 必须出一行 —— 静默省略会被读成「逐轨都没有缺口」。
    if (!result.track_coverage.available) {
        // 有账本却没上抛覆盖，是「没统计」而不是「没有数据」。旧兜底无条件写
        // 「本次执行没有样本账本」，在 S08 这类路径上是实打实的错误结论。
        std::string reason = result.track_coverage.unavailable_reason;
        if (reason.empty())
            reason = result.sample_ledger.available ? "有样本账本但未产出逐轨覆盖统计" : "本次执行没有样本账本";
        recovery.rows.emplace_back("逐轨覆盖", "未接入（" + reason + "）");
    } else {
        if (!result.track_coverage.complete)
            recovery.items.push_back("逐轨覆盖只统计到已扫描前缀（本次执行未走到源末尾），不能读成整段没内容");
        if (result.track_coverage.common_span_known) {
            recovery.rows.emplace_back("音画共同覆盖", FixedSeconds(result.track_coverage.common_start_seconds) + "–" +
                                                           FixedSeconds(result.track_coverage.common_end_seconds) +
                                                           "s（" +
                                                           FixedSeconds(result.track_coverage.common_end_seconds -
                                                                        result.track_coverage.common_start_seconds) +
                                                           "s）");
        } else {
            recovery.rows.emplace_back("音画共同覆盖", "未知（" + result.track_coverage.common_span_note + "）");
        }
        for (const auto& track : result.track_coverage.tracks) {
            std::ostringstream line;
            line << "轨 " << track.output_stream_index << "（" << (track.kind.empty() ? "other" : track.kind);
            if (!track.codec.empty())
                line << " / " << track.codec;
            line << "）：" << track.samples << " 个样本";
            if (track.span_known) {
                line << "，覆盖 " << FixedSeconds(track.first_seconds) << "–" << FixedSeconds(track.last_seconds)
                     << "s";
            } else {
                // 下一行会单独报「多少个样本无时间戳未计入」，这里不重复写同一事实。
                line << "，覆盖区间未知";
            }
            if (track.samples_without_timestamp > 0)
                line << "；" << track.samples_without_timestamp << " 个样本无时间戳未计入";
            line << "；缺口 " << track.gap_count << " 段 / " << FixedSeconds(track.gap_seconds) << "s";
            if (track.typical_interval_seconds > 0.0)
                line << "（判据 ≥" << FixedSeconds(std::max(0.5, 3.0 * track.typical_interval_seconds))
                     << "s，典型间隔约 " << FixedSeconds(track.typical_interval_seconds) << "s）";
            if (track.gaps_truncated)
                line << "，明细超过上限只列前 " << track.gaps.size() << " 段";
            recovery.items.push_back(line.str());
            for (const auto& gap : track.gaps) {
                std::ostringstream detail;
                detail << "  缺口: " << FixedSeconds(gap.start_seconds) << "–" << FixedSeconds(gap.end_seconds) << "s（"
                       << FixedSeconds(gap.DurationSeconds()) << "s）";
                recovery.items.push_back(detail.str());
            }
        }
        for (const auto& text : result.track_coverage.limitations)
            recovery.items.push_back("逐轨覆盖限制: " + text);
    }
    if (result.recovered_segments.empty()) {
        recovery.items.push_back("未发生跳段恢复：产物由单一连续区段组成");
    } else {
        for (const auto& segment : result.recovered_segments) {
            std::ostringstream line;
            line << "段 " << segment.index << ": 输入 " << FixedSeconds(segment.input_start_seconds) << "–"
                 << FixedSeconds(segment.input_end_seconds) << "s → 产物 " << FixedSeconds(segment.output_start_seconds)
                 << "–" << FixedSeconds(segment.output_end_seconds) << "s，写入 " << segment.packets_written << " 包";
            if (segment.resumed_after_damage) {
                line << "（跳段恢复";
                if (segment.skipped_before_seconds > 0.0)
                    line << "，跳过 " << FixedSeconds(segment.skipped_before_seconds) << "s";
                line << "，恢复点来源: " << model::DisplayName(segment.recovery_evidence);
                if (!segment.recovery_point.empty())
                    line << "，" << segment.recovery_point;
                line << "）";
            }
            recovery.items.push_back(line.str());
        }
    }
    recovery.rows.emplace_back("时间轴修复状态", result.timeline_repair.status);
    recovery.rows.emplace_back("重写时间戳", std::to_string(result.timeline_repair.rewritten_timestamps) + "（重复 " +
                                                 std::to_string(result.timeline_repair.timestamp_duplicates) +
                                                 " / 回退 " +
                                                 std::to_string(result.timeline_repair.timestamp_regressions) + "）");
    recovery.rows.emplace_back("缺失时间戳", std::to_string(result.timeline_repair.missing_timestamps) + "（已合成 " +
                                                 std::to_string(result.timeline_repair.synthesized_timestamps) +
                                                 "，其余未伪造）");
    recovery.rows.emplace_back("缺口消除", (result.timeline_repair.gap_removed ? "yes" : "no") + std::string("（") +
                                               FixedSeconds(result.timeline_repair.removed_gap_seconds) + "s）");
    if (!result.timeline_repair.note.empty())
        recovery.rows.emplace_back("说明", result.timeline_repair.note);
    sections.push_back(std::move(recovery));

    ReportSection conservation;
    conservation.title = "逐轨样本守恒";
    if (result.track_conservation.empty()) {
        conservation.items.push_back("本次没有源侧逐轨声明基线（普通 MP4 路径），未做逐轨守恒判定");
    } else {
        for (const auto& record : result.track_conservation) {
            conservation.items.push_back(
                "轨道 " + std::to_string(record.track_id) + " (" + record.kind + ") 输出流#" +
                std::to_string(record.output_stream_index) + ": 声明 " + std::to_string(record.expected_samples) +
                " / 写入 " + std::to_string(record.packets_written) + "，关系=" + record.relation +
                "，判定=" + record.status + (record.note.empty() ? "" : "（" + record.note + "）"));
        }
    }
    sections.push_back(std::move(conservation));

    ReportSection verification;
    verification.title = "验证（" + std::string(model::DisplayName(result.verification.level)) + "）";
    verification.rows.emplace_back("状态", model::ToString(result.verification.status));
    verification.rows.emplace_back("证据", result.verification.evidence);
    verification.rows.emplace_back(
        "解码范围", result.verification.decode_scope.empty() ? "（未记录）" : result.verification.decode_scope);
    verification.rows.emplace_back("结构验证专用", result.verification.structure_only ? "yes（尚未验证播放）" : "no");
    verification.rows.emplace_back("帧统计", "解码 " + std::to_string(result.verification.decoded_frames) + " / 有效 " +
                                                 std::to_string(result.verification.valid_frames) + " / 损坏 " +
                                                 std::to_string(result.verification.corrupt_frames) + " / 错误恢复 " +
                                                 std::to_string(result.verification.error_recovery_frames) +
                                                 " / API 错误 " + std::to_string(result.verification.decode_errors));
    // R05：把「帧标记恒 0」显式写成能力证据，避免把 0 读成「画面干净」。
    const std::string marker_note =
        result.verification.frame_marker_evidence == "observed"
            ? "解码器确实给出帧级损坏标记"
            : (result.verification.frame_marker_evidence == "not_run" ? "未解码，不存在帧级证据"
                                                                      : "未观察到帧级标记，不能据此断言画面干净");
    verification.rows.emplace_back("帧标记证据", result.verification.frame_marker_evidence + "（带标记帧 " +
                                                     std::to_string(result.verification.marked_frames) + "；" +
                                                     marker_note + "）");
    for (const auto& limitation : result.verification.frame_marker_limitations)
        verification.items.push_back("帧标记限制: " + limitation);
    verification.rows.emplace_back("跳转", std::to_string(result.verification.seek_passes) + "/" +
                                               std::to_string(result.verification.seek_checks));
    verification.rows.emplace_back("时间轴实测",
                                   "源侧估算=" + FixedSeconds(result.verification.expected_duration_seconds) +
                                       "s / 产物实测=" + FixedSeconds(result.verification.observed_duration_seconds) +
                                       "s / 差值=" + FixedSeconds(result.verification.duration_delta_seconds) +
                                       "s / 可比对=" + (result.verification.timeline_comparable ? "yes" : "no") +
                                       " / 偏差超阈值=" + (result.verification.timeline_mismatch ? "yes" : "no"));
    verification.rows.emplace_back("音画同步",
                                   "状态=" + result.verification.av_sync_status +
                                       " / 首端偏移=" + FixedSeconds(result.verification.av_sync_start_delta_seconds) +
                                       "s / 尾端偏移=" + FixedSeconds(result.verification.av_sync_end_delta_seconds) +
                                       "s / 漂移=" + FixedSeconds(result.verification.av_sync_drift_seconds) + "s");
    for (const auto& track : result.verification.per_track)
        verification.items.push_back("逐轨 " + track.kind + "#" + std::to_string(track.output_stream_index) + " [" +
                                     model::ToString(track.status) + "] verified=" + (track.verified ? "yes" : "no") +
                                     " frames=" + std::to_string(track.frames) +
                                     " corrupt_packets=" + std::to_string(track.corrupt_packets) +
                                     " corrupt=" + std::to_string(track.corrupt_frames) + " — " + track.detail);
    for (const auto& check : result.verification.checks)
        verification.items.push_back("[" + std::string(model::ToString(check.status)) + "] " + check.name + " — " +
                                     check.detail);
    for (const auto& line : result.verification.timeline_checks)
        verification.items.push_back("时间轴实测: " + line);
    sections.push_back(std::move(verification));

    ReportSection residual;
    residual.title = "修复 / 验证后的残留问题";
    if (result.residual_findings.empty()) {
        residual.items.push_back("本次未记录到残留问题（注意：这不等于已证明无失真）");
    } else {
        for (const auto& finding : result.residual_findings)
            residual.items.push_back(FindingLine(finding));
    }
    sections.push_back(std::move(residual));

    ReportSection fidelity;
    fidelity.title = "保真度与已知限制";
    fidelity.rows.emplace_back("视频", model::ToString(result.fidelity.video));
    fidelity.rows.emplace_back("音频", model::ToString(result.fidelity.audio));
    fidelity.rows.emplace_back("内容完整性", result.fidelity.content_completeness);
    fidelity.rows.emplace_back("感知保真度", result.fidelity.perceptual_fidelity);
    // R03：三态展示。旧实现只看引擎是否主动重写时间戳，借用未经核验的参考时基时
    // 会写「时间轴改写: no」，把「无法判定」冒充成「已证明没变」。
    const std::string timeline_display = result.fidelity.timeline_status == "unchanged"
                                             ? "no（时基已核验且时长一致）"
                                             : (result.fidelity.timeline_status == "modified"
                                                    ? "yes（实测时长不符，播放速度可能已变）"
                                                    : (result.fidelity.timeline_status == "adjusted"
                                                           ? "yes（主动重写时间戳 / 合并缺口，片段内部速度未变）"
                                                           : "unknown（不能断言未改写）"));
    fidelity.rows.emplace_back("时间轴改写", timeline_display);
    fidelity.rows.emplace_back("时间轴结论", result.fidelity.timeline_status);
    fidelity.rows.emplace_back("时间轴说明", result.fidelity.timeline_note);
    fidelity.rows.emplace_back("参考段兼容性", result.fidelity.reference_compatibility);
    for (const auto& borrowed : result.fidelity.borrowed_parameters)
        fidelity.items.push_back("借用参数: " + borrowed);
    for (const auto& omitted : result.fidelity.omitted_items)
        fidelity.items.push_back("未保留: " + omitted);
    for (const auto& limitation : result.fidelity.limitations)
        fidelity.items.push_back("限制 / 未知项: " + limitation);
    // 切片 J：S08 的源侧属性结论与 JSON 的 fidelity.transcode 同源。非转码路径
    // 也必须明确写「未探测」，不能靠空表让读者以为没有损失。
    const model::RepairTranscodeFidelity& transcode = result.fidelity.transcode;
    fidelity.rows.emplace_back("转码保真探测", transcode.inspected ? "已实测（首帧 + 输入流）" : "未探测");
    // 切片 O：编码器行必须独立于「是否探测到首帧」—— 编码器打开过就是事实，
    // 而且这一行正是用来纠正旧版把 libx264 当既成事实的地方。
    if (!transcode.encoder_note.empty())
        fidelity.rows.emplace_back("重编码编码器", transcode.encoder_note);
    if (!transcode.profile_note.empty())
        fidelity.rows.emplace_back("重编码 profile", transcode.profile_note);
    if (transcode.av_start_offset_seconds.has_value()) {
        const double av_offset = *transcode.av_start_offset_seconds;
        fidelity.rows.emplace_back("音画起点差",
                                   FixedSeconds(av_offset) + " s（视频" + (av_offset >= 0.0 ? "较晚" : "较早") + "）");
    }
    if (!transcode.av_sync_note.empty())
        fidelity.rows.emplace_back("音画同步处理", transcode.av_sync_note);
    // 切片 T：轨级决策。汇总一行、逐轨一行一条 item，另加覆盖自证 ——
    // 「决策表 3 条 / 源侧流 4 条」必须显式写出来，不能让少掉的那条看起来像没事。
    if (!transcode.track_decision_summary.empty()) {
        fidelity.rows.emplace_back("轨道决策", transcode.track_decision_summary);
        if (transcode.track_decision_input_streams > 0 &&
            static_cast<int>(transcode.track_decisions.size()) != transcode.track_decision_input_streams) {
            fidelity.rows.emplace_back("轨道决策覆盖", "决策 " + std::to_string(transcode.track_decisions.size()) +
                                                           " 条 / 源侧 " +
                                                           std::to_string(transcode.track_decision_input_streams) +
                                                           " 条流 —— 不一致，未列出的轨不能当成已保留");
        }
        // drop 的轨已经在上面「未保留: …」里逐条列过（含同一条理由），这里不再重复；
        // 汇总数仍算在「轨道决策」行里，读者不会以为漏了什么。
        for (const auto& decision : transcode.track_decisions) {
            if (decision.action == "drop")
                continue;
            fidelity.items.push_back("轨道决策: " + decision.text);
        }
        if (!transcode.track_decision_note.empty())
            fidelity.items.push_back("轨道决策依据: " + transcode.track_decision_note);
    }
    if (!transcode.inspected) {
        fidelity.rows.emplace_back("源侧属性结论", "未探测，不能声称位深 / 色彩 / 旋转 / HDR 均已保留");
    } else {
        fidelity.rows.emplace_back("源像素格式 / 位深", transcode.source_pixel_format + " / " +
                                                            std::to_string(transcode.source_bit_depth) + " bit");
        fidelity.rows.emplace_back("输出像素格式 / 位深", transcode.output_pixel_format + " / " +
                                                              std::to_string(transcode.output_bit_depth) + " bit");
        fidelity.rows.emplace_back("源首帧分辨率", std::to_string(transcode.source_width) + "x" +
                                                       std::to_string(transcode.source_height));
        std::string tags;
        const auto append_tag = [&tags](const char* label, const std::string& value) {
            if (value.empty())
                return;
            if (!tags.empty())
                tags += "/";
            tags += label;
            tags += "=";
            tags += value;
        };
        append_tag("primaries", transcode.color_primaries);
        append_tag("transfer", transcode.color_transfer);
        append_tag("space", transcode.color_space);
        append_tag("range", transcode.color_range);
        append_tag("chroma_loc", transcode.chroma_location);
        fidelity.rows.emplace_back("色彩标记", tags.empty() ? "源未声明" : tags);
        fidelity.rows.emplace_back("像素宽高比",
                                   transcode.sample_aspect_ratio.empty() ? "未声明" : transcode.sample_aspect_ratio);
        // 方向 / HDR 两行的文案由修复层渲染（源侧事实 + 产物实测同源），
        // 报告层只决定「这行印不印」，不自己拼「已继承 / 未继承」。
        fidelity.rows.emplace_back("旋转 / 镜像矩阵",
                                   transcode.display_matrix_note.empty() ? "源未声明" : transcode.display_matrix_note);
        fidelity.rows.emplace_back("HDR 静态元数据",
                                   transcode.hdr_metadata_note.empty() ? "源未声明" : transcode.hdr_metadata_note);
        fidelity.rows.emplace_back("动态分辨率帧", std::to_string(transcode.resolution_changed_frames));
        std::string verdict;
        if (transcode.all_properties_preserved) {
            verdict = "实测可继承属性全部保留（重编码代际损失另计）";
        } else {
            verdict = "实测 " + std::to_string(transcode.lost_properties.size()) + " 项未保留";
            if (!transcode.unverified_properties.empty())
                verdict += "，" + std::to_string(transcode.unverified_properties.size()) + " 项未实测";
        }
        fidelity.rows.emplace_back("源侧属性结论", verdict);
        for (const auto& kept : transcode.preserved_properties)
            fidelity.items.push_back("转码属性已继承: " + kept);
        for (const auto& lost : transcode.lost_properties)
            fidelity.items.push_back("转码属性未保留: " + lost);
        for (const auto& unverified : transcode.unverified_properties)
            fidelity.items.push_back("转码属性未实测: " + unverified);
    }
    sections.push_back(std::move(fidelity));

    return sections;
}

std::string BuildTextReport(const model::RepairResult& result) {
    std::ostringstream out;
    out << "VideoEye 视频修复报告\n";
    out << "=====================\n";
    out << "结果 ID: " << result.result_id << "\n";
    out << "请求 ID: " << result.request_id << "\n";
    out << "生成时间: " << result.created_at_iso8601 << "\n";
    out << "FFmpeg: " << result.ffmpeg_version << "\n";
    for (const auto& section : BuildSections(result)) {
        out << "\n" << section.title << "\n";
        out << std::string(section.title.size(), '-') << "\n";
        for (const auto& row : section.rows)
            out << row.first << ": " << row.second << "\n";
        for (const auto& item : section.items)
            out << "- " << item << "\n";
    }
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
         << "th{width:180px;color:#57606a;font-weight:500}"
         << ".ok{color:#1a7f37}.fail{color:#cf222e}.warn{color:#9a6700}.muted{color:#57606a}"
         << "li{font-size:13px;margin:4px 0}"
         << "code{background:#f0f2f4;border-radius:4px;padding:1px 4px}"
         << "</style></head><body>";
    html << "<div class=\"card\"><h1>VideoEye 视频修复报告</h1>"
         << "<p class=\"muted\">" << EscapeHtml(result.result_id) << " · " << EscapeHtml(result.created_at_iso8601)
         << " · " << EscapeHtml(result.ffmpeg_version) << "</p></div>";
    for (const auto& section : BuildSections(result)) {
        html << "<div class=\"card\"><h2>" << EscapeHtml(section.title) << "</h2>";
        if (!section.rows.empty()) {
            html << "<table>";
            for (const auto& row : section.rows)
                html << "<tr><th>" << EscapeHtml(row.first) << "</th><td>" << EscapeHtml(row.second) << "</td></tr>";
            html << "</table>";
        }
        if (!section.items.empty()) {
            html << "<ul>";
            for (const auto& item : section.items)
                html << "<li>" << EscapeHtml(item) << "</li>";
            html << "</ul>";
        }
        html << "</div>";
    }
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
