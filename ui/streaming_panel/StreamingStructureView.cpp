#include "ui/streaming_panel/StreamingStructureView.h"

#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/streaming_panel/StreamingFormat.h"

namespace videoeye {
namespace ui {
namespace {

constexpr int kMaxSegmentNodes = 100; // 结构树里单个列表最多画多少分片

} // namespace

StreamingStructureView::StreamingStructureView(QWidget* parent) : QWidget(parent) {
    SetupUi();
}

void StreamingStructureView::SetupUi() {
    // 外层是 QSplitter 的直接子控件，留边距会与拆分前的树控件对不齐
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels(QStringList() << tr("结构") << tr("值"));
    tree_->setAlternatingRowColors(true);
    tree_->setRootIsDecorated(true);
    layout->addWidget(tree_);
}

void StreamingStructureView::Clear() {
    tree_->clear();
}

void StreamingStructureView::SetResult(const model::StreamingPackageResult& result) {
    BuildTree(result);
}

void StreamingStructureView::BuildTree(const model::StreamingPackageResult& result) {
    tree_->clear();
    if (!result.valid)
        return;

    auto add = [](QTreeWidgetItem* parent, const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem(parent);
        item->setText(0, name);
        item->setText(1, value);
        return item;
    };
    auto add_top = [&](const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, name);
        item->setText(1, value);
        return item;
    };

    auto* root = add_top(QString::fromUtf8(model::ToString(result.kind)), QString::fromStdString(result.manifest_path));

    if (result.IsDash()) {
        add(root, tr("MPD 类型"), QString::fromStdString(result.mpd_type));
        if (result.media_presentation_duration_s > 0.0) {
            add(root, tr("总时长"), StreamingSeconds(result.media_presentation_duration_s) + " s");
        }
        // ---- 直播三件套：清单多久更新一次、能回退多久、可用性时间基准 ----
        // dynamic MPD 的分片数全靠这几项推算，缺了任一项界面上的"分片数"都会是 0。
        if (result.has_availability_start_time) {
            add(root, tr("availabilityStartTime"), QString::fromStdString(result.availability_start_time));
        }
        if (!result.publish_time.empty()) {
            add(root, tr("publishTime"), QString::fromStdString(result.publish_time));
        }
        if (result.has_minimum_update_period) {
            add(root, tr("minimumUpdatePeriod"), StreamingSeconds(result.minimum_update_period_s) + " s");
        }
        if (result.has_time_shift_buffer_depth) {
            add(root, tr("timeShiftBufferDepth"), StreamingSeconds(result.time_shift_buffer_depth_s) + " s");
        }
        if (result.has_suggested_presentation_delay) {
            add(root, tr("suggestedPresentationDelay"), StreamingSeconds(result.suggested_presentation_delay_s) + " s");
        }
        // ---- UTCTiming：直播"现在几点"只能从服务端拿 ----
        if (!result.utc_timings.empty()) {
            auto* utc = add(root, tr("UTCTiming"), tr("%1 条").arg(result.utc_timings.size()));
            for (const model::DashUtcTimingInfo& t : result.utc_timings) {
                add(utc, QString::fromStdString(t.scheme_id_uri), QString::fromStdString(t.value));
            }
        }
        // ---- EventStream：广告/章节标记（SCTE-35 over DASH 走这条）----
        if (!result.event_streams.empty()) {
            auto* es_node = add(root, tr("EventStream"),
                                tr("%1 条 / %2 个事件")
                                    .arg(result.event_streams.size())
                                    .arg(static_cast<qulonglong>(result.EventCount())));
            for (const model::DashEventStreamInfo& es : result.event_streams) {
                auto* s = add(es_node, QString::fromStdString(es.scheme_id_uri),
                              tr("timescale=%1 事件=%2").arg(es.timescale).arg(es.events.size()));
                for (const model::DashEventInfo& ev : es.events) {
                    add(s, QString::fromStdString(ev.id.empty() ? std::string("Event") : ev.id),
                        tr("t=%1 s%2")
                            .arg(StreamingSeconds(ev.presentation_time_s))
                            .arg(ev.has_duration ? tr(" d=%1 s").arg(StreamingSeconds(ev.duration_s)) : QString()));
                }
            }
        }
        for (const model::DashPeriodInfo& period : result.periods) {
            auto* p = add(root, QString("Period %1").arg(period.index),
                          QString("duration=%1 s").arg(StreamingSeconds(period.duration_seconds)));
            for (const model::DashAdaptationSetInfo& as : period.adaptation_sets) {
                auto* a = add(p, QString("AdaptationSet %1").arg(as.index),
                              QString::fromStdString(as.content_type + " " + as.mime_type));
                for (const model::DashDescriptor& role : as.roles) {
                    add(a, tr("Role"), QString::fromStdString(role.value));
                }
                if (as.has_content_protection) {
                    auto* cp_node = add(a, tr("ContentProtection"), tr("%1 个系统").arg(as.content_protections.size()));
                    for (const model::DashContentProtectionInfo& cp : as.content_protections) {
                        QString value =
                            QString::fromStdString(cp.IsCencDeclaration() ? ("CENC/" + cp.value) : cp.scheme_id_uri);
                        if (!cp.default_kid.empty())
                            value += tr(" KID=%1").arg(QString::fromStdString(cp.default_kid));
                        if (cp.has_pssh)
                            value += tr(" [pssh]");
                        add(cp_node, tr("DRM"), value);
                    }
                }
                for (const model::DashDescriptor& ep : as.essential_properties) {
                    add(a, tr("EssentialProperty"), QString::fromStdString(ep.scheme_id_uri));
                }
                for (int ri : as.representation_indices) {
                    if (ri < 0 || static_cast<size_t>(ri) >= result.representations.size())
                        continue;
                    const model::DashRepresentationInfo& rep = result.representations[ri];
                    auto* r = add(a, QString("Representation %1").arg(QString::fromStdString(rep.id)),
                                  QString("%1 %2 kbps")
                                      .arg(StreamingResolution(rep.width, rep.height))
                                      .arg(StreamingKbps(rep.bandwidth_bps)));
                    for (int i = 0; i < static_cast<int>(rep.segments.size()) && i < kMaxSegmentNodes; ++i) {
                        const model::SegmentInfo& seg = rep.segments[i];
                        add(r, QString("Segment %1").arg(seg.sequence),
                            QString("t=%1 d=%2 %3")
                                .arg(StreamingSeconds(seg.start_seconds))
                                .arg(StreamingSeconds(seg.duration_seconds))
                                .arg(seg.exists ? tr("ok") : tr("缺失")));
                    }
                    if (static_cast<int>(rep.segments.size()) > kMaxSegmentNodes) {
                        add(r,
                            tr("... 其余 %1 个分片省略").arg(static_cast<int>(rep.segments.size()) - kMaxSegmentNodes),
                            QString());
                    }
                }
            }
        }
    } else {
        for (const model::HlsVariantInfo& v : result.variants) {
            const QString name =
                v.iframe_only ? QString("I-FRAME #%1").arg(v.index) : QString("variant #%1").arg(v.index);
            add(root, name,
                QString("%1 %2 kbps").arg(StreamingResolution(v.width, v.height)).arg(StreamingKbps(v.bandwidth_bps)));
        }
        for (const model::HlsRenditionInfo& r : result.renditions) {
            add(root, QString::fromStdString(r.type + " " + r.name), QString::fromStdString("group=" + r.group_id));
        }
        for (const model::MediaPlaylistInfo& pl : result.playlists) {
            auto* p = add(root, QString("%1 #%2").arg(QString::fromStdString(pl.role)).arg(pl.index),
                          QString("%1 分片 target=%2 s").arg(pl.SegmentCount()).arg(pl.target_duration_s));
            // 直播清单要有明确的"类型 + 序号基准"：滑窗每次重载都会淘汰头部旧分片，
            // 没有 EXT-X-MEDIA-SEQUENCE 就没法知道剩下的分片从第几号开始。
            QString type_text = tr("点播");
            if (!pl.has_endlist) {
                type_text = (pl.playlist_type == "event") ? tr("直播 EVENT（只增不减）") : tr("直播滑窗");
            } else if (pl.playlist_type_vod) {
                type_text = tr("点播 VOD");
            }
            auto* meta = add(p, tr("清单类型"), type_text);
            add(meta, tr("序号基准"), pl.has_media_sequence ? QString::number(pl.media_sequence) : tr("未声明"));
            add(meta, tr("窗口时长"), StreamingSeconds(pl.WindowSeconds()) + " s");
            if (pl.encrypted)
                add(p, tr("加密"), QString::fromStdString(pl.key_method));
            if (pl.has_init_section)
                add(p, tr("初始化段"), QString::fromStdString(pl.init_uri));
            if (pl.low_latency) {
                add(p, tr("LL-HLS"), tr("%1 个部分分片").arg(pl.partial_count));
            }
            // 业务时间轴：墙钟与推荐起点。广告插播、DVR 回看都建立在这两项上。
            if (pl.has_program_date_time) {
                add(p, tr("墙钟起点 (PROGRAM-DATE-TIME)"),
                    QString::fromStdString(pl.program_date_time) +
                        (pl.program_date_time_valid ? QString() : tr("（无法解析）")));
            }
            if (pl.has_start) {
                add(p, tr("推荐起点 (EXT-X-START)"),
                    tr("%1 s%2")
                        .arg(StreamingSeconds(pl.start_time_offset_s))
                        .arg(pl.start_precise ? tr("，精确") : QString()));
            }
            if (pl.has_skip) {
                add(p, tr("delta update (EXT-X-SKIP)"), tr("跳过 %1 个分片").arg(pl.skipped_segments));
            }
            if (!pl.date_ranges.empty()) {
                auto* dr_node = add(p, tr("广告/章节标记 (EXT-X-DATERANGE)"), tr("%1 条").arg(pl.date_ranges.size()));
                for (const model::HlsDateRangeInfo& dr : pl.date_ranges) {
                    const QString label =
                        dr.id.empty() ? (dr.class_id.empty() ? tr("(无 ID)") : QString::fromStdString(dr.class_id))
                                      : QString::fromStdString(dr.id);
                    QString value =
                        dr.at_seconds >= 0.0 ? tr("t=%1 s").arg(StreamingSeconds(dr.at_seconds)) : tr("t=?");
                    if (dr.has_duration)
                        value += tr(" d=%1 s").arg(StreamingSeconds(dr.duration_seconds));
                    else if (!dr.end_date.empty())
                        value += tr(" 至 %1").arg(QString::fromStdString(dr.end_date));
                    else if (dr.end_on_next)
                        value += tr(" 至下一段");
                    else
                        value += tr(" 结束点未知");
                    if (dr.HasScte35())
                        value += tr(" [SCTE-35]");
                    add(dr_node, label, value);
                }
            }
            for (int i = 0; i < static_cast<int>(pl.segments.size()) && i < kMaxSegmentNodes; ++i) {
                const model::SegmentInfo& seg = pl.segments[i];
                add(p, seg.partial ? tr("Part") : QString("Segment %1").arg(seg.sequence),
                    QString("t=%1 d=%2 %3%4")
                        .arg(StreamingSeconds(seg.start_seconds))
                        .arg(StreamingSeconds(seg.duration_seconds))
                        .arg(seg.exists ? tr("ok") : tr("缺失"))
                        .arg(seg.discontinuity_before ? tr(" [discontinuity]") : QString()));
            }
            if (static_cast<int>(pl.segments.size()) > kMaxSegmentNodes) {
                add(p, tr("... 其余 %1 个分片省略").arg(static_cast<int>(pl.segments.size()) - kMaxSegmentNodes),
                    QString());
            }
        }
    }

    tree_->expandToDepth(2);
    tree_->resizeColumnToContents(0);
}

} // namespace ui
} // namespace videoeye
