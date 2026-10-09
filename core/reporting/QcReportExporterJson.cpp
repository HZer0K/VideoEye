#include "core/reporting/QcReportExporter.h"

#include "core/reporting/QcReportExporterInternal.h"

#include "infrastructure/serialization/Json.h"

namespace videoeye {
namespace reporting {

// JsonValue 还住在历史命名空间 videoeye::utils —— utils/ 目录已经拆进
// infrastructure/，命名空间没跟着改（全仓 200+ 处引用，单独一轮做）。
using videoeye::JsonValue;

using detail::kReportSchemaVersion;
using detail::WriteUtf8File;

namespace {

JsonValue JNumber(double value) { return JsonValue(value); }

JsonValue JText(const std::string& text) { return JsonValue(text); }

JsonValue BuildIssueObject(const model::DiagnosticIssue& issue) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("rule_id", JText(issue.rule_id));
    object.Set("title", JText(issue.title));
    object.Set("severity", JText(SeverityCode(issue.severity)));
    object.Set("severity_text", JText(issue.SeverityText()));
    object.Set("category", JText(CategoryCode(issue.category)));
    object.Set("category_text", JText(issue.CategoryText()));
    object.Set("detail", JText(issue.detail));
    object.Set("suggestion", JText(issue.suggestion));
    object.Set("range", JText(issue.range.ToString()));
    object.Set("stream_index", JNumber(static_cast<double>(issue.stream_index)));
    object.Set("metric_value", JNumber(issue.metric_value));
    object.Set("threshold", JNumber(issue.threshold));
    object.Set("occurrence_count", JNumber(static_cast<double>(issue.occurrence_count)));
    return object;
}

JsonValue BuildStreamObject(const model::StreamDigest& stream) {
    JsonValue object = JsonValue::MakeObject();
    const char* type = stream.IsVideo() ? "video" : (stream.IsAudio() ? "audio" : "other");
    object.Set("index", JNumber(static_cast<double>(stream.index)));
    object.Set("type", JText(type));
    object.Set("codec_name", JText(stream.codec_name));
    object.Set("profile_name", JText(stream.profile_name));
    object.Set("width", JNumber(static_cast<double>(stream.width)));
    object.Set("height", JNumber(static_cast<double>(stream.height)));
    object.Set("fps", JNumber(stream.avg_fps));
    object.Set("sample_rate", JNumber(static_cast<double>(stream.sample_rate)));
    object.Set("channels", JNumber(static_cast<double>(stream.channels)));
    object.Set("bitrate_bps", JNumber(static_cast<double>(stream.bitrate_bps)));
    object.Set("packet_count", JNumber(static_cast<double>(stream.packet_count)));
    object.Set("byte_count", JNumber(static_cast<double>(stream.byte_count)));
    object.Set("start_seconds", JNumber(stream.start_seconds));
    object.Set("duration_seconds", JNumber(stream.duration_seconds));
    return object;
}

JsonValue BuildRuleObject(const model::QcRule& rule) {
    JsonValue object = JsonValue::MakeObject();
    object.Set("id", JText(rule.id));
    object.Set("name", JText(rule.name));
    object.Set("category", JText(CategoryCode(rule.category)));
    object.Set("severity", JText(SeverityCode(rule.severity)));
    object.Set("op", JText(std::string([&rule]() {
        switch (rule.op) {
            case model::QcRuleOp::MaxExceeded: return "max_exceeded";
            case model::QcRuleOp::MinBelow:    return "min_below";
            case model::QcRuleOp::NonZero:     return "non_zero";
        }
        return "unknown";
    }())));
    object.Set("threshold", JNumber(rule.threshold));
    object.Set("unit", JText(rule.unit));
    object.Set("enabled", JsonValue(rule.enabled));
    object.Set("description", JText(rule.description));
    object.Set("suggestion", JText(rule.suggestion));
    return object;
}

JsonValue BuildMetricsObject(const model::AnalysisResult& result) {
    JsonValue metrics = JsonValue::MakeObject();

    if (const model::StreamDigest* video = result.FirstVideoStream()) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("codec_name", JText(video->codec_name));
        node.Set("profile_name", JText(video->profile_name));
        node.Set("width", JNumber(static_cast<double>(video->width)));
        node.Set("height", JNumber(static_cast<double>(video->height)));
        node.Set("fps", JNumber(video->avg_fps));
        node.Set("bitrate_bps", JNumber(static_cast<double>(video->bitrate_bps)));
        node.Set("frame_count", JNumber(static_cast<double>(video->frame_count)));
        node.Set("key_frame_count", JNumber(static_cast<double>(video->key_frame_count)));
        metrics.Set("video", node);
    }
    if (const model::StreamDigest* audio = result.FirstAudioStream()) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("codec_name", JText(audio->codec_name));
        node.Set("sample_rate", JNumber(static_cast<double>(audio->sample_rate)));
        node.Set("channels", JNumber(static_cast<double>(audio->channels)));
        node.Set("bitrate_bps", JNumber(static_cast<double>(audio->bitrate_bps)));
        node.Set("duration_seconds", JNumber(audio->duration_seconds));
        metrics.Set("audio", node);
    }

    JsonValue bitrate = JsonValue::MakeObject();
    bitrate.Set("avg_kbps", JNumber(result.bitrate_gop.avg_bitrate_kbps));
    bitrate.Set("peak_kbps", JNumber(result.bitrate_gop.peak_bitrate_kbps));
    bitrate.Set("min_kbps", JNumber(result.bitrate_gop.min_bitrate_kbps));
    bitrate.Set("p95_kbps", JNumber(result.bitrate_gop.p95_bitrate_kbps));
    bitrate.Set("median_kbps", JNumber(result.bitrate_gop.median_bitrate_kbps));
    bitrate.Set("peak_to_mean_ratio", JNumber(result.bitrate_gop.peak_to_mean_ratio));
    bitrate.Set("window_seconds", JNumber(result.bitrate_gop.window_seconds));
    metrics.Set("bitrate", bitrate);

    JsonValue gop = JsonValue::MakeObject();
    gop.Set("count", JNumber(static_cast<double>(result.bitrate_gop.gops.size())));
    gop.Set("mean_seconds", JNumber(result.bitrate_gop.gop_duration_mean));
    gop.Set("max_seconds", JNumber(result.bitrate_gop.gop_duration_max));
    gop.Set("stddev_seconds", JNumber(result.bitrate_gop.gop_duration_stddev));
    gop.Set("min_frames", JNumber(static_cast<double>(result.bitrate_gop.gop_frames_min)));
    gop.Set("max_frames", JNumber(static_cast<double>(result.bitrate_gop.gop_frames_max)));
    gop.Set("key_interval_mean_seconds", JNumber(result.bitrate_gop.key_interval_mean));
    gop.Set("key_interval_irregularity", JNumber(result.bitrate_gop.key_interval_irregularity));
    gop.Set("long_gop_count", JNumber(static_cast<double>(result.bitrate_gop.long_gop_count)));
    gop.Set("frame_types_known", JsonValue(result.bitrate_gop.frame_types_known));
    gop.Set("i_frame_count", JNumber(static_cast<double>(result.bitrate_gop.i_frame_count)));
    gop.Set("p_frame_count", JNumber(static_cast<double>(result.bitrate_gop.p_frame_count)));
    gop.Set("b_frame_count", JNumber(static_cast<double>(result.bitrate_gop.b_frame_count)));
    metrics.Set("gop", gop);

    if (result.audio_qc.analyzed) {
        JsonValue loudness = JsonValue::MakeObject();
        loudness.Set("integrated_lufs", JNumber(result.audio_qc.integrated_lufs));
        loudness.Set("true_peak_dbtp", JNumber(result.audio_qc.true_peak_dbtp));
        loudness.Set("loudness_range_lu", JNumber(result.audio_qc.loudness_range_lu));
        loudness.Set("short_term_max_lufs", JNumber(result.audio_qc.short_term_max_lufs));
        loudness.Set("momentary_max_lufs", JNumber(result.audio_qc.momentary_max_lufs));
        loudness.Set("sample_peak_dbfs", JNumber(result.audio_qc.sample_peak_dbfs));
        loudness.Set("rms_dbfs", JNumber(result.audio_qc.rms_dbfs));
        loudness.Set("silence_ratio", JNumber(result.audio_qc.silence_ratio));
        loudness.Set("longest_silence_seconds", JNumber(result.audio_qc.longest_silence_seconds));
        loudness.Set("clipping_sample_count",
                     JNumber(static_cast<double>(result.audio_qc.clipping_sample_count)));
        loudness.Set("channel_layout", JText(result.audio_qc.metadata.channel_layout));
        metrics.Set("loudness", loudness);
    }

    if (result.color_hdr.analyzed) {
        JsonValue color = JsonValue::MakeObject();
        const auto& info = result.color_hdr.color;
        color.Set("primaries", JText(info.primaries_name));
        color.Set("transfer", JText(info.transfer_name));
        color.Set("matrix", JText(info.matrix_name));
        color.Set("range", JText(info.range_name));
        color.Set("pixel_format", JText(info.pixel_format.name));
        color.Set("chroma_subsampling", JText(info.pixel_format.chroma_subsampling));
        color.Set("bit_depth", JNumber(static_cast<double>(info.pixel_format.bit_depth)));
        color.Set("level", JNumber(static_cast<double>(info.level)));
        color.Set("hdr_format", JText(result.color_hdr.hdr.format_name));
        color.Set("max_cll", JNumber(static_cast<double>(result.color_hdr.hdr.content_light.max_cll)));
        color.Set("max_fall",
                  JNumber(static_cast<double>(result.color_hdr.hdr.content_light.max_fall)));
        color.Set("dolby_vision_profile",
                  JNumber(static_cast<double>(result.color_hdr.hdr.dolby_vision.profile)));
        metrics.Set("color_hdr", color);
    }

    if (result.streaming_analyzed) {
        JsonValue streaming = JsonValue::MakeObject();
        streaming.Set("manifest_kind", JText(result.streaming_package.manifest_path.empty() ? "" : "local"));
        streaming.Set("variant_count",
                      JNumber(static_cast<double>(result.streaming_package.variants.size())));
        metrics.Set("streaming", streaming);
    }

    return metrics;
}

}  // namespace

// ===========================================================================
// JSON
// ===========================================================================

std::string QcReportExporter::BuildJson(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    JsonValue root = JsonValue::MakeObject();
    root.Set("schema", JText("videoeye.qc-report"));
    root.Set("schema_version", JNumber(static_cast<double>(kReportSchemaVersion)));
    root.Set("generated_at", JText(report.generated_at));

    JsonValue tool = JsonValue::MakeObject();
    tool.Set("name", JText("VideoEye"));
    tool.Set("version", JText("2.0.0"));
    root.Set("tool", tool);

    JsonValue profile = JsonValue::MakeObject();
    profile.Set("id", JText(bundle.profile_id));
    profile.Set("name", JText(bundle.profile_name));
    root.Set("profile", profile);

    JsonValue file = JsonValue::MakeObject();
    file.Set("path", JText(report.file_path));
    file.Set("name", JText(report.file_name));
    file.Set("container_format", JText(report.container_format));
    file.Set("size_bytes", JNumber(static_cast<double>(report.file_size_bytes)));
    file.Set("duration_seconds", JNumber(report.duration_seconds));
    file.Set("overall_bitrate_bps", JNumber(static_cast<double>(report.overall_bitrate_bps)));
    file.Set("video_stream_count", JNumber(static_cast<double>(report.video_stream_count)));
    file.Set("audio_stream_count", JNumber(static_cast<double>(report.audio_stream_count)));
    root.Set("file", file);

    JsonValue summary = JsonValue::MakeObject();
    summary.Set("score", JNumber(report.score));
    summary.Set("verdict", JText(report.verdict));
    summary.Set("completed", JsonValue(report.completed));
    // partial：抽样扫描 / 未覆盖全部帧的结论。下游若当"完整全检"消费会误判，
    // 落库前先看这个标志（此前导出会静默丢掉它，审计 P1）。
    summary.Set("partial", JsonValue(report.partial));
    summary.Set("analysis_elapsed_ms", JNumber(run.elapsed_ms));
    if (!run.error.empty()) summary.Set("error", JText(run.error));
    JsonValue counts = JsonValue::MakeObject();
    counts.Set("critical", JNumber(static_cast<double>(
                               report.CountBySeverity(model::IssueSeverity::Critical))));
    counts.Set("error", JNumber(static_cast<double>(
                            report.CountBySeverity(model::IssueSeverity::Error))));
    counts.Set("warning", JNumber(static_cast<double>(
                              report.CountBySeverity(model::IssueSeverity::Warning))));
    counts.Set("info", JNumber(static_cast<double>(
                           report.CountBySeverity(model::IssueSeverity::Info))));
    summary.Set("issue_counts", counts);
    root.Set("summary", summary);

    JsonValue issues = JsonValue::MakeArray();
    for (const auto& issue : report.issues) issues.PushBack(BuildIssueObject(issue));
    root.Set("issues", issues);

    root.Set("metrics", BuildMetricsObject(run.analysis));

    JsonValue streams = JsonValue::MakeArray();
    for (const auto& stream : run.analysis.streams) streams.PushBack(BuildStreamObject(stream));
    root.Set("streams", streams);

    JsonValue rules = JsonValue::MakeArray();
    for (const auto& rule : report.rules) rules.PushBack(BuildRuleObject(rule));
    root.Set("rules", rules);

    if (bundle.has_comparison) {
        const qc::QcComparison& comparison = bundle.comparison;
        JsonValue node = JsonValue::MakeObject();
        node.Set("left_path", JText(comparison.left_path));
        node.Set("right_path", JText(comparison.right_path));
        node.Set("different_count", JNumber(static_cast<double>(comparison.DifferentCount())));
        JsonValue rows = JsonValue::MakeArray();
        for (const auto& row : comparison.rows) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("group", JText(row.group));
            item.Set("field", JText(row.field));
            item.Set("left", JText(row.left));
            item.Set("right", JText(row.right));
            item.Set("diff", JText(std::string([&row]() {
                switch (row.diff) {
                    case qc::QcFieldDiff::Same:        return "same";
                    case qc::QcFieldDiff::Different:   return "different";
                    case qc::QcFieldDiff::OnlyLeft:    return "only_left";
                    case qc::QcFieldDiff::OnlyRight:   return "only_right";
                    case qc::QcFieldDiff::Unavailable: return "unavailable";
                }
                return "unavailable";
            }())));
            if (row.numeric) {
                item.Set("left_value", JNumber(row.left_value));
                item.Set("right_value", JNumber(row.right_value));
                item.Set("delta", JNumber(row.delta));
            }
            item.Set("unit", JText(row.unit));
            rows.PushBack(item);
        }
        node.Set("rows", rows);
        root.Set("comparison", node);
    }

    return root.ToPrettyString(2) + "\n";
}

bool QcReportExporter::ExportJson(const std::string& path, const QcExportBundle& bundle) {
    return WriteUtf8File(path, BuildJson(bundle));
}

bool QcReportExporter::ExportComparisonJson(const std::string& path,
                                           const qc::QcComparison& comparison) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("schema", JText("videoeye.qc-comparison"));
    root.Set("schema_version", JNumber(static_cast<double>(kReportSchemaVersion)));
    root.Set("generated_at", JText(model::CurrentTimestampString()));
    root.Set("left_path", JText(comparison.left_path));
    root.Set("right_path", JText(comparison.right_path));
    root.Set("left_score", JNumber(comparison.left_score));
    root.Set("right_score", JNumber(comparison.right_score));
    root.Set("left_verdict", JText(comparison.left_verdict));
    root.Set("right_verdict", JText(comparison.right_verdict));
    root.Set("different_count", JNumber(static_cast<double>(comparison.DifferentCount())));
    root.Set("compared_row_count", JNumber(static_cast<double>(comparison.AvailableRowCount())));

    JsonValue rows = JsonValue::MakeArray();
    for (const auto& row : comparison.rows) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("group", JText(row.group));
        item.Set("field", JText(row.field));
        item.Set("left", JText(row.left));
        item.Set("right", JText(row.right));
        item.Set("unit", JText(row.unit));
        item.Set("diff", JText(std::string([&row]() {
            switch (row.diff) {
                case qc::QcFieldDiff::Same:        return "same";
                case qc::QcFieldDiff::Different:   return "different";
                case qc::QcFieldDiff::OnlyLeft:    return "only_left";
                case qc::QcFieldDiff::OnlyRight:   return "only_right";
                case qc::QcFieldDiff::Unavailable: return "unavailable";
            }
            return "unavailable";
        }())));
        if (row.numeric) {
            item.Set("left_value", JNumber(row.left_value));
            item.Set("right_value", JNumber(row.right_value));
            item.Set("delta", JNumber(row.delta));
        }
        rows.PushBack(item);
    }
    root.Set("rows", rows);

    return WriteUtf8File(path, root.ToPrettyString(2) + "\n");
}

bool QcReportExporter::ExportBatchSummaryJson(const std::string& path,
                                             const QcBatchSummaryInput& input) {
    JsonValue root = JsonValue::MakeObject();
    root.Set("schema", JText("videoeye.qc-batch-summary"));
    root.Set("schema_version", JNumber(static_cast<double>(kReportSchemaVersion)));
    root.Set("generated_at", JText(model::CurrentTimestampString()));
    root.Set("root", JText(input.root));

    JsonValue profile = JsonValue::MakeObject();
    profile.Set("id", JText(input.profile_id));
    profile.Set("name", JText(input.profile_name));
    root.Set("profile", profile);

    JsonValue counts = JsonValue::MakeObject();
    counts.Set("total", JNumber(static_cast<double>(input.rows.size())));
    counts.Set("succeeded", JNumber(static_cast<double>(input.succeeded)));
    counts.Set("failed", JNumber(static_cast<double>(input.failed)));
    counts.Set("timed_out", JNumber(static_cast<double>(input.timed_out)));
    counts.Set("cancelled", JNumber(static_cast<double>(input.cancelled)));
    counts.Set("skipped", JNumber(static_cast<double>(input.skipped)));
    counts.Set("critical", JNumber(static_cast<double>(input.critical_count)));
    counts.Set("error", JNumber(static_cast<double>(input.error_count)));
    counts.Set("warning", JNumber(static_cast<double>(input.warning_count)));
    counts.Set("info", JNumber(static_cast<double>(input.info_count)));
    root.Set("counts", counts);
    root.Set("elapsed_ms", JNumber(input.elapsed_ms));
    root.Set("completed", JsonValue(input.completed));

    JsonValue rows = JsonValue::MakeArray();
    for (const auto& row : input.rows) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("path", JText(row.path));
        item.Set("status", JText(row.status));
        item.Set("score", JNumber(row.score));
        item.Set("verdict", JText(row.verdict));
        item.Set("critical_count", JNumber(static_cast<double>(row.critical_count)));
        item.Set("error_count", JNumber(static_cast<double>(row.error_count)));
        item.Set("warning_count", JNumber(static_cast<double>(row.warning_count)));
        item.Set("info_count", JNumber(static_cast<double>(row.info_count)));
        item.Set("elapsed_ms", JNumber(row.elapsed_ms));
        if (!row.output_path.empty()) item.Set("output_path", JText(row.output_path));
        if (!row.error.empty()) item.Set("error", JText(row.error));
        rows.PushBack(item);
    }
    root.Set("files", rows);

    return WriteUtf8File(path, root.ToPrettyString(2) + "\n");
}

}  // namespace reporting
}  // namespace videoeye