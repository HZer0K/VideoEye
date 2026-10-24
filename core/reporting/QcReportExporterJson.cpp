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

JsonValue JNumber(double value) {
    return JsonValue(value);
}

JsonValue JText(const std::string& text) {
    return JsonValue(text);
}

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
    const char* type =
        stream.attached_picture ? "cover" : (stream.IsVideo() ? "video" : (stream.IsAudio() ? "audio" : "other"));
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
                   case model::QcRuleOp::MaxExceeded:
                       return "max_exceeded";
                   case model::QcRuleOp::MinBelow:
                       return "min_below";
                   case model::QcRuleOp::NonZero:
                       return "non_zero";
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

// 证据与覆盖状态表（阶段 5）。每个维度一行，报告读者据此判断
// "结论覆盖了多少内容 / 没跑是为啥"。
//
// 为什么必须整表落盘而不是只落"没通过的"：这表的用途是**证明没漏**。
// 只落异常项的话，"这张表里没有 XX 维度"与"XX 维度跑了且没问题"无法区分。
JsonValue BuildDimensionsArray(const model::AnalysisResult& result) {
    JsonValue array = JsonValue::MakeArray();
    for (const model::DimensionStatus& status : result.dimensions) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("dimension", JText(model::DimensionCode(status.dimension)));
        node.Set("name", JText(model::DimensionName(status.dimension)));
        node.Set("executed", JsonValue(status.executed));
        node.Set("capability", JText(model::CapabilityToString(status.capability)));
        // conclusive 是"本结论可当通过用"的唯一权威口径，下游不该自己用
        // capability 组合推断。
        node.Set("conclusive", JsonValue(status.conclusive));
        if (!status.scope_unit.empty())
            node.Set("scope_unit", JText(status.scope_unit));
        node.Set("units_scanned", JNumber(static_cast<double>(status.units_scanned)));
        node.Set("units_total", JNumber(static_cast<double>(status.units_total)));
        // has_coverage=false 时 coverage 字段必须缺省：落 0 会被读成"覆盖率为 0"，
        // 而实际语义是"总量未知"。
        if (status.has_coverage)
            node.Set("coverage_ratio", JNumber(status.coverage_ratio));
        node.Set("failures", JNumber(static_cast<double>(status.failures)));
        if (!status.failure_reason.empty())
            node.Set("failure_reason", JText(status.failure_reason));
        JsonValue streams = JsonValue::MakeArray();
        for (int index : status.stream_indices)
            streams.PushBack(JNumber(static_cast<double>(index)));
        node.Set("stream_indices", streams);
        JsonValue notes = JsonValue::MakeArray();
        for (const std::string& note : status.notes)
            notes.PushBack(JText(note));
        node.Set("notes", notes);
        node.Set("summary", JText(status.Summary()));
        array.PushBack(node);
    }
    return array;
}

// 参数集时间线（阶段 1）：把"首次配置"升级成"全程配置"。
JsonValue BuildParameterTimelinesArray(const model::AnalysisResult& result) {
    JsonValue array = JsonValue::MakeArray();
    for (const model::ParameterTimeline& timeline : result.parameter_timelines) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("stream_index", JNumber(static_cast<double>(timeline.stream_index)));
        node.Set("codec", JText(timeline.codec_name));
        node.Set("analyzed", JsonValue(timeline.analyzed));
        node.Set("packets_scanned", JNumber(static_cast<double>(timeline.packets_scanned)));
        node.Set("parameter_set_units", JNumber(static_cast<double>(timeline.parameter_set_units)));
        // 引用从未传输过的参数集 = 解码必然失败的硬证据，单独一栏顶到最前
        node.Set("missing_reference_count", JNumber(static_cast<double>(timeline.missing_reference_count)));
        node.Set("truncated", JsonValue(timeline.truncated));
        node.Set("summary", JText(timeline.Summary()));
        node.Set("active_config", JText(timeline.ActiveSummary()));

        JsonValue sets = JsonValue::MakeArray();
        for (const model::ParameterSetEntry& entry : timeline.parameter_sets) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("kind", JText(model::ParameterSetKindCode(entry.kind)));
            item.Set("id", JNumber(static_cast<double>(entry.id)));
            item.Set("digest", JText(entry.digest));
            if (!entry.summary.empty())
                item.Set("config", JText(entry.summary));
            item.Set("occurrences", JNumber(static_cast<double>(entry.occurrences)));
            // replacements > 0 = 同 id 被换过内容，这是最危险的一类变化
            item.Set("replacements", JNumber(static_cast<double>(entry.replacements)));
            item.Set("first_packet_index", JNumber(static_cast<double>(entry.first_packet_index)));
            item.Set("first_file_offset", JNumber(static_cast<double>(entry.first_file_offset)));
            if (entry.has_time)
                item.Set("first_time_seconds", JNumber(entry.first_time_seconds));
            sets.PushBack(item);
        }
        node.Set("parameter_sets", sets);

        JsonValue changes = JsonValue::MakeArray();
        for (const model::ParameterChangeEvent& event : timeline.changes) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("kind", JText(model::ParameterSetKindCode(event.kind)));
            item.Set("parameter_set_id", JNumber(static_cast<double>(event.parameter_set_id)));
            item.Set("source", JText(event.source));
            item.Set("replaced_same_id", JsonValue(event.replaced_same_id));
            if (event.has_time)
                item.Set("time_seconds", JNumber(event.time_seconds));
            item.Set("packet_index", JNumber(static_cast<double>(event.packet_index)));
            // 文件偏移让读者能直接跳到现场（-1 = 容器给不出）
            item.Set("file_offset", JNumber(static_cast<double>(event.file_offset)));
            if (!event.old_config.empty())
                item.Set("old_config", JText(event.old_config));
            if (!event.new_config.empty())
                item.Set("new_config", JText(event.new_config));
            if (!event.changes.empty())
                item.Set("changes", JText(event.changes));
            item.Set("text", JText(event.ToString()));
            changes.PushBack(item);
        }
        node.Set("changes", changes);

        JsonValue refs = JsonValue::MakeArray();
        for (const model::ParameterReferenceStat& stat : timeline.references) {
            JsonValue item = JsonValue::MakeObject();
            item.Set("kind", JText(model::ParameterSetKindCode(stat.kind)));
            item.Set("id", JNumber(static_cast<double>(stat.id)));
            item.Set("frame_count", JNumber(static_cast<double>(stat.frame_count)));
            item.Set("slice_count", JNumber(static_cast<double>(stat.slice_count)));
            item.Set("transmitted", JsonValue(stat.transmitted));
            item.Set("referenced_before_transmit", JsonValue(stat.referenced_before_transmit));
            item.Set("first_reference_packet", JNumber(static_cast<double>(stat.first_reference_packet)));
            if (stat.has_time)
                item.Set("first_reference_seconds", JNumber(stat.first_reference_time));
            refs.PushBack(item);
        }
        node.Set("references", refs);

        JsonValue notes = JsonValue::MakeArray();
        for (const std::string& note : timeline.notes)
            notes.PushBack(JText(note));
        node.Set("notes", notes);
        array.PushBack(node);
    }
    return array;
}

// 帧级显示顺序（POC 重建，仅 H.264）。
//
// 与 parameter_timelines 的分工：那条记"配置怎么变"，这条记"帧怎么排"。
// 字段全部来自逐 slice 解析出的 POC；解析不出来的组合（pic_order_cnt_type=1、
// 非 H.264）走 available=false + reason，不给推断不出的字段留 0 占位。
JsonValue BuildFrameOrdersArray(const model::AnalysisResult& result) {
    JsonValue array = JsonValue::MakeArray();
    for (const model::FrameOrderTimeline& timeline : result.frame_order_timelines) {
        JsonValue node = JsonValue::MakeObject();
        node.Set("stream_index", JNumber(static_cast<double>(timeline.stream_index)));
        node.Set("available", JsonValue(timeline.available));
        if (!timeline.available) {
            node.Set("reason", JText(timeline.unavailable_reason));
            array.PushBack(node);
            continue;
        }
        node.Set("poc_type", JNumber(static_cast<double>(timeline.poc_type)));
        node.Set("field_coded", JsonValue(timeline.field_coded));
        node.Set("truncated", JsonValue(timeline.truncated));
        node.Set("frames_observed", JNumber(static_cast<double>(timeline.frames_observed)));
        node.Set("i_count", JNumber(static_cast<double>(timeline.i_count)));
        node.Set("p_count", JNumber(static_cast<double>(timeline.p_count)));
        node.Set("b_count", JNumber(static_cast<double>(timeline.b_count)));
        node.Set("idr_count", JNumber(static_cast<double>(timeline.idr_count)));
        // 显示顺序重排：解码顺序里 POC 回退的次数与最大幅度（POC 步长通常为 2，
        // 所以"回退 4"≈ 重排 2 帧）。这是 B 帧存在的直接证据，不是靠 slice_type 猜。
        node.Set("reorder_events", JNumber(static_cast<double>(timeline.reorder_events)));
        node.Set("reorder_depth", JNumber(static_cast<double>(timeline.reorder_depth)));
        node.Set("duplicate_poc_count", JNumber(static_cast<double>(timeline.duplicate_poc_count)));
        node.Set("poc_gap_slots", JNumber(static_cast<double>(timeline.poc_gap_slots)));
        node.Set("gop_count", JNumber(static_cast<double>(timeline.gop_count)));
        node.Set("min_gop_frames", JNumber(static_cast<double>(timeline.min_gop_frames)));
        node.Set("max_gop_frames", JNumber(static_cast<double>(timeline.max_gop_frames)));
        node.Set("avg_gop_frames", JNumber(timeline.avg_gop_frames));
        node.Set("summary", JText(timeline.Summary()));

        // 支持范围：把"哪些结论能下、哪些不能"写进报告，避免读者把缺失当通过。
        JsonValue scope = JsonValue::MakeObject();
        scope.Set("codecs", JText("仅 H.264；HEVC/AV1/VP9/VVC 的 POC 未实现（对应维度标记为不可用）"));
        scope.Set("poc_type", JText("pic_order_cnt_type 0 与 2 支持；1 不支持（需要 offset_for_ref_frame 表）"));
        scope.Set("reference_lists", JText("不重建 DPB / 参考帧列表（无 MMCO、无 RPL），只给 POC 序列"));
        scope.Set("gap_inference", JText("缺帧数按观测到的最小 POC 步长推断，只给总量不给具体帧号；"
                                         "隔行（field coding）时不推断"));
        node.Set("scope", scope);

        JsonValue frames = JsonValue::MakeArray();
        for (const model::FrameOrderEntry& entry : timeline.frames) {
            JsonValue item = JsonValue::MakeObject();
            // 解码顺序下标：等于它在 frames 数组里的位置，但显式写出来，
            // 免得读者拿"第几个"去对 POC 时对不上（数组下标与显示顺序无关）。
            item.Set("decode_index", JNumber(static_cast<double>(frames.Size())));
            item.Set("type", JText(std::string(1, entry.TypeChar())));
            item.Set("idr", JsonValue(entry.idr));
            item.Set("frame_num", JNumber(static_cast<double>(entry.frame_num)));
            item.Set("poc", JNumber(static_cast<double>(entry.poc)));
            item.Set("packet_index", JNumber(static_cast<double>(entry.packet_index)));
            if (entry.has_time)
                item.Set("time_seconds", JNumber(entry.time_seconds));
            frames.PushBack(item);
        }
        node.Set("frames", frames);
        array.PushBack(node);
    }
    return array;
}

// 逐轨音频 QC（阶段 4）。原来只有主轨一份 loudness，多语言/评论轨完全不可见。
JsonValue BuildAudioQcStreamsArray(const model::AnalysisResult& result) {
    JsonValue array = JsonValue::MakeArray();
    for (size_t i = 0; i < result.audio_qc_streams.size(); ++i) {
        const model::AudioQcResult& qc = result.audio_qc_streams[i];
        JsonValue node = JsonValue::MakeObject();
        // 流号要从并行的索引表取：AudioQcResult 自己不记 stream_index
        const int stream_index = (i < result.audio_qc_stream_indices.size()) ? result.audio_qc_stream_indices[i] : -1;
        node.Set("stream_index", JNumber(static_cast<double>(stream_index)));
        node.Set("analyzed", JsonValue(qc.analyzed));
        node.Set("integrated_lufs", JNumber(qc.integrated_lufs));
        node.Set("true_peak_dbtp", JNumber(qc.true_peak_dbtp));
        node.Set("loudness_range_lu", JNumber(qc.loudness_range_lu));
        node.Set("sample_peak_dbfs", JNumber(qc.sample_peak_dbfs));
        node.Set("rms_dbfs", JNumber(qc.rms_dbfs));
        node.Set("silence_ratio", JNumber(qc.silence_ratio));
        node.Set("longest_silence_seconds", JNumber(qc.longest_silence_seconds));
        node.Set("channel_layout", JText(qc.metadata.channel_layout));
        node.Set("channels", JNumber(static_cast<double>(qc.metadata.channels)));
        node.Set("sample_rate", JNumber(static_cast<double>(qc.metadata.sample_rate)));
        // 逐轨覆盖率是这块的核心价值：总轨数对上了不代表每条轨都跑满了
        node.Set("coverage_ratio", JNumber(qc.coverage_ratio));
        node.Set("covered_seconds", JNumber(qc.covered_seconds));
        node.Set("correlation_available", JsonValue(qc.correlation_available));
        if (qc.correlation_available) {
            node.Set("correlation_mean", JNumber(qc.correlation_mean));
            node.Set("correlation_min", JNumber(qc.correlation_min));
            node.Set("out_of_phase_ratio", JNumber(qc.out_of_phase_ratio));
        }
        node.Set("decode_error_packets", JNumber(static_cast<double>(qc.decode_error_packets)));
        node.Set("decode_error_frames", JNumber(static_cast<double>(qc.decode_error_frames)));
        node.Set("dropped_frames", JNumber(static_cast<double>(qc.dropped_frames)));
        JsonValue notes = JsonValue::MakeArray();
        for (const std::string& note : qc.notes)
            notes.PushBack(JText(note));
        node.Set("notes", notes);
        array.PushBack(node);
    }
    return array;
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
        loudness.Set("clipping_sample_count", JNumber(static_cast<double>(result.audio_qc.clipping_sample_count)));
        loudness.Set("channel_layout", JText(result.audio_qc.metadata.channel_layout));
        // 完整性：只报 analyzed 的话，"只覆盖前 30 秒"和"完整跑完"在 JSON 里长得一样。
        const char* completeness = "not_run";
        switch (result.audio_qc.completeness) {
        case model::AudioQcResult::Completeness::Complete:
            completeness = "complete";
            break;
        case model::AudioQcResult::Completeness::Partial:
            completeness = "partial";
            break;
        case model::AudioQcResult::Completeness::Unsupported:
            completeness = "unsupported";
            break;
        case model::AudioQcResult::Completeness::Failed:
            completeness = "failed";
            break;
        case model::AudioQcResult::Completeness::NotRun:
            completeness = "not_run";
            break;
        }
        loudness.Set("completeness", JText(completeness));
        loudness.Set("coverage_ratio", JNumber(result.audio_qc.coverage_ratio));
        loudness.Set("covered_seconds", JNumber(result.audio_qc.covered_seconds));
        loudness.Set("decode_error_packets", JNumber(static_cast<double>(result.audio_qc.decode_error_packets)));
        loudness.Set("decode_error_frames", JNumber(static_cast<double>(result.audio_qc.decode_error_frames)));
        loudness.Set("dropped_frames", JNumber(static_cast<double>(result.audio_qc.dropped_frames)));
        metrics.Set("loudness", loudness);
    }

    // 音频位流参数（D1）：与 loudness 并列 —— 一个来自解码后的 PCM，一个来自帧头。
    // 审核要对照的正是"声明 vs 实测"，分开放反而更好比对。
    if (result.audio_bitstream_analyzed) {
        JsonValue streams = JsonValue::MakeArray();
        for (const auto& stream : result.audio_bitstream.streams) {
            JsonValue node = JsonValue::MakeObject();
            node.Set("stream_index", JNumber(static_cast<double>(stream.stream_index)));
            node.Set("codec", JText(model::AudioBitstreamCodecCode(stream.codec)));
            node.Set("profile", JText(stream.profile));
            node.Set("sample_rate", JNumber(static_cast<double>(stream.sample_rate)));
            node.Set("channels", JNumber(static_cast<double>(stream.channels)));
            node.Set("container_sample_rate", JNumber(static_cast<double>(stream.container_sample_rate)));
            node.Set("container_channels", JNumber(static_cast<double>(stream.container_channels)));
            node.Set("bit_rate_kbps", JNumber(static_cast<double>(stream.bit_rate_kbps)));
            node.Set("frame_count", JNumber(static_cast<double>(stream.frame_count)));
            node.Set("parsed_packets", JNumber(static_cast<double>(stream.parsed_packets)));
            node.Set("invalid_packets", JNumber(static_cast<double>(stream.invalid_packets)));
            // 未实现逐帧解析的编码（MP3/FLAC/PCM…）：单独一栏，别和 invalid 混在一起
            node.Set("unsupported_packets", JNumber(static_cast<double>(stream.unsupported_packets)));
            node.Set("has_dialnorm", JsonValue(stream.has_dialnorm));
            node.Set("dialnorm_dbfs", JNumber(static_cast<double>(stream.dialnorm_dbfs)));
            if (stream.dialnorm_delta_available) {
                node.Set("dialnorm_delta_lu", JNumber(stream.dialnorm_delta_lu));
            }
            node.Set("atmos_joc", JsonValue(stream.atmos_joc));
            if (stream.atmos_joc) {
                node.Set("atmos_source", JText(stream.atmos_source));
                // "heuristic" = 只是线索，调用方不得当确定结论用
                node.Set("atmos_confidence", JText(stream.atmos_heuristic ? "heuristic" : "parsed"));
            }
            node.Set("vbr", JsonValue(stream.vbr));
            node.Set("params_changed_midstream", JsonValue(stream.params_changed_midstream));
            node.Set("container_mismatch", JsonValue(stream.container_mismatch));
            // ---- 帧头结构深化（阶段 D2）：MP3 / FLAC / AAC PCE ----
            //
            // 这些字段只在对应编码下有意义，无条件落会造成"MP3 的 flac_streaminfo=false"
            // 这种把"不适用"读成"缺失"的假象，所以按 codec 分块。
            if (stream.codec == model::AudioBitstreamCodec::Mp3) {
                JsonValue mp3 = JsonValue::MakeObject();
                mp3.Set("mpeg_version", JNumber(static_cast<double>(stream.mpeg_version)));
                mp3.Set("layer", JNumber(static_cast<double>(stream.mp3_layer)));
                mp3.Set("channel_mode", JNumber(static_cast<double>(stream.channel_mode)));
                // 帧头保护位只报"存在"，CRC-16 覆盖范围有歧义故不校验（见 parser 注释）
                mp3.Set("crc_present", JsonValue(stream.crc_present));
                // free_format 时帧长不可定界，后续所有逐帧统计都不可信 —— 必须显式告知
                mp3.Set("free_format", JsonValue(stream.free_format));
                if (!stream.vbr_tag.empty()) {
                    mp3.Set("vbr_tag", JText(stream.vbr_tag));
                    // 声明值 vs 实测值分开落，对不上才是证据
                    mp3.Set("declared_frame_count", JNumber(static_cast<double>(stream.declared_frame_count)));
                    mp3.Set("measured_frame_count", JNumber(static_cast<double>(stream.frame_count)));
                    mp3.Set("declared_stream_bytes", JNumber(static_cast<double>(stream.declared_stream_bytes)));
                }
                if (stream.has_encoder_delay) {
                    mp3.Set("encoder_delay_samples", JNumber(static_cast<double>(stream.encoder_delay_samples)));
                    mp3.Set("encoder_padding_samples", JNumber(static_cast<double>(stream.encoder_padding_samples)));
                }
                if (!stream.encoder.empty())
                    mp3.Set("encoder", JText(stream.encoder));
                node.Set("mp3", mp3);
            }
            if (stream.codec == model::AudioBitstreamCodec::Flac) {
                JsonValue flac = JsonValue::MakeObject();
                flac.Set("streaminfo_present", JsonValue(stream.flac_streaminfo));
                flac.Set("min_block_size", JNumber(static_cast<double>(stream.streaminfo_min_block)));
                flac.Set("max_block_size", JNumber(static_cast<double>(stream.streaminfo_max_block)));
                flac.Set("declared_total_samples", JNumber(static_cast<double>(stream.declared_total_samples)));
                // STREAMINFO 声明了总样本数才有比对基准；0 = 未知，不是"0 个样本"
                flac.Set("total_samples_known", JsonValue(stream.declared_total_samples > 0));
                flac.Set("frame_crc_checked", JsonValue(stream.flac_frame_crc_checked));
                flac.Set("frame_crc_failures", JNumber(static_cast<double>(stream.flac_frame_crc_failures)));
                flac.Set("high_variance_block_sizes", JsonValue(stream.flac_high_variance_blocks));
                node.Set("flac", flac);
            }
            // AAC 的 PCE：有没有解出 PCE 决定"声道数"这一栏是一手证据还是容器抄来的。
            // SBR/PS：sbr_source 为空表示**没有显式信令**，不等于"确认没有 SBR"
            // —— implicit signaling 的 SBR 在 raw_data_block 的 ID_FIL 里，本工具不解析。
            // 报告必须能把这两种情况分开，否则读报告的人会把"未知"当成"没有"。
            if (stream.codec == model::AudioBitstreamCodec::AacAdts ||
                stream.codec == model::AudioBitstreamCodec::AacLatm ||
                stream.codec == model::AudioBitstreamCodec::AacRaw) {
                JsonValue aac = JsonValue::MakeObject();
                aac.Set("channel_source", JText(stream.aac_pce ? "pce" : "asc_or_container"));
                aac.Set("sbr", JsonValue(stream.aac_sbr));
                aac.Set("ps", JsonValue(stream.aac_ps));
                aac.Set("sbr_source",
                        JText(stream.aac_sbr_source.empty() ? std::string("not_signaled") : stream.aac_sbr_source));
                if (stream.aac_core_sample_rate > 0)
                    aac.Set("core_sample_rate", JNumber(static_cast<double>(stream.aac_core_sample_rate)));
                if (stream.aac_output_sample_rate > 0)
                    aac.Set("output_sample_rate", JNumber(static_cast<double>(stream.aac_output_sample_rate)));
                if (stream.latm_num_sub_frames >= 0)
                    aac.Set("latm_num_sub_frames", JNumber(static_cast<double>(stream.latm_num_sub_frames)));
                if (stream.aac_pce) {
                    aac.Set("object_type", JNumber(static_cast<double>(stream.pce_object_type)));
                    aac.Set("sample_rate", JNumber(static_cast<double>(stream.pce_sample_rate)));
                    aac.Set("channels", JNumber(static_cast<double>(stream.pce_channels)));
                    aac.Set("front", JNumber(static_cast<double>(stream.pce_front)));
                    aac.Set("side", JNumber(static_cast<double>(stream.pce_side)));
                    aac.Set("back", JNumber(static_cast<double>(stream.pce_back)));
                    aac.Set("lfe", JNumber(static_cast<double>(stream.pce_lfe)));
                    aac.Set("layout", JText(stream.pce_layout));
                }
                node.Set("aac", aac);
            }
            if (!stream.note.empty())
                node.Set("note", JText(stream.note));
            JsonValue inconsistencies = JsonValue::MakeArray();
            for (const std::string& text : stream.inconsistencies) {
                inconsistencies.PushBack(JsonValue(text));
            }
            node.Set("inconsistencies", inconsistencies);
            streams.PushBack(node);
        }
        metrics.Set("audio_bitstream", streams);
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
        color.Set("max_fall", JNumber(static_cast<double>(result.color_hdr.hdr.content_light.max_fall)));
        color.Set("dolby_vision_profile", JNumber(static_cast<double>(result.color_hdr.hdr.dolby_vision.profile)));
        metrics.Set("color_hdr", color);
    }

    if (result.streaming_analyzed) {
        JsonValue streaming = JsonValue::MakeObject();
        streaming.Set("manifest_kind", JText(result.streaming_package.manifest_path.empty() ? "" : "local"));
        streaming.Set("variant_count", JNumber(static_cast<double>(result.streaming_package.variants.size())));
        metrics.Set("streaming", streaming);
    }

    // MPEG-TS 传输层体检的原始数字。
    //
    // 为什么必须整体落进报告（而不是只让规则带几个数）：规则只在**命中**时才有
    // detail，没命中就什么都不留。于是"这条 TS 的连续计数错误到底是 0 还是压根没扫"
    // 在报告里长得一模一样 —— 争议时无法回答"扫了没有"。这里无条件记下扫描范围
    // （扫了多少包 / 共约多少包 / 是否扫完）和各计数，读者自己就能判断
    // container.ts.* 的"未命中"是"没问题"还是"没证据"。
    //
    // 只在 applicable（本地文件 + iformat 是 mpegts）时落，非 TS 不该有这一节。
    // applicable=true 但 analyzed=false 表示"是 TS 但没扫出结论"，同样要落，
    // 并且带 error_message —— 这是最需要被看见的一种情况。
    if (result.ts_transport.applicable) {
        const model::TransportStreamInfo& ts = result.ts_transport;
        JsonValue node = JsonValue::MakeObject();
        node.Set("analyzed", JsonValue(ts.analyzed));
        node.Set("packet_size", JNumber(static_cast<double>(ts.packet_size)));
        node.Set("scanned_packets", JNumber(static_cast<double>(ts.scanned_packets)));
        node.Set("estimated_packets", JNumber(static_cast<double>(ts.estimated_packets)));
        node.Set("scan_complete", JsonValue(ts.scan_complete));
        node.Set("programs", JNumber(static_cast<double>(ts.programs)));
        node.Set("streams", JNumber(static_cast<double>(ts.streams)));
        node.Set("services", JNumber(static_cast<double>(ts.services)));
        node.Set("has_pat", JsonValue(ts.has_pat));
        node.Set("has_pmt", JsonValue(ts.has_pmt));
        node.Set("has_sdt", JsonValue(ts.has_sdt));
        node.Set("sync_loss", JNumber(static_cast<double>(ts.sync_loss)));
        node.Set("continuity_errors", JNumber(static_cast<double>(ts.continuity_errors)));
        node.Set("continuity_error_percent", JNumber(ts.ContinuityErrorPercent()));
        node.Set("scrambled_packets", JNumber(static_cast<double>(ts.scrambled_packets)));
        node.Set("pcr_max_gap_ms", JNumber(ts.pcr_max_gap_ms));
        // 载荷级（PES 内）的数字也要落：container.ts.no_idr / pid_stall 未命中时
        // 报告里只剩这两个数能说明"是真的没断流"还是"根本没扫载荷"。
        node.Set("pes_payload_scanned", JsonValue(ts.pes_payload_scanned));
        node.Set("pes_streams_checked", JNumber(static_cast<double>(ts.pes_streams_checked)));
        node.Set("idr_count", JNumber(static_cast<double>(ts.idr_count)));
        node.Set("max_no_idr_interval_ms", JNumber(ts.max_no_idr_interval_ms));
        node.Set("max_pid_gap_ms", JNumber(ts.max_pid_gap_ms));
        node.Set("video_pids_without_idr", JNumber(static_cast<double>(ts.video_pids_without_idr)));
        node.Set("stalled_pids", JNumber(static_cast<double>(ts.stalled_pids)));
        if (!ts.max_no_idr_pid.empty())
            node.Set("max_no_idr_pid", JText(ts.max_no_idr_pid));
        if (!ts.max_pid_gap_pid.empty())
            node.Set("max_pid_gap_pid", JText(ts.max_pid_gap_pid));
        if (!ts.error_message.empty())
            node.Set("error_message", JText(ts.error_message));
        metrics.Set("ts_transport", node);
    }

    return metrics;
}

} // namespace

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
    if (!run.error.empty())
        summary.Set("error", JText(run.error));
    JsonValue counts = JsonValue::MakeObject();
    counts.Set("critical", JNumber(static_cast<double>(report.CountBySeverity(model::IssueSeverity::Critical))));
    counts.Set("error", JNumber(static_cast<double>(report.CountBySeverity(model::IssueSeverity::Error))));
    counts.Set("warning", JNumber(static_cast<double>(report.CountBySeverity(model::IssueSeverity::Warning))));
    counts.Set("info", JNumber(static_cast<double>(report.CountBySeverity(model::IssueSeverity::Info))));
    summary.Set("issue_counts", counts);
    root.Set("summary", summary);

    JsonValue issues = JsonValue::MakeArray();
    for (const auto& issue : report.issues)
        issues.PushBack(BuildIssueObject(issue));
    root.Set("issues", issues);

    root.Set("metrics", BuildMetricsObject(run.analysis));

    // 证据与覆盖状态（阶段 5）：顶层而不是塞进 metrics —— 它描述的是
    // metrics 里那些数字**有多可信**，与具体测量值不是一个层级。
    root.Set("dimensions", BuildDimensionsArray(run.analysis));

    JsonValue streams = JsonValue::MakeArray();
    for (const auto& stream : run.analysis.streams)
        streams.PushBack(BuildStreamObject(stream));
    root.Set("streams", streams);

    JsonValue rules = JsonValue::MakeArray();
    for (const auto& rule : report.rules)
        rules.PushBack(BuildRuleObject(rule));
    root.Set("rules", rules);

    // 参数变化时间线（阶段 1）：与 streaming 同层级，属于"过程证据"而非测量值。
    // 无条件落（空数组也落）：报告读者要能区分"没有变化"与"没追踪"，
    // 后者由 dimensions 里 parameter_timeline 一行的 executed 说明。
    root.Set("parameter_timelines", BuildParameterTimelinesArray(run.analysis));
    // 帧级显示顺序（POC）。同样无条件落：空数组意味着"没有 H.264 轨"，
    // 与"有轨但没分析出来"由 dimensions 里 frame_order 一行说明。
    root.Set("frame_orders", BuildFrameOrdersArray(run.analysis));
    // 逐轨音频 QC（阶段 4）：主轨的 loudness 已在 metrics.loudness 里，
    // 这里给出全部轨的横向对照（多语言/评论轨）。
    root.Set("audio_qc_streams", BuildAudioQcStreamsArray(run.analysis));

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
                         case qc::QcFieldDiff::Same:
                             return "same";
                         case qc::QcFieldDiff::Different:
                             return "different";
                         case qc::QcFieldDiff::OnlyLeft:
                             return "only_left";
                         case qc::QcFieldDiff::OnlyRight:
                             return "only_right";
                         case qc::QcFieldDiff::Unavailable:
                             return "unavailable";
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

bool QcReportExporter::ExportComparisonJson(const std::string& path, const qc::QcComparison& comparison) {
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
                     case qc::QcFieldDiff::Same:
                         return "same";
                     case qc::QcFieldDiff::Different:
                         return "different";
                     case qc::QcFieldDiff::OnlyLeft:
                         return "only_left";
                     case qc::QcFieldDiff::OnlyRight:
                         return "only_right";
                     case qc::QcFieldDiff::Unavailable:
                         return "unavailable";
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

bool QcReportExporter::ExportBatchSummaryJson(const std::string& path, const QcBatchSummaryInput& input) {
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
        if (!row.output_path.empty())
            item.Set("output_path", JText(row.output_path));
        if (!row.error.empty())
            item.Set("error", JText(row.error));
        rows.PushBack(item);
    }
    root.Set("files", rows);

    return WriteUtf8File(path, root.ToPrettyString(2) + "\n");
}

} // namespace reporting
} // namespace videoeye