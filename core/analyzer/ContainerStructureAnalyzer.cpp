#include "ContainerStructureAnalyzer.h"
#include "FormatDetector.h"
#include "Mp4BoxAnalyzer.h"
#include "EbmlAnalyzer.h"
#include "AviStructureAnalyzer.h"
#include "FlvStructureAnalyzer.h"
#include "TsStructureAnalyzer.h"
#include "AsfStructureAnalyzer.h"
#include "OggStructureAnalyzer.h"
#include "utils/Logger.h"

namespace videoeye {
namespace analyzer {

ContainerStructureAnalyzer::ContainerStructureAnalyzer() = default;
ContainerStructureAnalyzer::~ContainerStructureAnalyzer() = default;

bool ContainerStructureAnalyzer::Analyze(const QString& file_path,
                                          model::ContainerStructureResult& result) {
    result.file_path = file_path;

    // 1. 检测格式
    auto fmt = FormatDetector::Detect(file_path);
    result.format = fmt;
    result.format_name = FormatDetector::FormatName(fmt);

    LOG_INFO("容器结构分析: 检测到格式 = " + result.format_name.toStdString());

    // 2. 根据格式分发到对应解析器
    switch (fmt) {
    case model::ContainerFormat::MP4:
    case model::ContainerFormat::MOV: {
        Mp4BoxAnalyzer mp4_analyzer;
        if (mp4_analyzer.AnalyzeFile(file_path, result.mp4_detail)) {
            // 将 Box 树映射为通用树
            ConvertMp4Tree(result.mp4_detail.box_tree, 0, result.element_tree);
            result.valid = true;

            // 提取流信息
            for (const auto& track : result.mp4_detail.track_tables) {
                model::ContainerStreamInfo si;
                si.index = track.track_id;
                si.type = track.track_type;
                si.codec = "MP4 Codec";
                result.streams.append(si);
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
                                 .arg(result.mp4_detail.track_tables.size());
        } else {
            result.error_message = "MP4 Box 分析失败";
        }
        return result.valid;
    }

    case model::ContainerFormat::MKV:
    case model::ContainerFormat::WebM: {
        EbmlAnalyzer ebml_analyzer;
        if (ebml_analyzer.Analyze(file_path, result.ebml_detail)) {
            ConvertEbmlTree(result.ebml_detail.element_tree, 0, result.element_tree);
            result.valid = true;

            // 提取流信息
            for (const auto& track : result.ebml_detail.tracks) {
                model::ContainerStreamInfo si;
                si.index = track.track_number;
                si.type = track.track_type_name;
                si.codec = track.codec_name;
                if (track.track_type == 1 && track.pixel_width > 0) {
                    si.details = QString("%1x%2").arg(track.pixel_width).arg(track.pixel_height);
                } else if (track.track_type == 2 && track.sampling_frequency > 0) {
                    si.details = QString("%1 Hz, %2 ch").arg(track.sampling_frequency, 0, 'f', 0).arg(track.channels);
                }
                result.streams.append(si);
            }

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
                                 .arg(result.ebml_detail.tracks.size());
        } else {
            result.error_message = "EBML 分析失败";
        }
        return result.valid;
    }

    case model::ContainerFormat::AVI: {
        AviStructureAnalyzer avi;
        return avi.Analyze(file_path, result);
    }
    case model::ContainerFormat::FLV: {
        FlvStructureAnalyzer flv;
        return flv.Analyze(file_path, result);
    }
    case model::ContainerFormat::MPEG_TS: {
        TsStructureAnalyzer ts;
        return ts.Analyze(file_path, result);
    }
    case model::ContainerFormat::ASF: {
        AsfStructureAnalyzer asf;
        return asf.Analyze(file_path, result);
    }
    case model::ContainerFormat::OGG: {
        OggStructureAnalyzer ogg;
        return ogg.Analyze(file_path, result);
    }

    default:
        // FFmpeg 通用回退
        return AnalyzeWithFFmpeg(file_path, result);
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

        // 拼接关键字段作为 value
        QString key_props;
        for (const auto& f : node.fields) {
            if (f.name == "handler_type" || f.name == "major_brand" ||
                f.name == "timescale" || f.name == "duration" ||
                f.name == "width" || f.name == "height" ||
                f.name == "entry_count" || f.name == "sample_rate" ||
                f.name == "channel_count") {
                if (!key_props.isEmpty()) key_props += " | ";
                key_props += f.name + "=" + f.value;
            }
        }
        elem.value = key_props;

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

void ContainerStructureAnalyzer::Reset() {
    // 无状态, 无需重置
}

} // namespace analyzer
} // namespace videoeye
