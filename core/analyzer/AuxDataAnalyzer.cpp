#include "core/analyzer/AuxDataAnalyzer.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/dict.h>
#include <libavutil/rational.h>
}

namespace videoeye {
namespace analyzer {
namespace {

constexpr int kMediaTypeData = 4;        // AVMEDIA_TYPE_DATA
constexpr int kMediaTypeAttachment = 5;  // AVMEDIA_TYPE_ATTACHMENT

// MOV/MP4 时码轨的四字符 tag（'tmcd'）。FFmpeg 没有 AV_CODEC_ID_TIMECODE，
// tmcd 轨的 codec 一直是 "none"，只能靠 tag 识别。
constexpr uint32_t kTagTmcd = MKTAG('t', 'm', 'c', 'd');

std::string FourccToString(uint32_t tag) {
    if (tag == 0) return "";
    char buf[8] = {0};
    buf[0] = static_cast<char>((tag >> 24) & 0xFF);
    buf[1] = static_cast<char>((tag >> 16) & 0xFF);
    buf[2] = static_cast<char>((tag >> 8) & 0xFF);
    buf[3] = static_cast<char>(tag & 0xFF);
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c < 0x20 || c > 0x7E) return "";   // 非可打印时放弃，避免表格里出现乱码
    }
    return std::string(buf, 4);
}

const char* MetadataString(const AVDictionary* dict, const char* key) {
    if (dict == nullptr) return nullptr;
    const AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
    return (entry != nullptr) ? entry->value : nullptr;
}

}  // namespace

void AuxDataAnalyzer::Reset(const AuxDataOptions& options) {
    options_ = options;
    result_ = model::AuxiliaryDataResult{};
    data_stream_indices_.clear();
    registered_ = false;
}

model::AuxStreamKind AuxDataAnalyzer::KindFromCodecId(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_SCTE_35:
            return model::AuxStreamKind::Scte35;
        case AV_CODEC_ID_SMPTE_KLV:
            return model::AuxStreamKind::Klv;
        case AV_CODEC_ID_DVB_TELETEXT:
            return model::AuxStreamKind::Teletext;
        case AV_CODEC_ID_EIA_608:
            return model::AuxStreamKind::SubtitleData;
        default:
            return model::AuxStreamKind::GenericData;
    }
}

void AuxDataAnalyzer::AddMetadata(const char* scope, int stream_index, int chapter_index,
                                  const AVDictionary* dict) {
    if (dict == nullptr) return;
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
        if (result_.metadata.size() >= options_.max_metadata_entries) return;
        model::MetadataTagEntry tag;
        tag.scope = scope;
        tag.stream_index = stream_index;
        tag.chapter_index = chapter_index;
        tag.key = entry->key ? entry->key : "";
        tag.value = entry->value ? entry->value : "";
        result_.metadata.push_back(std::move(tag));
    }
}

void AuxDataAnalyzer::CollectMetadata(const AVFormatContext* fmt) {
    if (fmt == nullptr) return;
    AddMetadata("容器", -1, -1, fmt->metadata);

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st == nullptr) continue;
        char scope[32] = {0};
        std::snprintf(scope, sizeof(scope), "流 %u", i);
        AddMetadata(scope, static_cast<int>(i), -1, st->metadata);
    }

    for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
        const AVChapter* ch = fmt->chapters[i];
        if (ch == nullptr) continue;
        char scope[32] = {0};
        std::snprintf(scope, sizeof(scope), "章节 %u", i);
        AddMetadata(scope, -1, static_cast<int>(i), ch->metadata);
    }
}

void AuxDataAnalyzer::RegisterStreams(const AVFormatContext* fmt) {
    if (fmt == nullptr) return;
    result_.streams.clear();
    data_stream_indices_.clear();

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st == nullptr || st->codecpar == nullptr) continue;

        const int media_type = static_cast<int>(st->codecpar->codec_type);
        // 只登记 data 流；字幕流交给 SubtitleAnalyzer（CEA-608 走 data 流的情形仍归这里）
        if (media_type != kMediaTypeData && media_type != kMediaTypeAttachment) continue;

        model::AuxDataStream stream;
        stream.stream_index = static_cast<int>(i);
        stream.media_type = media_type;
        stream.kind = KindFromCodecId(static_cast<int>(st->codecpar->codec_id));
        const char* codec_name = avcodec_get_name(st->codecpar->codec_id);
        stream.codec_name = codec_name ? codec_name : "";
        stream.codec_tag = FourccToString(st->codecpar->codec_tag);

        // MOV/MP4 的 tmcd 时码轨在 FFmpeg 里没有独立 codec id（一直是 codec=none），
        // 只能靠四字符 tag 认出来。
        if (st->codecpar->codec_tag == kTagTmcd) {
            stream.kind = model::AuxStreamKind::Timecode;
        }

        if (const char* handler = MetadataString(st->metadata, "handler_name")) {
            stream.handler_name = handler;
        }
        if (const char* language = MetadataString(st->metadata, "language")) {
            stream.language = language;
        }
        if (const char* title = MetadataString(st->metadata, "title")) {
            stream.title = title;
        }

        const double tb = (st->time_base.den > 0) ? av_q2d(st->time_base) : 0.0;
        if (st->start_time != AV_NOPTS_VALUE && tb > 0.0) {
            stream.start_seconds = static_cast<double>(st->start_time) * tb;
        }
        if (st->duration != AV_NOPTS_VALUE && tb > 0.0) {
            stream.duration_seconds = static_cast<double>(st->duration) * tb;
        }

        if (stream.kind == model::AuxStreamKind::Scte35) {
            stream.note = "SCTE-35 cue 流（广告插入点）";
        } else if (stream.kind == model::AuxStreamKind::Timecode) {
            stream.note = "MOV/MP4 时码轨（首帧时码由 TimecodeAnalyzer 读取）";
        } else if (stream.kind == model::AuxStreamKind::GenericData) {
            stream.note = "未识别的私有 data 流，仅统计包与字节";
        }

        result_.streams.push_back(std::move(stream));
        data_stream_indices_.push_back(static_cast<int>(i));
    }

    if (options_.collect_metadata) CollectMetadata(fmt);
    registered_ = true;
}

void AuxDataAnalyzer::OnPacket(const AVPacket* pkt, const AVStream* stream) {
    if (pkt == nullptr || stream == nullptr || stream->codecpar == nullptr) return;

    const int stream_index = pkt->stream_index;
    model::AuxDataStream* target = nullptr;
    for (model::AuxDataStream& s : result_.streams) {
        if (s.stream_index == stream_index) {
            target = &s;
            break;
        }
    }
    if (target == nullptr) return;

    ++target->packet_count;
    target->byte_count += static_cast<int64_t>(pkt->size);

    const double tb = (stream->time_base.den > 0) ? av_q2d(stream->time_base) : 0.0;
    if (pkt->pts != AV_NOPTS_VALUE && tb > 0.0) {
        const double ts = static_cast<double>(pkt->pts) * tb;
        if (target->packet_count == 1 || ts < target->start_seconds) target->start_seconds = ts;
        const double end = (pkt->duration > 0) ? ts + static_cast<double>(pkt->duration) * tb : ts;
        if (end > target->duration_seconds) target->duration_seconds = end;
    }

    if (!options_.analyze_scte35) return;
    if (result_.cues.size() >= options_.max_cues) return;

    const int codec_id = static_cast<int>(stream->codecpar->codec_id);
    const bool looks_like_scte35 = (codec_id == AV_CODEC_ID_SCTE_35) ||
                                   (pkt->size >= 8 && pkt->data != nullptr && pkt->data[0] == 0xFC);
    if (!looks_like_scte35) return;

    model::Scte35Cue cue;
    if (!Scte35Analyzer::ParseSection(pkt->data, static_cast<size_t>(pkt->size), cue,
                                      options_.scte35_options)) {
        ++result_.parse_error_count;
        return;
    }

    cue.index = static_cast<int>(result_.cues.size());
    cue.stream_index = stream_index;
    if (pkt->pts != AV_NOPTS_VALUE && tb > 0.0) {
        cue.packet_pts_seconds = static_cast<double>(pkt->pts) * tb;
    }
    ++result_.scte35_cue_count;
    if (cue.out_of_network) {
        ++result_.scte35_out_count;
    } else if (!cue.cancel_indicator) {
        ++result_.scte35_in_count;
    }
    if (cue.crc_checked) {
        result_.crc_checked_any = true;
        if (!cue.crc_valid) ++result_.crc_invalid_count;
    }
    result_.cues.push_back(std::move(cue));
}

void AuxDataAnalyzer::Finish() {
    result_.analyzed = registered_;
}

}  // namespace analyzer
}  // namespace videoeye
