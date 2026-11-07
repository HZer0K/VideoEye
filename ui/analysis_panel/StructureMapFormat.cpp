#include "ui/analysis_panel/StructureMapFormat.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "ui/theme/AppTheme.h"

namespace videoeye {
namespace ui {
namespace structure_map_detail {

namespace {

using CF = model::ContainerFormat;

QString YesNo(bool value) {
    return value ? QStringLiteral("是") : QStringLiteral("否");
}

QString HtmlEscape(const QString& text) {
    return text.toHtmlEscaped();
}

void AddFact(FormatProfile& profile, const QString& group, const QString& label, const QString& value,
             const QString& note = {}, int level = 0) {
    profile.facts.push_back({group, label, value, note, level});
}

// 通用字节布局判据：至少两个顶层元素给出 size，且总跨度 > 0。
// 靠实测而不是按格式硬编码 —— TS 只有 root 有 size、清单节点全 0，都会被这里挡住。
bool HasUsableByteLayout(const model::ContainerStructureResult& result) {
    int sized = 0;
    quint64 total = 0;
    for (const model::ContainerElement& top : result.element_tree) {
        if (top.size > 0) {
            ++sized;
            total += top.size;
        }
    }
    return sized >= 2 && total > 0;
}

void AddCommonFacts(FormatProfile& profile, const model::ContainerStructureResult& result) {
    AddFact(profile, QStringLiteral("结构"), QStringLiteral("格式"),
            result.format_name.empty() ? profile.family_label : QString::fromStdString(result.format_name));
    AddFact(profile, QStringLiteral("结构"), QStringLiteral("顶层元素"), QString::number(result.element_tree.size()));
    AddFact(profile, QStringLiteral("结构"), QStringLiteral("流"), QString::number(result.streams.size()));
    AddFact(profile, QStringLiteral("结构"), QStringLiteral("元数据"), QString::number(result.metadata.size()));
    const quint64 span = FileSpan(result);
    AddFact(profile, QStringLiteral("结构"), QStringLiteral("字节跨度"),
            span > 0 ? FormatBytes(span) : QStringLiteral("未知"),
            span > 0 ? QStringLiteral("由顶层元素 offset + size 推出") : QStringLiteral("顶层元素未给出字节跨度"));
}

void AddIsobmffFacts(FormatProfile& profile, const model::ContainerStructureResult& result) {
    profile.family = MapFormatFamily::Isobmff;
    profile.family_label = QStringLiteral("ISO-BMFF · MP4 / MOV");
    profile.structure_note = QStringLiteral("顶层 box 按真实 offset / size 定位；mdat 载荷内容不展开");

    const model::Mp4SampleTableResult& samples = result.mp4_samples;
    if (!samples.valid) {
        AddFact(profile, QStringLiteral("样本表"), QStringLiteral("样本表"), QStringLiteral("未产出"),
                QStringLiteral("容器结构树仍可使用；包带视图因此没有 MP4 时间轴"), 1);
        return;
    }
    AddFact(profile, QStringLiteral("样本表"), QStringLiteral("轨道"), QString::number(samples.tracks.size()));
    AddFact(profile, QStringLiteral("样本表"), QStringLiteral("已展开样本"), QString::number(samples.TotalSamples()),
            QStringLiteral("每轨有上限；超限见下方的截断标记"));
    AddFact(profile, QStringLiteral("样本表"), QStringLiteral("分片 moof"), QString::number(samples.fragments.size()),
            samples.fragmented ? QStringLiteral("fMP4 / CMAF 分片文件") : QStringLiteral("单 moov 文件"));
    AddFact(profile, QStringLiteral("样本表"), QStringLiteral("快启"),
            samples.IsFastStart() ? QStringLiteral("moov 在 mdat 前 / 分片") : QStringLiteral("moov 在 mdat 后"));
    int truncated = 0;
    for (const model::Mp4TrackSampleTable& track : samples.tracks) {
        if (track.samples_truncated)
            ++truncated;
    }
    AddFact(profile, QStringLiteral("样本表"), QStringLiteral("样本表截断"),
            truncated > 0 ? QStringLiteral("%1 轨").arg(truncated) : QStringLiteral("无"),
            truncated > 0 ? QStringLiteral("包带是抽样结果，右侧时间段不代表全片") : QString(), truncated > 0 ? 1 : 0);
    AddFact(profile, QStringLiteral("样本表"), QStringLiteral("一致性问题"), QString::number(samples.issues.size()),
            QString(), samples.issues.empty() ? 0 : 1);
}

void AddEbmlFacts(FormatProfile& profile, const model::ContainerStructureResult& result) {
    profile.family = MapFormatFamily::Ebml;
    profile.family_label = QStringLiteral("EBML · Matroska / WebM");
    profile.structure_note = QStringLiteral("元素树只覆盖文件开头的扫描窗口，片尾 Cues / Tags 可能不在树里");

    const model::EbmlAnalysisResult& ebml = result.ebml_detail;
    if (!ebml.valid) {
        AddFact(profile, QStringLiteral("EBML"), QStringLiteral("元素解析"), QStringLiteral("未产出"), QString(), 1);
        return;
    }
    AddFact(profile, QStringLiteral("EBML"), QStringLiteral("DocType"), QString::fromStdString(ebml.doc_type));
    AddFact(profile, QStringLiteral("EBML"), QStringLiteral("时长"),
            ebml.duration_seconds > 0.0 ? QString::number(ebml.duration_seconds, 'f', 2) + QStringLiteral(" s")
                                        : QStringLiteral("未给出"));
    AddFact(profile, QStringLiteral("EBML"), QStringLiteral("轨道 / Cues"),
            QStringLiteral("%1 / %2").arg(ebml.tracks.size()).arg(ebml.cues.size()));
    AddFact(profile, QStringLiteral("EBML"), QStringLiteral("Cluster 总数"), QString::number(ebml.total_clusters),
            QStringLiteral("来自解析期计数，不受结构树窗口限制"));
    AddFact(profile, QStringLiteral("EBML"), QStringLiteral("块摘要"), QString::number(ebml.blocks.size()),
            QStringLiteral("最多 1000 条；块节点另有上限，超限不挂进树"), 1);
    if (ebml.block_nodes_truncated)
        AddFact(profile, QStringLiteral("EBML"), QStringLiteral("树内块节点"), QStringLiteral("按上限未挂进树"),
                QStringLiteral("total_* 计数仍精确"), 1);

    if (ebml.file_bytes > 0 && ebml.scanned_bytes > 0) {
        const double fraction = std::max(
            0.0, std::min(1.0, static_cast<double>(ebml.scanned_bytes) / static_cast<double>(ebml.file_bytes)));
        profile.window_fraction = fraction;
        profile.has_precise_window = fraction < 0.999;
        profile.window_limited = ebml.scan_truncated || fraction < 0.999;
        profile.window_label = QStringLiteral("结构树扫描窗口：前 %1 / 共 %2（%3%）")
                                   .arg(FormatBytes(static_cast<quint64>(ebml.scanned_bytes)),
                                        FormatBytes(static_cast<quint64>(ebml.file_bytes)))
                                   .arg(fraction * 100.0, 0, 'f', 1);
        AddFact(profile, QStringLiteral("扫描窗口"), QStringLiteral("已扫描 / 文件大小"), profile.window_label,
                QStringLiteral("元素树只建到这个边界，右侧区域是未扫描的文件尾部"), profile.window_limited ? 1 : 0);
        if (profile.window_limited)
            profile.coverage_notes << QStringLiteral("结构树仅覆盖前 %1 / 共 %2，片尾元素可能不在树里")
                                          .arg(FormatBytes(static_cast<quint64>(ebml.scanned_bytes)),
                                               FormatBytes(static_cast<quint64>(ebml.file_bytes)));
    }

    const model::MkvDiagnostics& mkv = result.diagnostics.mkv;
    if (mkv.analyzed) {
        AddFact(profile, QStringLiteral("全文件体检"), QStringLiteral("扫描方式"),
                QStringLiteral("元素头顺序扫描（O(1) 内存）"),
                QStringLiteral("与结构树窗口不同口径：这里覆盖到片尾，但不建树"));
        AddFact(profile, QStringLiteral("全文件体检"), QStringLiteral("Cluster"), QString::number(mkv.clusters),
                mkv.scan_complete ? QString() : QStringLiteral("体检扫描未到文件尾"), mkv.scan_complete ? 0 : 1);
        AddFact(profile, QStringLiteral("全文件体检"), QStringLiteral("Cues / SeekHead"),
                QStringLiteral("%1 / %2").arg(YesNo(mkv.has_cues), YesNo(mkv.has_seek_head)));
        AddFact(profile, QStringLiteral("全文件体检"), QStringLiteral("时间戳回退"),
                QString::number(mkv.cluster_timestamp_backwards), QString(),
                mkv.cluster_timestamp_backwards > 0 ? 2 : 0);
        AddFact(profile, QStringLiteral("全文件体检"), QStringLiteral("缺 CodecPrivate 轨道"),
                QString::number(mkv.tracks_missing_codec_private), QString(),
                mkv.tracks_missing_codec_private > 0 ? 2 : 0);
        profile.coverage_notes << QStringLiteral("容器体检为全文件元素头扫描，与结构树窗口口径不同");
    }
}

void AddTransportFacts(FormatProfile& profile, const model::ContainerStructureResult& result) {
    profile.family = MapFormatFamily::TransportStream;
    profile.family_label = QStringLiteral("MPEG-TS 传输流");
    profile.structure_note = QStringLiteral("Program → ES → PES 采样汇总；除根节点外没有 size，字节布局与面积图不适用");
    profile.coverage_notes << QStringLiteral("TS 结构树是 Program / ES / PES 采样汇总，不是全量包布局");

    const model::TransportStreamInfo& ts = result.transport;
    if (!ts.analyzed) {
        AddFact(profile, QStringLiteral("传输层"), QStringLiteral("扫描状态"),
                ts.applicable ? QStringLiteral("适用但未产出") : QStringLiteral("未运行"),
                ts.error_message.empty() ? QStringLiteral("该格式不走传输层体检")
                                         : QString::fromStdString(ts.error_message),
                ts.applicable ? 1 : 0);
        return;
    }
    AddFact(profile, QStringLiteral("传输层"), QStringLiteral("包长"), QStringLiteral("%1 B").arg(ts.packet_size));
    AddFact(profile, QStringLiteral("传输层"), QStringLiteral("已扫 / 预估包数"),
            QStringLiteral("%1 / %2").arg(ts.scanned_packets).arg(ts.estimated_packets));
    AddFact(profile, QStringLiteral("传输层"), QStringLiteral("扫到文件尾"), YesNo(ts.scan_complete),
            ts.scan_complete ? QString() : QStringLiteral("后面的包没有进入任何统计"), ts.scan_complete ? 0 : 1);
    AddFact(profile, QStringLiteral("节目表"), QStringLiteral("节目 / ES / 服务"),
            QStringLiteral("%1 / %2 / %3").arg(ts.programs).arg(ts.streams).arg(ts.services));
    AddFact(profile, QStringLiteral("节目表"), QStringLiteral("PAT / PMT / SDT"),
            QStringLiteral("%1 / %2 / %3").arg(YesNo(ts.has_pat), YesNo(ts.has_pmt), YesNo(ts.has_sdt)));
    AddFact(profile, QStringLiteral("异常计数"), QStringLiteral("失步 / CC 错误 / 加扰"),
            QStringLiteral("%1 / %2 / %3").arg(ts.sync_loss).arg(ts.continuity_errors).arg(ts.scrambled_packets),
            QString(), ts.continuity_errors > 0 || ts.sync_loss > 0 ? 2 : 0);
    AddFact(profile, QStringLiteral("异常计数"), QStringLiteral("PCR 最大间隔"),
            QString::number(ts.pcr_max_gap_ms, 'f', 1) + QStringLiteral(" ms"), QStringLiteral("0 = 窗口内没采到 PCR"));
    AddFact(profile, QStringLiteral("PES 载荷"), QStringLiteral("载荷级扫描"),
            ts.pes_payload_scanned ? QStringLiteral("已开（%1 条 ES）").arg(ts.pes_streams_checked)
                                   : QStringLiteral("未开"),
            ts.pes_payload_scanned ? QString() : QStringLiteral("随机接入点结论不可用"),
            ts.pes_payload_scanned ? 0 : 1);
    if (ts.pes_payload_scanned)
        AddFact(profile, QStringLiteral("PES 载荷"), QStringLiteral("IDR / 无 IDR 视频 PID"),
                QStringLiteral("%1 / %2").arg(ts.idr_count).arg(ts.video_pids_without_idr),
                QStringLiteral("最长无 IDR 间隔 %1 ms").arg(ts.max_no_idr_interval_ms, 0, 'f', 1));

    if (ts.estimated_packets > 0 && ts.scanned_packets > 0) {
        const double fraction = std::max(
            0.0, std::min(1.0, static_cast<double>(ts.scanned_packets) / static_cast<double>(ts.estimated_packets)));
        profile.window_fraction = fraction;
        profile.has_precise_window = fraction < 0.999;
        profile.window_limited = !ts.scan_complete || fraction < 0.999;
        profile.window_label = QStringLiteral("传输层扫描窗口：前 %1 / 预估 %2 包（%3%，按固定包长换算字节）")
                                   .arg(ts.scanned_packets)
                                   .arg(ts.estimated_packets)
                                   .arg(fraction * 100.0, 0, 'f', 1);
        AddFact(profile, QStringLiteral("扫描窗口"), QStringLiteral("扫描覆盖"), profile.window_label,
                QStringLiteral("包长固定，包数比例等价于字节比例；窗口外没有传输层结论"),
                profile.window_limited ? 1 : 0);
        if (profile.window_limited)
            profile.coverage_notes << QStringLiteral("传输层仅扫前 %1 / 预估 %2 包，窗口外的异常不在统计里")
                                          .arg(ts.scanned_packets)
                                          .arg(ts.estimated_packets);
    }
}

void AddLegacyFacts(FormatProfile& profile, const model::ContainerStructureResult& result) {
    profile.family = MapFormatFamily::LegacyBinary;
    profile.family_label = QStringLiteral("传统容器 · %1").arg(QString::fromStdString(result.format_name));
    profile.structure_note = QStringLiteral("按 chunk / tag / page / KLV 顺序建树，元素数有上限；体检为独立顺序扫描");

    const model::ContainerDiagnosticsInfo& d = result.diagnostics;
    const QString group = QStringLiteral("容器体检");
    auto add_window = [&](const char* name, bool analyzed, bool complete) {
        if (!analyzed)
            return;
        AddFact(profile, group, QString::fromUtf8(name) + QStringLiteral(" 扫描"),
                complete ? QStringLiteral("到文件尾") : QStringLiteral("未到文件尾"),
                complete ? QString() : QStringLiteral("结构树有元素上限，尾部元素可能不在树里"), complete ? 0 : 1);
        if (!complete) {
            profile.window_limited = true;
            profile.coverage_notes
                << QStringLiteral("%1 结构扫描未到文件尾（元素树有上限）").arg(QString::fromUtf8(name));
        }
    };

    if (d.avi.analyzed) {
        add_window("AVI", true, d.avi.scan_complete);
        AddFact(profile, group, QStringLiteral("idx1 条目 / 坏偏移"),
                QStringLiteral("%1 / %2").arg(d.avi.idx1_entries).arg(d.avi.idx1_bad_offsets), QString(),
                d.avi.idx1_bad_offsets > 0 ? 2 : 0);
        AddFact(profile, group, QStringLiteral("视频 / 音频流"),
                QStringLiteral("%1 / %2").arg(d.avi.video_streams).arg(d.avi.audio_streams));
        AddFact(profile, group, QStringLiteral("截断 chunk"), QString::number(d.avi.truncated_chunks));
    }
    if (d.flv.analyzed) {
        add_window("FLV", true, d.flv.scan_complete);
        AddFact(profile, group, QStringLiteral("已扫 tag"), QString::number(d.flv.scanned_tags));
        AddFact(profile, group, QStringLiteral("视频 / 音频 / script"),
                QStringLiteral("%1 / %2 / %3").arg(d.flv.video_tags).arg(d.flv.audio_tags).arg(d.flv.script_tags));
        AddFact(profile, group, QStringLiteral("时间戳回退 / 截断 tag"),
                QStringLiteral("%1 / %2").arg(d.flv.timestamp_backwards).arg(d.flv.truncated_tags), QString(),
                d.flv.timestamp_backwards > 0 || d.flv.truncated_tags > 0 ? 2 : 0);
        AddFact(profile, group, QStringLiteral("视频序列头"), YesNo(d.flv.video_sequence_header_seen),
                QStringLiteral("H.264 / HEVC / AV1 需要它才能初始化解码器"));
    }
    if (d.asf.analyzed) {
        add_window("ASF", true, d.asf.scan_complete);
        AddFact(profile, group, QStringLiteral("Header 对象 声明 / 解析"),
                QStringLiteral("%1 / %2").arg(d.asf.declared_header_objects).arg(d.asf.parsed_header_objects));
        AddFact(profile, group, QStringLiteral("Data / Index 对象"),
                QStringLiteral("%1 / %2").arg(YesNo(d.asf.has_data_object), YesNo(d.asf.has_index_object)));
    }
    if (d.ogg.analyzed) {
        add_window("OGG", true, d.ogg.scan_complete);
        AddFact(profile, group, QStringLiteral("页 / 逻辑流"),
                QStringLiteral("%1 / %2").arg(d.ogg.pages).arg(d.ogg.logical_streams));
        AddFact(profile, group, QStringLiteral("缺 EOS / 序号缺口"),
                QStringLiteral("%1 / %2").arg(d.ogg.missing_eos).arg(d.ogg.sequence_gaps), QString(),
                d.ogg.sequence_gaps > 0 ? 2 : 0);
    }
    if (d.mxf.analyzed) {
        add_window("MXF", true, d.mxf.scan_complete);
        AddFact(profile, group, QStringLiteral("分区 头 / 体 / 尾"),
                QStringLiteral("%1 / %2 / %3")
                    .arg(d.mxf.header_partitions)
                    .arg(d.mxf.body_partitions)
                    .arg(d.mxf.footer_partitions));
        AddFact(profile, group, QStringLiteral("片尾 RIP"), YesNo(d.mxf.has_rip),
                QStringLiteral("反向读文件尾得到，不受正向扫描窗口影响"));
        AddFact(profile, group, QStringLiteral("Index 段 / 素材对象"),
                QStringLiteral("%1 / %2").arg(d.mxf.index_segments).arg(d.mxf.essence_objects));
    }
    if (d.mpeg_ps.analyzed) {
        add_window("MPEG-PS", true, d.mpeg_ps.scan_complete);
        AddFact(profile, group, QStringLiteral("Pack / PES / 流"),
                QStringLiteral("%1 / %2 / %3")
                    .arg(d.mpeg_ps.pack_headers)
                    .arg(d.mpeg_ps.pes_packets)
                    .arg(d.mpeg_ps.streams));
        AddFact(profile, group, QStringLiteral("DTS 回退 / 未映射流"),
                QStringLiteral("%1 / %2").arg(d.mpeg_ps.timestamp_backwards).arg(d.mpeg_ps.unmapped_streams), QString(),
                d.mpeg_ps.timestamp_backwards > 0 ? 2 : 0);
    }
}

void AddManifestFacts(FormatProfile& profile, const model::ContainerStructureResult& result) {
    profile.family = MapFormatFamily::Manifest;
    profile.family_label = QStringLiteral("流媒体清单 · HLS / DASH");
    profile.structure_note =
        QStringLiteral("清单层级（variant / playlist / segment），节点没有 size / offset，宽度按等分表示");

    const model::StreamingPackageResult& streaming = result.streaming_package;
    AddFact(profile, QStringLiteral("清单"), QStringLiteral("类型"), QString::fromUtf8(model::ToString(streaming.kind)),
            streaming.valid ? QString() : QStringLiteral("清单解析未成功"), streaming.valid ? 0 : 1);
    if (!streaming.valid)
        return;
    AddFact(profile, QStringLiteral("清单"), QStringLiteral("variant / playlist"),
            QStringLiteral("%1 / %2").arg(streaming.variants.size()).arg(streaming.playlists.size()));
    AddFact(profile, QStringLiteral("清单"), QStringLiteral("Period / Representation"),
            QStringLiteral("%1 / %2").arg(streaming.periods.size()).arg(streaming.representations.size()));
    AddFact(profile, QStringLiteral("分片"), QStringLiteral("清单声明 / 已探测"),
            QStringLiteral("%1 / %2").arg(streaming.TotalSegments()).arg(streaming.ProbedSegments()),
            QStringLiteral("探测有上限，两者不相等是常态"), streaming.SegmentProbeComplete() ? 0 : 1);
    AddFact(profile, QStringLiteral("分片"), QStringLiteral("码率阶梯"), QString::number(streaming.ladder.size()));
    AddFact(profile, QStringLiteral("分片"), QStringLiteral("直播 / 加密 / 远程引用"),
            QStringLiteral("%1 / %2 / %3")
                .arg(YesNo(streaming.IsLive()), YesNo(streaming.IsEncrypted()), YesNo(streaming.remote)),
            QStringLiteral("远程分片只登记不下载"));
    AddFact(profile, QStringLiteral("清单"), QStringLiteral("解析截断"), YesNo(streaming.truncated), QString(),
            streaming.truncated ? 1 : 0);
    AddFact(profile, QStringLiteral("清单"), QStringLiteral("清单问题"), QString::number(streaming.issues.size()));
}

} // namespace

FormatProfile BuildFormatProfile(const model::ContainerStructureResult& result) {
    FormatProfile profile;
    if (!result.valid) {
        profile.family = MapFormatFamily::Unknown;
        profile.family_label = QStringLiteral("未取得结构结果");
        profile.structure_note = QStringLiteral("本次结构分析没有成功，所有视图都没有可用事实");
        profile.has_byte_layout = false;
        return profile;
    }

    switch (result.format) {
    case CF::MP4:
    case CF::MOV:
        AddIsobmffFacts(profile, result);
        break;
    case CF::MKV:
    case CF::WebM:
        AddEbmlFacts(profile, result);
        break;
    case CF::MPEG_TS:
        AddTransportFacts(profile, result);
        break;
    case CF::AVI:
    case CF::FLV:
    case CF::ASF:
    case CF::OGG:
    case CF::MPEG_PS:
    case CF::MXF:
        AddLegacyFacts(profile, result);
        break;
    case CF::HLS:
    case CF::DASH:
        AddManifestFacts(profile, result);
        break;
    case CF::RAW_ES:
        profile.family = MapFormatFamily::RawElementary;
        profile.family_label = QStringLiteral("裸 elementary stream");
        profile.structure_note = QStringLiteral("没有容器层，结构树是 FFmpeg 通用信息的回退");
        profile.coverage_notes << QStringLiteral("裸流无容器层，本图只展示通用信息回退");
        break;
    case CF::FFmpeg_Generic:
    case CF::Unknown:
    default:
        profile.family = result.format == CF::Unknown ? MapFormatFamily::Unknown : MapFormatFamily::Generic;
        profile.family_label = QStringLiteral("通用结构树");
        profile.structure_note = QStringLiteral("格式未特化，只展示已解析出的元素");
        break;
    }

    // 通用判据最后覆盖族默认值：TS 与清单即便族里没显式关掉，也由实测节点决定。
    profile.has_byte_layout = HasUsableByteLayout(result);
    AddCommonFacts(profile, result);
    AddFact(profile, QStringLiteral("口径"), QStringLiteral("结构树口径"), profile.structure_note,
            QStringLiteral("逐包数据 demux 后已释放，本页只画结构骨架"));
    return profile;
}

QString ViewUnsupportedReason(const FormatProfile& profile, MapViewKind kind) {
    if (kind != MapViewKind::Treemap)
        return {};
    if (profile.family == MapFormatFamily::TransportStream)
        return QStringLiteral("TS 只有根节点有文件大小，Program / ES / PES 是采样汇总，面积图无意义");
    if (!profile.has_byte_layout)
        return QStringLiteral("该格式的结构节点没有可比较的 size，面积图会失真（见「格式概览」）");
    return {};
}

bool ViewSupported(const FormatProfile& profile, MapViewKind kind) {
    return ViewUnsupportedReason(profile, kind).isEmpty();
}

QStringList FormatCoverageFacts(const FormatProfile& profile) {
    return profile.coverage_notes;
}

std::vector<MapVisual> BuildFormatVisuals(const FormatProfile& profile, QRect area) {
    std::vector<MapVisual> out;
    if (profile.facts.empty() || area.width() < 120 || area.height() < 60)
        return out;

    // 按 group 出现顺序分列（vector 保序，别换 set —— 卡片顺序也是口径顺序）。
    QStringList groups;
    QMap<QString, std::vector<int>> by_group;
    for (size_t i = 0; i < profile.facts.size(); ++i) {
        const QString& group = profile.facts[static_cast<int>(i)].group;
        if (!by_group.contains(group))
            groups << group;
        by_group[group].push_back(static_cast<int>(i));
    }
    const int columns = std::max(1, std::min(4, std::min<int>(groups.size(), area.width() / 210)));
    const int col_w = area.width() / columns;
    const int shown_groups = std::min<int>(groups.size(), columns);
    for (int col = 0; col < shown_groups; ++col) {
        const int x = area.left() + col * col_w;
        int y = area.top();
        // 组标题只是装饰，不参与命中；图例与事实卡片共用 MapVisual 才能一起进 SVG。
        MapVisual title;
        title.decorative = true;
        title.rect = QRect(x, y, std::max(10, col_w - 8), 22);
        title.fill = QColor(QStringLiteral("#101B27"));
        title.border = QColor(theme::color::kBorder);
        title.label = groups[col];
        out.push_back(title);
        y += 26;
        for (int index : by_group.value(groups[col])) {
            if (y + 32 > area.bottom())
                break; // 绘制预算：卡片不画到绘图区外面
            const FormatFact& fact = profile.facts[static_cast<size_t>(index)];
            MapVisual card;
            card.fact_index = index;
            card.rect = QRect(x, y, std::max(10, col_w - 8), 30);
            card.fill = fact.level == 2   ? QColor(QStringLiteral("#3A1B1B"))
                        : fact.level == 1 ? QColor(QStringLiteral("#332611"))
                                          : QColor(theme::color::kBgCard);
            card.border = QColor(theme::color::kBorder);
            card.label = QStringLiteral("%1  %2").arg(fact.label, fact.value);
            out.push_back(card);
            y += 34;
        }
    }
    return out;
}

QString DescribeFormatFact(const FormatProfile& profile, int index) {
    if (index < 0 || index >= static_cast<int>(profile.facts.size()))
        return {};
    const FormatFact& fact = profile.facts[static_cast<size_t>(index)];
    const QString level = fact.level == 2   ? QStringLiteral("异常事实")
                          : fact.level == 1 ? QStringLiteral("覆盖受限 / 需留意")
                                            : QStringLiteral("实测值");
    return QStringLiteral("<div style='color:%1'><b style='font-size:13px'>%2</b><br>"
                          "<span style='color:%3'>%4 · %5</span><br><br>"
                          "<table cellspacing='2'>"
                          "<tr><td style='color:%3'>实测值</td><td>%6</td></tr>"
                          "<tr><td style='color:%3'>性质</td><td>%7</td></tr>"
                          "</table>%8"
                          "<div style='color:%3;margin-top:8px'>%9</div></div>")
        .arg(QString::fromLatin1(theme::color::kTextPrimary), HtmlEscape(fact.label),
             QString::fromLatin1(theme::color::kTextSecondary), HtmlEscape(fact.group),
             HtmlEscape(profile.family_label), HtmlEscape(fact.value), level,
             fact.note.isEmpty() ? QString()
                                 : QStringLiteral("<hr><div style='color:%1'>%2</div>")
                                       .arg(QString::fromLatin1(theme::color::kTextSecondary), HtmlEscape(fact.note)),
             HtmlEscape(profile.structure_note));
}

std::vector<MapVisual> BuildWindowOverlay(const FormatProfile& profile, const MapGeometry& geom,
                                          const MapViewport& view) {
    std::vector<MapVisual> out;
    if (!profile.has_precise_window || profile.window_fraction <= 0.0 || profile.window_fraction >= 0.999)
        return out;
    const double screen = view.ToScreen(profile.window_fraction);
    if (screen >= 1.0)
        return out;
    const int x = geom.left + static_cast<int>(std::lround(std::max(0.0, screen) * geom.width));
    const int right = geom.left + geom.width;
    if (right - x < 4)
        return out;
    MapVisual overlay;
    overlay.decorative = true;
    overlay.rect = QRect(x, geom.top, right - x, geom.rows * geom.row_height);
    overlay.fill = QColor(QStringLiteral("#1B1414"));
    overlay.border = QColor(QStringLiteral("#5A1E1E"));
    overlay.label = QStringLiteral("窗口外未扫描");
    out.push_back(overlay);
    return out;
}

QJsonObject FormatProfileJson(const FormatProfile& profile) {
    QJsonObject root;
    root[QStringLiteral("family")] = static_cast<int>(profile.family);
    root[QStringLiteral("family_label")] = profile.family_label;
    root[QStringLiteral("structure_note")] = profile.structure_note;
    root[QStringLiteral("has_byte_layout")] = profile.has_byte_layout;
    root[QStringLiteral("window_limited")] = profile.window_limited;
    root[QStringLiteral("has_precise_window")] = profile.has_precise_window;
    root[QStringLiteral("window_fraction")] = profile.window_fraction;
    root[QStringLiteral("window_label")] = profile.window_label;
    root[QStringLiteral("coverage_notes")] = QJsonArray::fromStringList(profile.coverage_notes);
    QJsonArray facts;
    for (const FormatFact& fact : profile.facts) {
        QJsonObject item;
        item[QStringLiteral("group")] = fact.group;
        item[QStringLiteral("label")] = fact.label;
        item[QStringLiteral("value")] = fact.value;
        item[QStringLiteral("note")] = fact.note;
        item[QStringLiteral("level")] = fact.level;
        facts.append(item);
    }
    root[QStringLiteral("facts")] = facts;
    return root;
}

} // namespace structure_map_detail
} // namespace ui
} // namespace videoeye
