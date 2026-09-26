#include "core/analyzer/TimecodeAnalyzer.h"

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

constexpr uint32_t kTagTmcd = MKTAG('t', 'm', 'c', 'd');

// 可能被写成时码的 metadata key（小写比较）
const char* kTimecodeKeys[] = {"timecode", "time_code", "tc", "smpte_timecode", "start_timecode"};

bool EqualsIgnoreCase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
#if defined(_MSC_VER)
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

double StreamFps(const AVStream* st) {
    if (st == nullptr) return 0.0;
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
        return av_q2d(st->avg_frame_rate);
    }
    if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
        return av_q2d(st->r_frame_rate);
    }
    return 0.0;
}

}  // namespace

bool TimecodeAnalyzer::DecodeTmcdSample(const uint8_t* data, size_t size, double fps,
                                        bool drop_frame, model::Timecode& out) {
    out = model::Timecode{};
    if (data == nullptr || size < 4 || fps <= 0.0) return false;
    const uint32_t frame_count = (static_cast<uint32_t>(data[0]) << 24) |
                                 (static_cast<uint32_t>(data[1]) << 16) |
                                 (static_cast<uint32_t>(data[2]) << 8) |
                                 static_cast<uint32_t>(data[3]);
    out = model::TimecodeFromFrameCount(static_cast<int64_t>(frame_count), fps, drop_frame);
    return out.valid;
}

void TimecodeAnalyzer::Reset(const TimecodeOptions& options) {
    options_ = options;
    result_ = model::TimecodeAnalysisResult{};
    timecode_stream_index_ = -1;
    tmcd_decoded_ = false;
    inferred_fps_ = options.fps_hint;
}

void TimecodeAnalyzer::RegisterStreams(const AVFormatContext* fmt, double media_duration_seconds) {
    result_.media_duration_seconds = media_duration_seconds;
    if (fmt == nullptr) return;

    // 帧率：优先调用方给的值，否则取第一条视频流
    if (inferred_fps_ <= 0.0) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* st = fmt->streams[i];
            if (st != nullptr && st->codecpar != nullptr &&
                st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                inferred_fps_ = StreamFps(st);
                break;
            }
        }
    }

    // ---- 1) tmcd 时码轨 ----
    if (options_.read_timecode_track) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* st = fmt->streams[i];
            if (st == nullptr || st->codecpar == nullptr) continue;
            // FFmpeg 里 tmcd 轨没有专属 codec id（codec 一直是 none），
            // 只能靠四字符 tag 'tmcd' 认出来。
            const bool is_tmcd = (st->codecpar->codec_tag == kTagTmcd);
            if (!is_tmcd) continue;

            model::TimecodeTrack track;
            track.stream_index = static_cast<int>(i);
            track.source = model::TimecodeSource::TimecodeTrack;
            track.codec_name = "tmcd";
            track.frame_rate = (StreamFps(st) > 0.0) ? StreamFps(st) : inferred_fps_;
            track.drop_frame = options_.assume_drop_frame_on_ntsc &&
                               model::IsDropFrameRate(track.frame_rate);

            // tmcd 轨自带的 metadata timecode（FFmpeg 常把它直接挂在这一轨上）
            const AVDictionaryEntry* entry = nullptr;
            while ((entry = av_dict_get(st->metadata, "", entry, AV_DICT_IGNORE_SUFFIX))) {
                bool matched = false;
                for (const char* key : kTimecodeKeys) {
                    if (EqualsIgnoreCase(entry->key, key)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;
                const model::Timecode parsed =
                    model::TimecodeFromString(entry->value ? entry->value : "", track.frame_rate);
                if (parsed.valid) {
                    track.first_timecode = parsed;
                    track.drop_frame = parsed.drop_frame;
                    track.raw_text = entry->value;
                    track.metadata_key = entry->key;
                    track.source = model::TimecodeSource::Both;
                    break;
                }
            }

            result_.tracks.push_back(std::move(track));
            if (timecode_stream_index_ < 0) timecode_stream_index_ = static_cast<int>(i);
        }
    }

    // ---- 2) metadata 里的 timecode tag（容器级 + 各流） ----
    if (options_.read_metadata) {
        auto scan_dict = [&](const AVDictionary* dict, int stream_index) {
            if (dict == nullptr) return;
            const AVDictionaryEntry* entry = nullptr;
            while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
                bool matched = false;
                for (const char* key : kTimecodeKeys) {
                    if (EqualsIgnoreCase(entry->key, key)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) continue;

                const model::Timecode parsed =
                    model::TimecodeFromString(entry->value ? entry->value : "", inferred_fps_);
                if (!parsed.valid) continue;

                // 同一条流上的 tag 不重复登记（tmcd 轨已经记过就跳过）
                if (stream_index >= 0 && stream_index == timecode_stream_index_) continue;

                model::TimecodeTrack track;
                track.stream_index = stream_index;
                track.source = model::TimecodeSource::Metadata;
                track.metadata_key = entry->key;
                track.raw_text = entry->value;
                track.frame_rate = inferred_fps_;
                track.drop_frame = parsed.drop_frame ||
                                   (options_.assume_drop_frame_on_ntsc &&
                                    model::IsDropFrameRate(track.frame_rate));
                track.first_timecode = parsed;
                result_.tracks.push_back(std::move(track));
            }
        };

        scan_dict(fmt->metadata, -1);
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (fmt->streams[i] != nullptr) scan_dict(fmt->streams[i]->metadata, static_cast<int>(i));
        }
    }

    // ---- 3) 章节 ----
    for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
        const AVChapter* ch = fmt->chapters[i];
        if (ch == nullptr) continue;
        const double tb = (ch->time_base.den > 0) ? av_q2d(ch->time_base) : 0.0;

        model::ChapterInfo info;
        info.index = static_cast<int>(i);
        info.start_timestamp = ch->start;
        info.end_timestamp = ch->end;
        if (tb > 0.0) {
            info.start_seconds = static_cast<double>(ch->start) * tb;
            info.end_seconds = static_cast<double>(ch->end) * tb;
        }
        const AVDictionaryEntry* title = av_dict_get(ch->metadata, "title", nullptr, 0);
        if (title != nullptr && title->value != nullptr) info.title = title->value;
        const AVDictionaryEntry* lang = av_dict_get(ch->metadata, "language", nullptr, 0);
        if (lang != nullptr && lang->value != nullptr) info.language = lang->value;
        result_.chapters.push_back(std::move(info));
    }
}

void TimecodeAnalyzer::OnPacket(const AVPacket* pkt, const AVStream* stream) {
    if (pkt == nullptr || stream == nullptr) return;
    if (timecode_stream_index_ < 0 || pkt->stream_index != timecode_stream_index_) return;
    if (!options_.read_timecode_track || tmcd_decoded_) return;

    model::TimecodeTrack* track = nullptr;
    for (model::TimecodeTrack& t : result_.tracks) {
        if (t.stream_index == timecode_stream_index_) {
            track = &t;
            break;
        }
    }
    if (track == nullptr) return;

    const double fps = (track->frame_rate > 0.0) ? track->frame_rate : inferred_fps_;
    model::Timecode decoded;
    if (!DecodeTmcdSample(pkt->data, static_cast<size_t>(pkt->size), fps, track->drop_frame, decoded)) {
        return;
    }

    tmcd_decoded_ = true;
    track->first_timecode = decoded;
    const uint32_t frame_count = (pkt->size >= 4)
        ? ((static_cast<uint32_t>(pkt->data[0]) << 24) | (static_cast<uint32_t>(pkt->data[1]) << 16) |
           (static_cast<uint32_t>(pkt->data[2]) << 8) | static_cast<uint32_t>(pkt->data[3]))
        : 0u;
    track->first_frame_count = static_cast<int64_t>(frame_count);

    const double tb = (stream->time_base.den > 0) ? av_q2d(stream->time_base) : 0.0;
    if (pkt->pts != AV_NOPTS_VALUE && tb > 0.0) {
        track->first_presentation_seconds = static_cast<double>(pkt->pts) * tb;
    }
    if (track->source == model::TimecodeSource::Metadata) {
        track->source = model::TimecodeSource::Both;
    }
    track->note = "首帧时码来自 tmcd 轨的首个样本";
}

void TimecodeAnalyzer::AddChapterIssue(model::ChapterIssueType type, int index, double start,
                                       double end, const std::string& detail) {
    model::ChapterIssue issue;
    issue.type = type;
    issue.chapter_index = index;
    issue.start_seconds = start;
    issue.end_seconds = end;
    issue.detail = detail;
    result_.chapter_issues.push_back(std::move(issue));
}

void TimecodeAnalyzer::CheckChapters() {
    if (!options_.check_chapters) return;
    const double duration = result_.media_duration_seconds;

    double prev_start = -1.0;
    double prev_end = -1.0;
    for (size_t i = 0; i < result_.chapters.size(); ++i) {
        const model::ChapterInfo& ch = result_.chapters[i];
        char buf[160] = {0};

        if (prev_start >= 0.0 && ch.start_seconds < prev_start - 1e-6) {
            std::snprintf(buf, sizeof(buf), "章节 %zu 起点 %.3fs 早于上一章节起点 %.3fs", i,
                          ch.start_seconds, prev_start);
            AddChapterIssue(model::ChapterIssueType::NonMonotonic, static_cast<int>(i),
                            ch.start_seconds, ch.end_seconds, buf);
        } else if (prev_end >= 0.0 && ch.start_seconds < prev_end - 1e-3) {
            std::snprintf(buf, sizeof(buf), "章节 %zu 起点 %.3fs 早于上一章节终点 %.3fs（重叠 %.3fs）",
                          i, ch.start_seconds, prev_end, prev_end - ch.start_seconds);
            AddChapterIssue(model::ChapterIssueType::Overlap, static_cast<int>(i),
                            ch.start_seconds, ch.end_seconds, buf);
        }
        if (duration > 0.0 && ch.end_seconds > duration + 0.5) {
            std::snprintf(buf, sizeof(buf), "章节 %zu 终点 %.3fs 超出媒体时长 %.3fs", i,
                          ch.end_seconds, duration);
            AddChapterIssue(model::ChapterIssueType::OutOfRange, static_cast<int>(i),
                            ch.start_seconds, ch.end_seconds, buf);
        }
        if (ch.end_seconds - ch.start_seconds <= 1e-6) {
            std::snprintf(buf, sizeof(buf), "章节 %zu 时长为 0", i);
            AddChapterIssue(model::ChapterIssueType::ZeroDuration, static_cast<int>(i),
                            ch.start_seconds, ch.end_seconds, buf);
        }
        if (ch.title.empty()) {
            std::snprintf(buf, sizeof(buf), "章节 %zu 没有标题", i);
            model::ChapterIssue issue;
            issue.type = model::ChapterIssueType::MissingTitle;
            issue.severity = model::IssueSeverity::Info;
            issue.chapter_index = static_cast<int>(i);
            issue.start_seconds = ch.start_seconds;
            issue.end_seconds = ch.end_seconds;
            issue.detail = buf;
            result_.chapter_issues.push_back(std::move(issue));
        }

        prev_start = ch.start_seconds;
        prev_end = ch.end_seconds;
    }
}

void TimecodeAnalyzer::Finish() {
    CheckChapters();

    // 主时码：tmcd 轨优先（逐样本读数最权威），其次 metadata
    const model::TimecodeTrack* best = nullptr;
    for (const model::TimecodeTrack& track : result_.tracks) {
        if (!track.first_timecode.valid) continue;
        if (best == nullptr) {
            best = &track;
            continue;
        }
        const bool better = (track.source == model::TimecodeSource::TimecodeTrack ||
                             track.source == model::TimecodeSource::Both) &&
                            best->source == model::TimecodeSource::Metadata;
        if (better) best = &track;
    }

    if (best != nullptr) {
        result_.primary = best->first_timecode;
        result_.has_primary = true;
        result_.primary_frame_rate = (best->frame_rate > 0.0) ? best->frame_rate : inferred_fps_;
        result_.primary_drop_frame = best->drop_frame;
    }
    result_.analyzed = true;
}

}  // namespace analyzer
}  // namespace videoeye
