#include "core/analyzer/Mp4BoxAnalyzer.h"

#include <algorithm>

#include "utils/IsobmffParser.h"
#include "utils/Logger.h"

namespace videoeye {
namespace analyzer {
namespace {

// 展示用的字段上限: box 树里只列前若干条样本, 完整表走 track_tables / 导出报告
constexpr int kMaxPreviewEntries = 8;
// 单表最多往 TrackBoxTables 里塞多少条（大文件保护）
constexpr uint32_t kMaxTableEntries = 50000;

void PushField(model::Mp4BoxNode& node, const char* name, const QString& value) {
    model::Mp4BoxNode::Field f;
    f.name = QString::fromLatin1(name);
    f.value = value;
    node.fields.push_back(f);
}

void PushFieldInt(model::Mp4BoxNode& node, const char* name, uint64_t value) {
    PushField(node, name, QString::number(static_cast<qulonglong>(value)));
}

// 从自研解析器产出的轨道数据里给样本表 box 填字段
void FillTableFields(model::Mp4BoxNode& node, const utils::IsobmffTrack& track) {
    const QString& type = node.type;
    if (type == QLatin1String("stts")) {
        PushFieldInt(node, "entry_count", track.stts.size());
        PushFieldInt(node, "sample_count", track.SttsSampleCount());
        for (int i = 0; i < std::min<int>(kMaxPreviewEntries, static_cast<int>(track.stts.size())); ++i) {
            PushField(node, QString("entry[%1]").arg(i).toUtf8().constData(),
                      QString("sample_count=%1, sample_duration=%2")
                          .arg(track.stts[i].sample_count)
                          .arg(track.stts[i].sample_delta));
        }
    } else if (type == QLatin1String("ctts")) {
        PushFieldInt(node, "entry_count", track.ctts.size());
        for (int i = 0; i < std::min<int>(kMaxPreviewEntries, static_cast<int>(track.ctts.size())); ++i) {
            PushField(node, QString("entry[%1]").arg(i).toUtf8().constData(),
                      QString("sample_count=%1, sample_offset=%2")
                          .arg(track.ctts[i].sample_count)
                          .arg(track.ctts[i].sample_offset));
        }
    } else if (type == QLatin1String("stco") || type == QLatin1String("co64")) {
        PushFieldInt(node, "entry_count", track.chunk_offsets.size());
        for (int i = 0; i < std::min<int>(kMaxPreviewEntries,
                                          static_cast<int>(track.chunk_offsets.size())); ++i) {
            PushField(node, QString("entry[%1]").arg(i).toUtf8().constData(),
                      QString("chunk_offset=%1").arg(static_cast<qulonglong>(track.chunk_offsets[i])));
        }
    } else if (type == QLatin1String("stsc")) {
        PushFieldInt(node, "entry_count", track.stsc.size());
        for (int i = 0; i < std::min<int>(kMaxPreviewEntries, static_cast<int>(track.stsc.size())); ++i) {
            PushField(node, QString("entry[%1]").arg(i).toUtf8().constData(),
                      QString("first_chunk=%1, samples_per_chunk=%2, sample_description_index=%3")
                          .arg(track.stsc[i].first_chunk)
                          .arg(track.stsc[i].samples_per_chunk)
                          .arg(track.stsc[i].sample_description_index));
        }
    } else if (type == QLatin1String("stsz")) {
        PushFieldInt(node, "sample_size", track.stsz.default_size);
        PushFieldInt(node, "sample_count", track.stsz.sample_count);
        for (int i = 0; i < std::min<int>(kMaxPreviewEntries,
                                          static_cast<int>(track.stsz.sizes.size())); ++i) {
            PushField(node, QString("entry[%1]").arg(i).toUtf8().constData(),
                      QString("size=%1").arg(track.stsz.sizes[i]));
        }
    } else if (type == QLatin1String("stz2")) {
        PushFieldInt(node, "field_size", track.stsz.field_size);
        PushFieldInt(node, "sample_count", track.stsz.sample_count);
    } else if (type == QLatin1String("stss")) {
        PushFieldInt(node, "entry_count", track.stss.size());
        for (int i = 0; i < std::min<int>(kMaxPreviewEntries, static_cast<int>(track.stss.size())); ++i) {
            PushField(node, QString("entry[%1]").arg(i).toUtf8().constData(),
                      QString("sample_number=%1").arg(track.stss[i]));
        }
    } else if (type == QLatin1String("elst")) {
        PushFieldInt(node, "entry_count", track.elst.size());
        for (int i = 0; i < std::min<int>(4, static_cast<int>(track.elst.size())); ++i) {
            PushField(node, QString("entry[%1]").arg(i).toUtf8().constData(),
                      QString("segment_duration=%1, media_time=%2")
                          .arg(static_cast<qulonglong>(track.elst[i].segment_duration))
                          .arg(static_cast<qlonglong>(track.elst[i].media_time)));
        }
    } else if (type == QLatin1String("stsd")) {
        PushField(node, "format", QString::fromStdString(track.codec));
    } else if (type == QLatin1String("tkhd")) {
        PushFieldInt(node, "track_id", track.track_id);
    } else if (type == QLatin1String("hdlr")) {
        PushField(node, "handler_type", QString::fromStdString(track.handler));
    } else if (type == QLatin1String("mdhd")) {
        PushFieldInt(node, "timescale", track.media_timescale);
        PushFieldInt(node, "duration", track.media_duration);
    }
}

// 解析器 box -> UI 模型 box（递归）
model::Mp4BoxNode ConvertBox(const utils::IsobmffBox& src,
                             const utils::IsobmffTrack* track) {
    model::Mp4BoxNode node;
    node.type = QString::fromStdString(src.type);
    node.size = src.size;
    node.offset = src.offset;
    node.depth = src.depth;

    if (src.type == "mvhd") {
        // 电影时基由解析结果整体提供
    } else if (track) {
        FillTableFields(node, *track);
    }

    node.children.reserve(static_cast<int>(src.children.size()));
    for (const auto& child : src.children) {
        node.children.push_back(ConvertBox(child, track));
    }
    return node;
}

model::TrackBoxTables ConvertTrackTables(const utils::IsobmffTrack& src) {
    model::TrackBoxTables t;
    t.track_id = static_cast<int>(src.track_id);
    t.track_type = QString::fromStdString(src.TypeName());

    const uint32_t cap = kMaxTableEntries;
    for (const auto& e : src.stts) {
        if (t.stts_entries.size() >= static_cast<int>(cap)) break;
        model::SttsEntry out;
        out.sample_count = e.sample_count;
        out.sample_delta = e.sample_delta;
        t.stts_entries.push_back(out);
    }
    const bool use_co64 = src.has_co64;
    for (const auto& off : src.chunk_offsets) {
        if (t.stco_entries.size() >= static_cast<int>(cap)) break;
        if (use_co64) {
            model::Co64Entry out;
            out.chunk_offset = off;
            t.co64_entries.push_back(out);
        } else {
            model::StcoEntry out;
            out.chunk_offset = static_cast<uint32_t>(off);
            t.stco_entries.push_back(out);
        }
    }
    for (const auto& e : src.stsc) {
        if (t.stsc_entries.size() >= static_cast<int>(cap)) break;
        model::StscEntry out;
        out.first_chunk = e.first_chunk;
        out.samples_per_chunk = e.samples_per_chunk;
        out.sample_description_index = e.sample_description_index;
        t.stsc_entries.push_back(out);
    }
    for (const auto& s : src.stsz.sizes) {
        if (t.stsz_entries.size() >= static_cast<int>(cap)) break;
        model::StszEntry out;
        out.sample_size = s;
        t.stsz_entries.push_back(out);
    }
    for (const auto& n : src.stss) {
        if (t.stss_entries.size() >= static_cast<int>(cap)) break;
        model::StssEntry out;
        out.sample_number = n;
        t.stss_entries.push_back(out);
    }
    t.stsz_default_size = src.stsz.default_size;
    t.stsz_sample_count = src.stsz.sample_count;
    return t;
}

}  // namespace

Mp4BoxAnalyzer::Mp4BoxAnalyzer() = default;

Mp4BoxAnalyzer::~Mp4BoxAnalyzer() = default;

bool Mp4BoxAnalyzer::AnalyzeFile(const QString& file_path,
                                 model::Mp4BoxAnalysisResult& result) {
    result = model::Mp4BoxAnalysisResult();
    result.file_path = file_path;

    utils::IsobmffParser::Options opt;
    opt.max_entries_per_table = kMaxTableEntries;
    utils::IsobmffFile file;
    if (!utils::IsobmffParser::Parse(file_path.toStdString(), file, opt)) {
        result.error_message = QString::fromStdString(file.error_message);
        LOG_WARN(("Mp4BoxAnalyzer: " + result.error_message).toStdString());
        return false;
    }

    // 顶层 box 树（trak 逐步与 file.tracks 对齐，用于给样本表 box 填字段）
    size_t track_cursor = 0;
    for (const auto& top : file.top_level) {
        if (top.type == "moov") {
            model::Mp4BoxNode moov;
            moov.type = QStringLiteral("moov");
            moov.size = top.size;
            moov.offset = top.offset;
            moov.depth = top.depth;
            for (const auto& child : top.children) {
                const utils::IsobmffTrack* track = nullptr;
                if (child.type == "trak" && track_cursor < file.tracks.size()) {
                    track = &file.tracks[track_cursor++];
                }
                moov.children.push_back(ConvertBox(child, track));
            }
            result.box_tree.push_back(moov);
        } else {
            result.box_tree.push_back(ConvertBox(top, nullptr));
        }
    }

    for (const auto& t : file.tracks) {
        result.track_tables.push_back(ConvertTrackTables(t));
    }

    result.valid = true;
    LOG_INFO("MP4 Box 分析完成: " + std::to_string(result.box_tree.size()) +
             " 个顶级 Box, " + std::to_string(result.track_tables.size()) + " 个 Track 表");
    return true;
}

void Mp4BoxAnalyzer::Reset() {
    // 无状态需要清理
}

}  // namespace analyzer
}  // namespace videoeye
