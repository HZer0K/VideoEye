#include "ContainerStructureAnalyzer.h"
#include "FormatDetector.h"
#include "Mp4BoxAnalyzer.h"
#include "Mp4SampleTableAnalyzer.h"
#include "EbmlAnalyzer.h"
#include "AviStructureAnalyzer.h"
#include "FlvStructureAnalyzer.h"
#include "TsStructureAnalyzer.h"
#include "AsfStructureAnalyzer.h"
#include "OggStructureAnalyzer.h"
#include "HlsManifestAnalyzer.h"
#include "DashManifestAnalyzer.h"
#include "SegmentQcAnalyzer.h"
#include "utils/Logger.h"
#include "utils/ScopedTimer.h"
#include <QStringList>
#include <algorithm>
#include <chrono>

namespace videoeye {
namespace analyzer {

namespace {
// MP4 stsd 采样描述 fourcc → 可读编码名。
// 覆盖 AV1 (av01) / VP9 (vp09) 及常见 H.264/HEVC/AAC 等，
// 未匹配的 fourcc 返回空串，调用方回退显示原始 fourcc。
QString Mp4FourccToName(const QString& fourcc) {
    if (fourcc == "av01") return "AV1";
    if (fourcc == "vp08") return "VP8";
    if (fourcc == "vp09") return "VP9";
    if (fourcc == "avc1" || fourcc == "avc2" ||
        fourcc == "avc3" || fourcc == "avc4") return "H.264 (AVC)";
    if (fourcc == "hev1" || fourcc == "hvc1") return "H.265 (HEVC)";
    if (fourcc == "vvc1" || fourcc == "vvi1") return "H.266 (VVC)";
    if (fourcc == "dva1") return "Dolby Vision (AV1 base)";
    if (fourcc == "dvav") return "Dolby Vision (AVC base)";
    if (fourcc == "dvhe" || fourcc == "dvh1") return "Dolby Vision (HEVC base)";
    if (fourcc == "mp4v") return "MPEG-4 Visual";
    if (fourcc == "mp4a") return "MPEG-4 Audio (AAC)";
    if (fourcc == "opus") return "Opus";
    if (fourcc == "fLaC") return "FLAC";
    if (fourcc == "alac") return "ALAC";
    if (fourcc == "ac-3") return "AC-3";
    if (fourcc == "ec-3") return "E-AC-3";
    if (fourcc == "ac-4") return "AC-4";
    if (fourcc == "mlpa") return "TrueHD (MLP)";
    if (fourcc == "stpp") return "Timed Text (STPP)";
    return QString();
}
} // namespace

ContainerStructureAnalyzer::ContainerStructureAnalyzer() = default;
ContainerStructureAnalyzer::~ContainerStructureAnalyzer() = default;

bool ContainerStructureAnalyzer::Analyze(const QString& file_path,
                                          model::ContainerStructureResult& result) {
    VE_PERF("ContainerStructureAnalyzer::Analyze");
    result.file_path = file_path;
    LOG_INFO("ContainerStructureAnalyzer::Analyze ENTER: " + file_path.toStdString());

    // 1. 检测格式
    auto fmt = FormatDetector::Detect(file_path);
    result.format = fmt;
    result.format_name = FormatDetector::FormatName(fmt);

    LOG_INFO("容器结构分析: 检测到格式 = " + result.format_name.toStdString());
    LOG_INFO("ContainerStructureAnalyzer: 分发到对应解析器, format=" + std::to_string(static_cast<int>(fmt)));

    // 2. 根据格式分发到对应解析器
    switch (fmt) {
    case model::ContainerFormat::MP4:
    case model::ContainerFormat::MOV: {
        Mp4BoxAnalyzer mp4_analyzer;
        bool mp4_ok = false;
        {
            VE_PERF("Mp4BoxAnalyzer::AnalyzeFile(box 树)");
            mp4_ok = mp4_analyzer.AnalyzeFile(file_path, result.mp4_detail);
        }
        if (mp4_ok) {
            auto t0 = std::chrono::steady_clock::now();
            ConvertMp4Tree(result.mp4_detail.box_tree, 0, result.element_tree);
            auto t1 = std::chrono::steady_clock::now();
            LOG_INFO("ContainerStructureAnalyzer: ConvertMp4Tree 耗时 = " +
                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()) + " ms");
            ExtractMp4StreamInfo(result.mp4_detail.box_tree, result);
            auto t2 = std::chrono::steady_clock::now();
            LOG_INFO("ContainerStructureAnalyzer: ExtractMp4StreamInfo 耗时 = " +
                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()) + " ms");
            result.valid = true;

            // 样本级一致性校验（stbl 全表交叉校验 / elst / moof-traf-trun / faststart）。
            // 与 Mp4BoxAnalyzer 是两次独立解析：那边走 Inspector 字符串（为了展示 box 树），
            // 这边直接读原子拿数值（为了算偏移与时间）。失败不影响结构树展示。
            Mp4SampleTableAnalyzer sample_analyzer;
            bool sample_ok = false;
            {
                VE_PERF("Mp4SampleTableAnalyzer::AnalyzeFile(容器页)");
                sample_ok = sample_analyzer.AnalyzeFile(file_path.toStdString(), result.mp4_samples);
            }
            if (sample_ok) {
                LOG_INFO("ContainerStructureAnalyzer: MP4 样本表校验 issues=" +
                         std::to_string(result.mp4_samples.issues.size()));
            }

            int box_count = 0;
            std::function<int(const QVector<model::Mp4BoxNode>&)> count;
            count = [&](const QVector<model::Mp4BoxNode>& nodes) -> int {
                int c = nodes.size();
                for (const auto& n : nodes) c += count(n.children);
                return c;
            };
            box_count = count(result.mp4_detail.box_tree);
            result.summary = QString("MP4 Box | 顶级: %1 | 总计: %2 | Track: %3")
                                 .arg(result.mp4_detail.box_tree.size())
                                 .arg(box_count)
                                 .arg(result.streams.size());
        } else {
            // MP4 解析失败, 回退到 FFmpeg
            LOG_WARN("MP4 专用解析器失败, 回退到 FFmpeg 通用分析");
            return AnalyzeWithFFmpeg(file_path, result);
        }
        return result.valid;
    }

    case model::ContainerFormat::MKV:
    case model::ContainerFormat::WebM: {
        EbmlAnalyzer ebml_analyzer;
        if (ebml_analyzer.Analyze(file_path, result.ebml_detail)) {
            ConvertEbmlTree(result.ebml_detail.element_tree, 0, result.element_tree);
            ExtractEbmlStreamInfo(result.ebml_detail, result);
            result.valid = true;

            int elem_count = 0;
            std::function<int(const QVector<model::EbmlElementNode>&)> count;
            count = [&](const QVector<model::EbmlElementNode>& nodes) -> int {
                int c = nodes.size();
                for (const auto& n : nodes) c += count(n.children);
                return c;
            };
            elem_count = count(result.ebml_detail.element_tree);
            result.summary = QString("%1 | %2 个元素 | %3 轨道")
                                 .arg(result.ebml_detail.doc_type)
                                 .arg(elem_count)
                                 .arg(result.streams.size());
        } else {
            LOG_WARN("EBML 专用解析器失败, 回退到 FFmpeg 通用分析");
            return AnalyzeWithFFmpeg(file_path, result);
        }
        return result.valid;
    }

    case model::ContainerFormat::AVI: {
        AviStructureAnalyzer avi;
        if (avi.Analyze(file_path, result)) return true;
        LOG_WARN("AVI 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result);
    }
    case model::ContainerFormat::FLV: {
        FlvStructureAnalyzer flv;
        if (flv.Analyze(file_path, result)) return true;
        LOG_WARN("FLV 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result);
    }
    case model::ContainerFormat::MPEG_TS: {
        TsStructureAnalyzer ts;
        if (ts.Analyze(file_path, result)) return true;
        LOG_WARN("TS 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result);
    }
    case model::ContainerFormat::HLS:
    case model::ContainerFormat::DASH: {
        if (AnalyzeStreamingManifest(file_path, result)) return true;
        LOG_WARN("流媒体清单解析失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result);
    }
    case model::ContainerFormat::ASF: {
        AsfStructureAnalyzer asf;
        if (asf.Analyze(file_path, result)) return true;
        LOG_WARN("ASF 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result);
    }
    case model::ContainerFormat::OGG: {
        OggStructureAnalyzer ogg;
        if (ogg.Analyze(file_path, result)) return true;
        LOG_WARN("OGG 专用解析器失败, 回退到 FFmpeg");
        return AnalyzeWithFFmpeg(file_path, result);
    }

    default:
        // FFmpeg 通用回退
        return AnalyzeWithFFmpeg(file_path, result);
    }
}

void ContainerStructureAnalyzer::ExtractMp4StreamInfo(const QVector<model::Mp4BoxNode>& box_tree,
                                                        model::ContainerStructureResult& result) {
    // 遍历 box 树, 找到所有 trak, 提取流信息
    std::function<void(const QVector<model::Mp4BoxNode>&)> walk;
    walk = [&](const QVector<model::Mp4BoxNode>& nodes) {
        for (const auto& node : nodes) {
            if (node.type == "trak") {
                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                QString handler_type;
                // 在 trak 子节点中提取 tkhd 和 hdlr 信息
                std::function<void(const QVector<model::Mp4BoxNode>&)> extract_trak;
                extract_trak = [&](const QVector<model::Mp4BoxNode>& children) {
                    for (const auto& child : children) {
                        if (child.type == "tkhd") {
                            for (const auto& f : child.fields) {
                                if (f.name == "track_id") si.details = QString("id=%1").arg(f.value);
                                if (f.name == "width" && f.value != "0") {
                                    // 查找 height
                                    for (const auto& f2 : child.fields) {
                                        if (f2.name == "height" && f2.value != "0") {
                                            si.details += QString(" %1x%2").arg(f.value, f2.value);
                                            break;
                                        }
                                    }
                                }
                            }
                        } else if (child.type == "mdia") {
                            for (const auto& mdia_child : child.children) {
                                if (mdia_child.type == "hdlr") {
                                    for (const auto& f : mdia_child.fields) {
                                        if (f.name == "handler_type") {
                                            handler_type = f.value;
                                            if (f.value == "vide") si.type = "video";
                                            else if (f.value == "soun") si.type = "audio";
                                            else if (f.value == "text" || f.value == "sbtl") si.type = "subtitle";
                                            else si.type = f.value;
                                        }
                                    }
                                } else if (mdia_child.type == "minf") {
                                    // 深入 minf -> stbl -> stsd
                                    std::function<void(const QVector<model::Mp4BoxNode>&)> find_stsd;
                                    find_stsd = [&](const QVector<model::Mp4BoxNode>& stbl_nodes) {
                                        for (const auto& stbl_n : stbl_nodes) {
                                            if (stbl_n.type == "stsd") {
                                                // stsd 的子节点就是编解码器描述
                                                for (const auto& codec_node : stbl_n.children) {
                                                    si.codec = codec_node.type;  // avc1, hvc1, mp4a, av01, vp09 等
                                                    // AV1/VP9 等补充可读编码名: "AV1 (av01)"
                                                    QString readable = Mp4FourccToName(codec_node.type);
                                                    if (!readable.isEmpty()) {
                                                        si.codec = readable + " (" + codec_node.type + ")";
                                                    }
                                                    // 提取编解码器字段中的关键参数
                                                    for (const auto& cf : codec_node.fields) {
                                                        if (cf.name == "width" && !cf.value.isEmpty()) {
                                                            if (si.details.contains("x")) {
                                                                // 替换 tkhd 的粗略尺寸
                                                                int idx = si.details.indexOf("x");
                                                                int start = si.details.lastIndexOf(" ", idx);
                                                                si.details = si.details.left(start + 1) +
                                                                             cf.value + "x";
                                                                // 查找 height
                                                                for (const auto& cf2 : codec_node.fields) {
                                                                    if (cf2.name == "height") {
                                                                        si.details += cf2.value;
                                                                        break;
                                                                    }
                                                                }
                                                            } else {
                                                                si.details += QString(" %1x").arg(cf.value);
                                                                for (const auto& cf2 : codec_node.fields) {
                                                                    if (cf2.name == "height") {
                                                                        si.details += cf2.value;
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        if (cf.name == "sample_rate" && !cf.value.isEmpty()) {
                                                            si.details += QString(" %1Hz").arg(cf.value);
                                                        }
                                                        if (cf.name == "channel_count" && !cf.value.isEmpty()) {
                                                            si.details += QString(" %1ch").arg(cf.value);
                                                        }
                                                    }
                                                }
                                                return;
                                            }
                                            find_stsd(stbl_n.children);
                                        }
                                    };
                                    find_stsd(mdia_child.children);
                                }
                            }
                        }
                    }
                };
                extract_trak(node.children);
                si.details = si.details.trimmed();
                result.streams.append(si);
            }
            walk(node.children);
        }
    };
    walk(box_tree);
}

void ContainerStructureAnalyzer::ExtractEbmlStreamInfo(const model::EbmlAnalysisResult& ebml_detail,
                                                         model::ContainerStructureResult& result) {
    for (const auto& track : ebml_detail.tracks) {
        model::ContainerStreamInfo si;
        si.index = track.track_number;
        si.type = track.track_type_name;
        si.codec = track.codec_name;

        // 构建丰富的 details 字符串
        QStringList parts;
        if (track.track_type == 1) {  // video
            if (track.pixel_width > 0 && track.pixel_height > 0) {
                parts << QString("%1x%2").arg(track.pixel_width).arg(track.pixel_height);
            }
            if (track.frame_rate > 0) {
                parts << QString("%.2f fps").arg(track.frame_rate);
            }
        } else if (track.track_type == 2) {  // audio
            if (track.sampling_frequency > 0) {
                parts << QString("%1 Hz").arg(track.sampling_frequency, 0, 'f', 0);
            }
            if (track.channels > 0) {
                parts << QString("%1 ch").arg(track.channels);
            }
            if (track.bit_depth > 0) {
                parts << QString("%1 bit").arg(track.bit_depth);
            }
        }
        if (!track.language.isEmpty() && track.language != "und") {
            parts << track.language;
        }
        if (!track.track_name.isEmpty()) {
            parts << track.track_name;
        }
        si.details = parts.join(" ");
        result.streams.append(si);
    }

    // 提取 EBML 元数据
    if (!ebml_detail.title.isEmpty()) result.metadata["title"] = ebml_detail.title;
    if (!ebml_detail.muxing_app.isEmpty()) result.metadata["muxing_app"] = ebml_detail.muxing_app;
    if (!ebml_detail.writing_app.isEmpty()) result.metadata["writing_app"] = ebml_detail.writing_app;
    if (ebml_detail.duration_seconds > 0) {
        result.metadata["duration"] = QString("%1s").arg(ebml_detail.duration_seconds, 0, 'f', 2);
    }
}

void ContainerStructureAnalyzer::ConvertMp4Tree(const QVector<model::Mp4BoxNode>& nodes,
                                                  int depth,
                                                  QVector<model::ContainerElement>& out) {
    for (const auto& node : nodes) {
        model::ContainerElement elem;
        elem.name = node.type;
        elem.type = "Box";
        elem.size = node.size;
        elem.offset = node.offset;
        elem.depth = depth;

        // value 列：优先展示关键字段，其后补充其余字段（保证 box 内部信息在树中可见）
        auto isKeyField = [](const QString& n) {
            return n == "handler_type" || n == "major_brand" ||
                   n == "timescale" || n == "duration" ||
                   n == "width" || n == "height" ||
                   n == "entry_count" || n == "sample_rate" ||
                   n == "channel_count" || n == "version" ||
                   n == "flags" || n == "creation_time" ||
                   n == "modification_time" || n == "track_id" ||
                   n == "language" || n == "compatible_brands" ||
                   n == "data_format" || n == "codec";
        };
        QString key_props;    // 关键字段（放前面）
        QString rest_props;   // 其余字段
        QString all_props;    // 全部字段（供 extra/tooltip）
        for (const auto& f : node.fields) {
            const QString kv = f.name + "=" + f.value;
            if (!all_props.isEmpty()) all_props += "\n";
            all_props += kv;
            if (isKeyField(f.name)) {
                if (!key_props.isEmpty()) key_props += " | ";
                key_props += kv;
            } else {
                if (!rest_props.isEmpty()) rest_props += " | ";
                rest_props += kv;
            }
        }
        QString value = key_props;
        if (!rest_props.isEmpty()) {
            if (!value.isEmpty()) value += " | ";
            value += rest_props;
        }
        // value 列长度限制，避免超长字段撑爆列宽；完整内容放 extra 供 tooltip 展示
        if (value.size() > 240) value = value.left(237) + "...";
        elem.value = value;
        elem.extra = all_props;

        ConvertMp4Tree(node.children, depth + 1, elem.children);
        out.append(elem);
    }
}

void ContainerStructureAnalyzer::ConvertEbmlTree(const QVector<model::EbmlElementNode>& nodes,
                                                   int depth,
                                                   QVector<model::ContainerElement>& out) {
    for (const auto& node : nodes) {
        model::ContainerElement elem;
        elem.name = node.name;
        elem.type = "EBML";
        elem.size = node.size;
        elem.offset = node.startOffset();
        elem.depth = depth;
        elem.value = node.value;
        elem.extra = node.extra;

        ConvertEbmlTree(node.children, depth + 1, elem.children);
        out.append(elem);
    }
}

bool ContainerStructureAnalyzer::AnalyzeWithFFmpeg(const QString& file_path,
                                                    model::ContainerStructureResult& result) {
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, file_path.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        result.format = model::ContainerFormat::FFmpeg_Generic;
        result.format_name = "Generic";
        result.error_message = "FFmpeg 无法打开文件";
        result.valid = false;
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        result.error_message = "FFmpeg 无法获取流信息";
        result.valid = false;
        return false;
    }

    result.format = model::ContainerFormat::FFmpeg_Generic;
    result.format_name = fmt_ctx->iformat->name ? fmt_ctx->iformat->name : "Generic";
    result.file_path = file_path;

    // 构建通用结构树
    model::ContainerElement root;
    root.name = QString("%1 Container").arg(result.format_name.toUpper());
    root.type = "Container";
    root.size = fmt_ctx->pb ? avio_size(fmt_ctx->pb) : 0;
    root.offset = 0;
    root.depth = 0;

    // 流信息
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        AVStream* st = fmt_ctx->streams[i];
        model::ContainerElement stream_elem;
        const char* codec_type_name = av_get_media_type_string(st->codecpar->codec_type);
        stream_elem.name = QString("Stream #%1 (%2)").arg(i).arg(codec_type_name ? codec_type_name : "unknown");
        stream_elem.type = "Stream";
        stream_elem.depth = 1;

        const char* codec_name = avcodec_get_name(st->codecpar->codec_id);
        stream_elem.value = QString("codec=%1").arg(codec_name ? codec_name : "?");

        model::ContainerStreamInfo si;
        si.index = i;
        si.type = codec_type_name ? codec_type_name : "unknown";
        si.codec = codec_name ? codec_name : "?";
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            si.details = QString("%1x%2").arg(st->codecpar->width).arg(st->codecpar->height);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            si.details = QString("%1 Hz, %2 ch").arg(st->codecpar->sample_rate).arg(st->codecpar->ch_layout.nb_channels);
        }
        result.streams.append(si);

        root.children.append(stream_elem);
    }

    // Metadata
    AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        result.metadata[QString::fromUtf8(tag->key)] = QString::fromUtf8(tag->value);

        model::ContainerElement meta_elem;
        meta_elem.name = QString::fromUtf8(tag->key);
        meta_elem.type = "Metadata";
        meta_elem.depth = 1;
        meta_elem.value = QString::fromUtf8(tag->value);
        root.children.append(meta_elem);
    }

    // Chapters
    for (unsigned int i = 0; i < fmt_ctx->nb_chapters; ++i) {
        AVChapter* ch = fmt_ctx->chapters[i];
        model::ContainerElement ch_elem;
        ch_elem.name = QString("Chapter #%1").arg(i);
        ch_elem.type = "Chapter";
        ch_elem.depth = 1;
        double start_sec = ch->start * av_q2d(ch->time_base);
        double end_sec = ch->end * av_q2d(ch->time_base);
        ch_elem.value = QString("start=%.2fs end=%.2fs").arg(start_sec).arg(end_sec);

        // Chapter metadata
        AVDictionaryEntry* ch_tag = nullptr;
        while ((ch_tag = av_dict_get(ch->metadata, "", ch_tag, AV_DICT_IGNORE_SUFFIX))) {
            result.metadata[QString("chapter%1_%2").arg(i).arg(ch_tag->key)] =
                QString::fromUtf8(ch_tag->value);
        }

        root.children.append(ch_elem);
    }

    result.element_tree.append(root);
    result.valid = true;
    result.summary = QString("%1 | %2 流 | %3 章节")
                         .arg(result.format_name.toUpper())
                         .arg(fmt_ctx->nb_streams)
                         .arg(fmt_ctx->nb_chapters);

    avformat_close_input(&fmt_ctx);
    return true;
}

bool ContainerStructureAnalyzer::AnalyzeStreamingManifest(const QString& file_path,
                                                          model::ContainerStructureResult& result) {
    VE_PERF("AnalyzeStreamingManifest");
    const std::string path = file_path.toStdString();
    model::StreamingPackageResult& pkg = result.streaming_package;

    bool ok = false;
    if (result.format == model::ContainerFormat::DASH) {
        DashManifestAnalyzer dash;
        ok = dash.AnalyzeFile(path, pkg);
    } else {
        HlsManifestAnalyzer hls;
        ok = hls.AnalyzeFile(path, pkg);
    }
    if (!ok) {
        result.valid = false;
        result.error_message = QString::fromStdString(pkg.error_message);
        return false;
    }

    // 分片级 / ladder 级交叉校验。fMP4 分片在这里被 Mp4SampleTableAnalyzer 解析，
    // MPEG-TS 分片的逐包解析要依赖 Qt，放到下面的 ProbeTsSegments。
    SegmentQcAnalyzer::Analyze(pkg, SegmentQcOptions{});

    ProbeTsSegments(result);
    BuildStreamingTree(result);

    result.valid = true;
    return true;
}

void ContainerStructureAnalyzer::ProbeTsSegments(model::ContainerStructureResult& result) {
    model::StreamingPackageResult& pkg = result.streaming_package;
    constexpr int kMaxTsProbe = 3;  // 抽查头几个就够了：分片是同一次切片产出的
    int probed = 0;

    auto probe_one = [&](model::SegmentInfo& seg) {
        if (probed >= kMaxTsProbe) return;
        if (seg.partial || seg.container != model::SegmentContainer::MPEG_TS) return;
        if (!seg.exists || seg.resolved_path.empty()) return;
        ++probed;
        TsStructureAnalyzer ts;
        model::ContainerStructureResult ts_result;
        if (ts.Analyze(QString::fromStdString(seg.resolved_path), ts_result)) {
            seg.probed = true;
            return;
        }
        seg.container_parse_failed = true;
        seg.container_error = ts_result.error_message.isEmpty()
                                  ? std::string("TS 结构解析失败")
                                  : ts_result.error_message.toStdString();
    };

    for (model::MediaPlaylistInfo& pl : pkg.playlists) {
        for (model::SegmentInfo& seg : pl.segments) probe_one(seg);
    }
    for (model::DashRepresentationInfo& rep : pkg.representations) {
        for (model::SegmentInfo& seg : rep.segments) probe_one(seg);
    }
}

void ContainerStructureAnalyzer::BuildStreamingTree(model::ContainerStructureResult& result) {
    const model::StreamingPackageResult& pkg = result.streaming_package;

    auto make_elem = [](const QString& name, const QString& type, int depth,
                        const QString& value = QString(),
                        const QString& extra = QString()) {
        model::ContainerElement e;
        e.name = name;
        e.type = type;
        e.depth = depth;
        e.value = value;
        e.extra = extra;
        return e;
    };

    model::ContainerElement root = make_elem(QString::fromStdString(model::ToString(pkg.kind)),
                                             "Manifest", 0);
    root.extra = QString::fromStdString(pkg.manifest_path);

    if (pkg.IsDash()) {
        root.value = QString("%1 | %2 Period | %3 Representation")
                         .arg(QString::fromStdString(pkg.mpd_type))
                         .arg(pkg.periods.size())
                         .arg(pkg.representations.size());
        for (const model::DashPeriodInfo& period : pkg.periods) {
            model::ContainerElement period_elem =
                make_elem(QString("Period %1").arg(period.index), "Period", 1,
                          QString("duration=%1s").arg(period.duration_seconds, 0, 'f', 3));
            for (const model::DashAdaptationSetInfo& as : period.adaptation_sets) {
                model::ContainerElement as_elem =
                    make_elem(QString("AdaptationSet %1").arg(as.index), "AdaptationSet", 2,
                              QString::fromStdString(as.content_type + " " + as.mime_type));
                for (int ri : as.representation_indices) {
                    if (ri < 0 || static_cast<size_t>(ri) >= pkg.representations.size()) continue;
                    const model::DashRepresentationInfo& rep = pkg.representations[ri];
                    model::ContainerElement rep_elem = make_elem(
                        QString("Representation %1").arg(QString::fromStdString(rep.id)),
                        "Representation", 3,
                        QString("%1x%2 %3 kbps")
                            .arg(rep.width)
                            .arg(rep.height)
                            .arg(rep.bandwidth_bps / 1000));
                    rep_elem.extra = QString::fromStdString(rep.codecs);
                    constexpr int kMaxSegmentNodes = 100;
                    for (int i = 0; i < static_cast<int>(rep.segments.size()) && i < kMaxSegmentNodes;
                         ++i) {
                        const model::SegmentInfo& seg = rep.segments[i];
                        rep_elem.children.append(make_elem(
                            QString("Segment %1").arg(seg.sequence), "Segment", 4,
                            QString("t=%1s d=%2s %3")
                                .arg(seg.start_seconds, 0, 'f', 3)
                                .arg(seg.duration_seconds, 0, 'f', 3)
                                .arg(seg.exists ? QString("ok") : QString("缺失")),
                            QString::fromStdString(seg.uri)));
                    }
                    if (static_cast<int>(rep.segments.size()) > kMaxSegmentNodes) {
                        rep_elem.children.append(
                            make_elem(QString("... 其余 %1 个分片省略")
                                          .arg(static_cast<int>(rep.segments.size()) - kMaxSegmentNodes),
                                      "Segment", 4));
                    }
                    as_elem.children.append(rep_elem);
                }
                period_elem.children.append(as_elem);
            }
            root.children.append(period_elem);
        }

        // 流信息：视频 ladder + 音频 representation
        for (const model::StreamingLadderEntry& e : pkg.ladder) {
            model::ContainerStreamInfo si;
            si.index = result.streams.size();
            si.type = "video";
            si.codec = QString::fromStdString(e.video_codec);
            si.details = QString("%1x%2 %3 kbps")
                             .arg(e.width)
                             .arg(e.height)
                             .arg(e.bandwidth_bps / 1000);
            result.streams.append(si);
        }
        for (const model::DashRepresentationInfo& rep : pkg.representations) {
            if (rep.content_type != "audio") continue;
            model::ContainerStreamInfo si;
            si.index = result.streams.size();
            si.type = "audio";
            si.codec = QString::fromStdString(rep.audio_codec);
            si.details = QString("%1 kbps").arg(rep.bandwidth_bps / 1000);
            result.streams.append(si);
        }

        result.metadata["MPD type"] = QString::fromStdString(pkg.mpd_type);
        if (pkg.media_presentation_duration_s > 0.0) {
            result.metadata["时长"] =
                QString("%1 s").arg(pkg.media_presentation_duration_s, 0, 'f', 3);
        }
        result.summary = QString("DASH | %1 Period | %2 Representation | %3 分片")
                             .arg(pkg.periods.size())
                             .arg(pkg.representations.size())
                             .arg(static_cast<qulonglong>(pkg.TotalSegments()));
    } else {
        root.value = QString("%1 | %2 variant | %3 playlist")
                         .arg(pkg.kind == model::StreamingKind::HlsMaster ? "master" : "media")
                         .arg(pkg.variants.size())
                         .arg(pkg.playlists.size());

        model::ContainerElement variants_elem =
            make_elem(QString("Variants (%1)").arg(pkg.variants.size()), "Group", 1);
        for (const model::HlsVariantInfo& v : pkg.variants) {
            model::ContainerElement v_elem = make_elem(
                QString("variant #%1").arg(v.index), "Variant", 2,
                QString("%1 kbps %2").arg(v.bandwidth_bps / 1000).arg(QString::fromStdString(v.resolution)));
            v_elem.extra = QString::fromStdString(v.uri + "  codecs=" + v.codecs);
            variants_elem.children.append(v_elem);
        }
        root.children.append(variants_elem);

        if (!pkg.renditions.empty()) {
            model::ContainerElement rend_elem =
                make_elem(QString("Renditions (%1)").arg(pkg.renditions.size()), "Group", 1);
            for (const model::HlsRenditionInfo& r : pkg.renditions) {
                rend_elem.children.append(make_elem(
                    QString::fromStdString(r.type + " " + r.name), "Rendition", 2,
                    QString::fromStdString("group=" + r.group_id +
                                           (r.language.empty() ? "" : " lang=" + r.language))));
            }
            root.children.append(rend_elem);
        }

        model::ContainerElement pl_elem =
            make_elem(QString("Media playlists (%1)").arg(pkg.playlists.size()), "Group", 1);
        constexpr int kMaxSegmentNodes = 100;
        for (const model::MediaPlaylistInfo& pl : pkg.playlists) {
            model::ContainerElement one =
                make_elem(QString("%1 #%2").arg(QString::fromStdString(pl.role)).arg(pl.index),
                          "Playlist", 2,
                          QString("%1 分片 target=%2s")
                              .arg(pl.SegmentCount())
                              .arg(pl.target_duration_s));
            one.extra = QString::fromStdString(pl.uri);
            for (int i = 0; i < static_cast<int>(pl.segments.size()) && i < kMaxSegmentNodes; ++i) {
                const model::SegmentInfo& seg = pl.segments[i];
                one.children.append(make_elem(
                    seg.partial ? QString("Part") : QString("Segment %1").arg(seg.sequence),
                    "Segment", 3,
                    QString("t=%1s d=%2s %3%4")
                        .arg(seg.start_seconds, 0, 'f', 3)
                        .arg(seg.duration_seconds, 0, 'f', 3)
                        .arg(seg.exists ? QString("ok") : QString("缺失"))
                        .arg(seg.discontinuity_before ? " [discontinuity]" : ""),
                    QString::fromStdString(seg.uri)));
            }
            if (static_cast<int>(pl.segments.size()) > kMaxSegmentNodes) {
                one.children.append(make_elem(
                    QString("... 其余 %1 个分片省略")
                        .arg(static_cast<int>(pl.segments.size()) - kMaxSegmentNodes),
                    "Segment", 3));
            }
            pl_elem.children.append(one);
        }
        root.children.append(pl_elem);

        for (const model::StreamingLadderEntry& e : pkg.ladder) {
            model::ContainerStreamInfo si;
            si.index = result.streams.size();
            si.type = e.width > 0 ? "video" : "audio";
            si.codec = QString::fromStdString(e.video_codec.empty() ? e.audio_codec : e.video_codec);
            si.details = QString("%1 kbps %2")
                             .arg(e.bandwidth_bps / 1000)
                             .arg(e.width > 0 ? QString("%1x%2").arg(e.width).arg(e.height)
                                              : QString("audio"));
            result.streams.append(si);
        }

        for (const model::MediaPlaylistInfo& pl : pkg.playlists) {
            if (pl.has_target_duration) {
                result.metadata[QString("EXT-X-TARGETDURATION #%1").arg(pl.index)] =
                    QString("%1 s").arg(pl.target_duration_s);
            }
            if (pl.encrypted) {
                result.metadata[QString("加密 #%1").arg(pl.index)] =
                    QString::fromStdString(pl.key_method);
            }
        }
        result.metadata["点播/直播"] = pkg.IsLive() ? "直播" : "点播";
        result.metadata["低延迟 HLS"] = pkg.playlists.empty() ? "否"
            : (std::any_of(pkg.playlists.begin(), pkg.playlists.end(),
                           [](const model::MediaPlaylistInfo& p) { return p.low_latency; })
                   ? "是"
                   : "否");
        result.summary = QString("HLS | %1 variant | %2 playlist | %3 分片")
                             .arg(pkg.variants.size())
                             .arg(pkg.playlists.size())
                             .arg(static_cast<qulonglong>(pkg.TotalSegments()));
    }

    result.element_tree.append(root);
}

void ContainerStructureAnalyzer::Reset() {
    // 无状态, 无需重置
}

} // namespace analyzer
} // namespace videoeye
