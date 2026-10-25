#include "ui/streaming_panel/StreamingPackageJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace videoeye {
namespace ui {

QString BuildStreamingPackageJson(const model::StreamingPackageResult& result) {
    QJsonObject root;
    root["manifest"] = QString::fromStdString(result.manifest_path);
    root["kind"] = QString::fromUtf8(model::ToString(result.kind));
    root["valid"] = result.valid;
    root["live"] = result.IsLive();
    // 能力边界：导出 JSON 会被当成报告引用，抽查范围与"只做一次快照"必须跟着数据走，
    // 否则下游（人工复核 / 自动化比对）会拿抽样结果当全量结论。
    QJsonObject scope;
    scope["snapshot_only"] = true; // 不持续轮询、不重拉清单
    scope["remote_fetch"] = false; // 远端清单与分片不下载
    scope["segments_total"] = static_cast<qint64>(result.TotalSegments());
    scope["segments_probed"] = static_cast<qint64>(result.ProbedSegments());
    scope["segments_probe_complete"] = result.SegmentProbeComplete();
    scope["remote_manifest"] = result.remote;
    scope["output_truncated"] = result.truncated;
    root["check_scope"] = scope;
    if (result.IsDash()) {
        QJsonObject dash_live;
        dash_live["type"] = QString::fromStdString(result.mpd_type);
        dash_live["availability_start_time"] = QString::fromStdString(result.availability_start_time);
        dash_live["publish_time"] = QString::fromStdString(result.publish_time);
        dash_live["minimum_update_period"] = result.minimum_update_period_s;
        dash_live["time_shift_buffer_depth"] = result.time_shift_buffer_depth_s;
        dash_live["suggested_presentation_delay"] = result.suggested_presentation_delay_s;
        root["dash_live"] = dash_live;

        // DRM：加密流的码流类结论不可信，导出时必须带上"用了哪些系统、KID 是什么"，
        // 否则下游只看到一堆"正常"的分片统计。
        QJsonObject drm;
        drm["encrypted"] = result.IsEncrypted();
        QJsonArray cp_list;
        for (const model::DashRepresentationInfo& rep : result.representations) {
            for (const model::DashContentProtectionInfo& cp : rep.content_protections) {
                QJsonObject o;
                o["representation"] = QString::fromStdString(rep.id);
                o["scheme_id_uri"] = QString::fromStdString(cp.scheme_id_uri);
                o["value"] = QString::fromStdString(cp.value);
                o["default_kid"] = QString::fromStdString(cp.default_kid);
                o["has_pssh"] = cp.has_pssh;
                cp_list.append(o);
            }
        }
        drm["content_protection"] = cp_list;
        QJsonArray utc;
        for (const model::DashUtcTimingInfo& t : result.utc_timings) {
            QJsonObject o;
            o["scheme_id_uri"] = QString::fromStdString(t.scheme_id_uri);
            o["value"] = QString::fromStdString(t.value);
            utc.append(o);
        }
        drm["utc_timing"] = utc;
        root["dash_descriptors"] = drm;

        QJsonArray events;
        for (const model::DashEventStreamInfo& es : result.event_streams) {
            QJsonObject o;
            o["scheme_id_uri"] = QString::fromStdString(es.scheme_id_uri);
            o["timescale"] = static_cast<qint64>(es.timescale);
            o["event_count"] = static_cast<qint64>(es.events.size());
            events.append(o);
        }
        root["event_streams"] = events;
    } else {
        QJsonArray playlists;
        for (const model::MediaPlaylistInfo& pl : result.playlists) {
            QJsonObject o;
            o["uri"] = QString::fromStdString(pl.uri);
            o["role"] = QString::fromStdString(pl.role);
            o["playlist_type"] = QString::fromStdString(pl.playlist_type);
            o["has_endlist"] = pl.has_endlist;
            o["live"] = pl.IsLive();
            o["sliding_window"] = pl.IsSlidingWindowLive();
            o["media_sequence"] = static_cast<qint64>(pl.media_sequence);
            o["has_media_sequence"] = pl.has_media_sequence;
            o["window_seconds"] = pl.WindowSeconds();
            o["segment_count"] = static_cast<qint64>(pl.SegmentCount());
            // 业务时间轴：墙钟 / 推荐起点 / delta update / 广告标记
            o["has_program_date_time"] = pl.has_program_date_time;
            o["program_date_time"] = QString::fromStdString(pl.program_date_time);
            o["program_date_time_valid"] = pl.program_date_time_valid;
            o["start_time_offset_s"] = pl.has_start ? pl.start_time_offset_s : 0.0;
            o["has_start"] = pl.has_start;
            o["skipped_segments"] = static_cast<qint64>(pl.skipped_segments);
            if (!pl.date_ranges.empty()) {
                QJsonArray ranges;
                for (const model::HlsDateRangeInfo& dr : pl.date_ranges) {
                    QJsonObject r;
                    r["id"] = QString::fromStdString(dr.id);
                    r["class"] = QString::fromStdString(dr.class_id);
                    r["start_date"] = QString::fromStdString(dr.start_date);
                    r["end_date"] = QString::fromStdString(dr.end_date);
                    r["duration_s"] = dr.has_duration ? dr.duration_seconds : 0.0;
                    r["end_on_next"] = dr.end_on_next;
                    r["has_scte35"] = dr.HasScte35();
                    r["at_seconds"] = dr.at_seconds;
                    ranges.append(r);
                }
                o["date_ranges"] = ranges;
            }
            playlists.append(o);
        }
        root["playlists"] = playlists;

        QJsonArray iframe;
        for (const model::HlsVariantInfo& v : result.variants) {
            if (!v.iframe_only)
                continue;
            QJsonObject o;
            o["index"] = v.index;
            o["uri"] = QString::fromStdString(v.uri);
            o["bandwidth_bps"] = static_cast<qint64>(v.bandwidth_bps);
            iframe.append(o);
        }
        if (!iframe.isEmpty())
            root["iframe_variants"] = iframe;
    }

    QJsonArray ladder;
    for (const model::StreamingLadderEntry& e : result.ladder) {
        QJsonObject o;
        o["label"] = QString::fromStdString(e.label);
        o["bandwidth_bps"] = static_cast<qint64>(e.bandwidth_bps);
        o["width"] = e.width;
        o["height"] = e.height;
        o["codecs"] = QString::fromStdString(e.codecs);
        o["container"] = QString::fromStdString(e.container_hint);
        o["segment_count"] = e.segment_count;
        o["avg_segment_duration"] = e.avg_segment_duration_s;
        o["max_segment_duration"] = e.max_segment_duration_s;
        o["has_init_section"] = e.has_init_section;
        QJsonArray keyframes;
        for (double t : e.keyframe_times)
            keyframes.append(t);
        o["keyframe_times"] = keyframes;
        ladder.append(o);
    }
    root["ladder"] = ladder;

    QJsonArray issues;
    for (const model::StreamingIssue& issue : result.issues) {
        QJsonObject o;
        o["code"] = QString::fromStdString(issue.code);
        o["severity"] = QString::fromUtf8(model::ToString(issue.severity));
        o["title"] = QString::fromStdString(issue.title);
        o["detail"] = QString::fromStdString(issue.detail);
        o["suggestion"] = QString::fromStdString(issue.suggestion);
        o["metric"] = issue.metric_value;
        o["threshold"] = issue.threshold;
        o["occurrence"] = issue.occurrence_count;
        issues.append(o);
    }
    root["issues"] = issues;

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

} // namespace ui
} // namespace videoeye
