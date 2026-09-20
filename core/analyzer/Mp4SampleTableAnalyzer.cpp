#include "core/analyzer/Mp4SampleTableAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "utils/Logger.h"

#include "utils/IsobmffParser.h"
namespace videoeye {
namespace analyzer {

namespace {

std::string Hex(uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return std::string(buf);
}

std::string HumanBytes(uint64_t bytes) {
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f KB", kb);
        return std::string(buf);
    }
    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
        return std::string(buf);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    return std::string(buf);
}

std::string Ms(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f ms", ms);
    return std::string(buf);
}

// ---- 校验逻辑（纯 C++，可单测）----

// 追加一条问题；同时把首个相关样本序号记下来供 UI 定位
void PushIssue(model::Mp4SampleTableResult& out,
               const char* code, const char* title,
               const std::string& detail, const std::string& suggestion,
               model::IssueSeverity severity,
               int track_id = -1,
               int occurrence = 1,
               uint32_t sample_index = 0, bool has_sample = false,
               double metric = 0.0, double threshold = 0.0,
               int fragment_index = -1) {
    model::Mp4ConsistencyIssue issue;
    issue.code = code;
    issue.title = title;
    issue.detail = detail;
    issue.suggestion = suggestion;
    issue.severity = severity;
    issue.category = model::IssueCategory::Container;
    issue.track_id = track_id;
    issue.occurrence_count = occurrence;
    issue.sample_index = sample_index;
    issue.has_sample_index = has_sample;
    issue.metric_value = metric;
    issue.threshold = threshold;
    issue.fragment_index = fragment_index;
    out.issues.push_back(std::move(issue));
}

void ValidateTrack(model::Mp4SampleTableResult& out,
                   model::Mp4TrackSampleTable& t,
                   const Mp4SampleTableOptions& opt) {
    using model::IssueSeverity;
    namespace C = model::Mp4IssueCode;

    const uint32_t n = t.stsz_sample_count;

    // ---- 各表样本数一致性 ----
    std::string counts = "stsz=" + std::to_string(n);
    bool mismatch = false;
    if (t.has_stts) {
        counts += ", stts=" + std::to_string(t.stts_sample_count);
        if (t.stts_sample_count != n) mismatch = true;
    }
    if (t.has_ctts) {
        counts += ", ctts=" + std::to_string(t.ctts_sample_count);
        if (t.ctts_sample_count != n) mismatch = true;
    }
    if (t.has_stco || t.has_co64) {
        counts += ", chunk=" + std::to_string(t.chunk_count);
    }
    if (mismatch) {
        PushIssue(out, C::kSampleCountMismatch, "样本表样本数不一致",
                  "Track " + std::to_string(t.track_id) + " 各表样本数不一致（" + counts + "）。"
                  "播放器按 stsz 取大小、按 stts 取时间，两者不符会导致尾部样本读不到或时间轴错位。",
                  "重新封装（-c copy）让 muxer 重写全部 sample table；若文件被截断请重新下载。",
                  IssueSeverity::Error, static_cast<int>(t.track_id));
    }
    if (n > 0 && t.chunk_count == 0) {
        PushIssue(out, C::kMissingChunkOffsets, "缺少 chunk offset 表",
                  "Track " + std::to_string(t.track_id) + " 有 " + std::to_string(n) +
                  " 个样本但没有 stco/co64，无法定位任何样本数据。",
                  "重新封装；碎片化的 stbl 通常是写入过程中被中断造成的。",
                  IssueSeverity::Error, static_cast<int>(t.track_id));
    }
    if (t.has_stco && t.has_co64) {
        PushIssue(out, C::kBothStcoAndCo64, "stco 与 co64 同时存在",
                  "Track " + std::to_string(t.track_id) + " 同时含 stco 与 co64，规范只允许其一，"
                  "不同播放器选取的表可能不同。",
                  "只保留一份 chunk offset 表（大文件保留 co64），重新封装。",
                  IssueSeverity::Error, static_cast<int>(t.track_id));
    }
    if (t.has_stco && t.max_chunk_offset > 0xFFFFFFFFull) {
        PushIssue(out, C::kStcoOverflow, "stco 无法表达 64 位偏移",
                  "Track " + std::to_string(t.track_id) + " 最大 chunk 偏移为 " +
                  Hex(t.max_chunk_offset) + "，超过 32 位，必须用 co64。",
                  "重新封装时使用 co64（多数 muxer 会自动切换，或用 -movflags 相关选项强制）。",
                  IssueSeverity::Error, static_cast<int>(t.track_id));
    }
    if (t.has_stss && t.stss_sync_count > n) {
        PushIssue(out, C::kStssCountMismatch, "stss 关键帧号越界",
                  "Track " + std::to_string(t.track_id) + " 的 stss 声明了 " +
                  std::to_string(t.stss_sync_count) + " 个关键帧，但只有 " + std::to_string(n) +
                  " 个样本。",
                  "重新封装；seek 到越界关键帧会直接失败。",
                  IssueSeverity::Error, static_cast<int>(t.track_id));
    }

    // ---- 逐样本校验 ----
    int out_of_range = 0, zero_size = 0, negative_cts = 0, dts_back = 0;
    int zero_duration = 0, discontinuity = 0;
    uint32_t first_bad_sample = 0;
    uint64_t first_bad_end = 0;
    uint32_t keyframes = 0;
    int64_t prev_dts = 0;
    bool have_prev = false;

    for (size_t i = 0; i < t.samples.size(); ++i) {
        model::Mp4Sample& s = t.samples[i];
        s.flags = model::Mp4SampleFlags::kNone;

        if (s.size == 0) {
            s.flags |= model::Mp4SampleFlags::kZeroSize;
            ++zero_size;
        }
        if (out.file_size > 0 && s.offset + s.size > out.file_size) {
            s.flags |= model::Mp4SampleFlags::kOffsetOutOfRange;
            if (out_of_range == 0) {
                first_bad_sample = s.index;
                first_bad_end = s.offset + s.size;
            }
            ++out_of_range;
        }
        if (s.cts < 0) {
            const double cts_ms = t.media_timescale > 0
                                      ? static_cast<double>(s.cts) * 1000.0 / t.media_timescale
                                      : 0.0;
            if (std::fabs(cts_ms) > opt.negative_cts_tolerance_ms) {
                s.flags |= model::Mp4SampleFlags::kNegativeCts;
                ++negative_cts;
            }
        }
        if (have_prev && s.dts < prev_dts) {
            s.flags |= model::Mp4SampleFlags::kDtsNotMonotonic;
            ++dts_back;
        }
        if (s.duration == 0 && n > 1) {
            s.flags |= model::Mp4SampleFlags::kZeroDuration;
            ++zero_duration;
        }
        if (i > 0) {
            const model::Mp4Sample& p = t.samples[i - 1];
            if (s.chunk_index == p.chunk_index && s.offset != p.offset + p.size) {
                s.flags |= model::Mp4SampleFlags::kChunkDiscontinuity;
                ++discontinuity;
            }
        }
        prev_dts = s.dts;
        have_prev = true;
        if (s.keyframe) ++keyframes;
    }
    if (!t.samples.empty()) {
        t.keyframe_count = keyframes;
        t.first_sample_cts = t.samples.front().cts;
    }

    if (out_of_range > 0) {
        PushIssue(out, C::kChunkOffsetOutOfRange, "样本偏移越过文件末尾",
                  "Track " + std::to_string(t.track_id) + " 有 " + std::to_string(out_of_range) +
                  " 个样本的数据区间超出文件大小（首个异常样本 #" +
                  std::to_string(first_bad_sample) + "，结束位置 " + Hex(first_bad_end) +
                  "，文件 " + HumanBytes(out.file_size) + "）。",
                  "文件很可能被截断或 stco/stsz 被写坏：重新下载/重新封装；"
                  "-c copy 重封装可让 muxer 按实际数据重建偏移表。",
                  IssueSeverity::Error, static_cast<int>(t.track_id), out_of_range,
                  first_bad_sample, true);
    }
    if (zero_size > 0) {
        PushIssue(out, C::kZeroSizeSample, "存在 0 字节样本",
                  "Track " + std::to_string(t.track_id) + " 有 " + std::to_string(zero_size) +
                  " 个大小为 0 的样本，解码器可能报错或丢帧。",
                  "检查编码器输出；重新封装无法凭空补数据。",
                  IssueSeverity::Warning, static_cast<int>(t.track_id), zero_size);
    }
    if (negative_cts > 0) {
        PushIssue(out, C::kNegativeCts, "合成时间为负（ctts 负偏移）",
                  "Track " + std::to_string(t.track_id) + " 有 " + std::to_string(negative_cts) +
                  " 个样本的 PTS 早于 0（ctts version 1 负偏移）。部分播放器会把它当成时间轴回退，"
                  "表现为起播黑帧、首帧被丢或音画不同步。",
                  "用 elst 整体平移代替负 ctts：ffmpeg -c copy 重新封装，"
                  "或用 -avoid_negative_ts make_zero 让时间戳从 0 开始。",
                  IssueSeverity::Warning, static_cast<int>(t.track_id), negative_cts);
    }
    if (dts_back > 0) {
        PushIssue(out, C::kDtsNotMonotonic, "解码时间回退",
                  "Track " + std::to_string(t.track_id) + " 有 " + std::to_string(dts_back) +
                  " 处 DTS 比前一帧更小。含 B 帧时解码顺序会被打乱，"
                  "表现为花屏、解码报错或 seek 卡死。",
                  "重新封装并让 muxer 重算 stts；若是转码产物，检查是否误用了 VFR 输入。",
                  IssueSeverity::Error, static_cast<int>(t.track_id), dts_back);
    }
    if (zero_duration > 0) {
        PushIssue(out, C::kZeroDuration, "存在 0 时长样本",
                  "Track " + std::to_string(t.track_id) + " 有 " + std::to_string(zero_duration) +
                  " 个样本时长为 0，帧率统计与实际时长会偏差。",
                  "确认是否为 VFR 源；必要时用 -vsync cfr 规整时间戳。",
                  model::IssueSeverity::Info, static_cast<int>(t.track_id), zero_duration);
    }
    if (discontinuity > 0) {
        PushIssue(out, C::kOffsetDiscontinuity, "chunk 内样本偏移不连续",
                  "Track " + std::to_string(t.track_id) + " 有 " + std::to_string(discontinuity) +
                  " 处同一 chunk 内样本偏移不等于「上一帧结束位置」，"
                  "说明 stsz 与 stco/stsc 不自洽，读取时会出现数据错位（花屏/爆音）。",
                  "重新封装（-c copy）重建 sample table。",
                  IssueSeverity::Warning, static_cast<int>(t.track_id), discontinuity);
    }
    if (t.has_stss && !t.samples.empty() && !t.samples.front().keyframe) {
        PushIssue(out, C::kFirstSampleNotSync, "首个样本不是关键帧",
                  "Track " + std::to_string(t.track_id) +
                  " 的第一个样本不是关键帧（无 stss 时视为全部关键帧）。"
                  "起播必须先解码到下一个 IDR，首屏会明显变慢甚至出现短暂花屏。",
                  "确认剪辑点是否切在非 IDR 上；重新编码时在起点插入 IDR（-force_key_frames）。",
                  IssueSeverity::Warning, static_cast<int>(t.track_id), 1, 0, true);
    }
    if (t.has_stss && !t.samples.empty() && !t.samples_truncated &&
        keyframes != t.stss_sync_count) {
        PushIssue(out, C::kStssCountMismatch, "关键帧数量与 stss 不符",
                  "Track " + std::to_string(t.track_id) + " 实际关键帧 " +
                  std::to_string(keyframes) + " 个，stss 声明 " +
                  std::to_string(t.stss_sync_count) + " 个。seek 表与真实关键帧不一致。",
                  "重新封装重建 stss。",
                  IssueSeverity::Warning, static_cast<int>(t.track_id));
    }

    // ---- edit list ----
    if (!t.edit_list.empty()) {
        const model::Mp4EditListEntry& e = t.edit_list.front();
        if (e.is_empty_edit) {
            PushIssue(out, model::Mp4IssueCode::kElstEmptyEdit, "存在空编辑（empty edit）",
                      "Track " + std::to_string(t.track_id) +
                      " 的 elst 首条为空编辑，媒体开始前会插入一段静默/黑帧延迟。",
                      "用于对齐音视频起点时这是正常手段；若非预期，去掉 elst 重新封装。",
                      model::IssueSeverity::Info, static_cast<int>(t.track_id));
        } else if (e.media_time > 0 && t.media_timescale > 0) {
            const double shift_ms = static_cast<double>(e.media_time) * 1000.0 /
                                    static_cast<double>(t.media_timescale);
            t.composition_delay_ms = shift_ms;
            if (shift_ms > opt.elst_shift_tolerance_ms) {
                PushIssue(out, model::Mp4IssueCode::kElstFirstFrameShift, "elst 造成首帧偏移",
                          "Track " + std::to_string(t.track_id) + " 的 elst 把媒体起点推后了 " +
                          Ms(shift_ms) + "（media_time=" + std::to_string(e.media_time) +
                          "），前 " + Ms(shift_ms) + " 的内容不会呈现，"
                          "常见表现是「开头少了一段」或首帧与封面不一致。",
                          "确认是否为刻意的音视频对齐；不需要就删除 elst（ffmpeg -c copy 会重写），"
                          "或用 -ss 前先把 elst 折算进去。",
                          IssueSeverity::Warning, static_cast<int>(t.track_id), 1, 0, false,
                          shift_ms, opt.elst_shift_tolerance_ms);
            }
        }
    }
}

void ValidateAvStart(model::Mp4SampleTableResult& out, const Mp4SampleTableOptions& opt) {
    const model::Mp4TrackSampleTable* v = out.FirstTrack("video");
    const model::Mp4TrackSampleTable* a = out.FirstTrack("audio");
    if (!v || !a || v->samples.empty() || a->samples.empty()) return;

    const double vs = v->FirstPresentationSeconds();
    const double as = a->FirstPresentationSeconds();
    const double diff_ms = std::fabs(vs - as) * 1000.0;
    if (diff_ms <= opt.av_start_tolerance_ms) return;

    PushIssue(out, model::Mp4IssueCode::kAvStartMismatch, "音视频起点不一致",
              "视频首个样本呈现于 " + Ms(vs) + "，音频为 " + Ms(as) + "，相差 " + Ms(diff_ms) +
              "（阈值 " + Ms(opt.av_start_tolerance_ms) + "）。"
              "elst 或 ctts 只在一侧做了补偿时就会出现，表现为开头音画不同步。",
              "统一两侧的 elst 偏移（ffmpeg -c copy -avoid_negative_ts make_zero），"
              "或显式用 -itsoffset 对齐其中一条轨。",
              model::IssueSeverity::Warning, -1, 1, 0, false,
              diff_ms, opt.av_start_tolerance_ms);
}

void ValidateFragments(model::Mp4SampleTableResult& out, const Mp4SampleTableOptions& opt) {
    using model::IssueSeverity;
    namespace C = model::Mp4IssueCode;
    if (out.fragments.empty()) return;

    // ---- mfhd sequence_number 连续性（每个 moof 一个序号）----
    std::vector<uint32_t> seqs;
    uint32_t last_moof = UINT32_MAX;
    for (const auto& f : out.fragments) {
        if (f.moof_index != last_moof) {
            seqs.push_back(f.sequence_number);
            last_moof = f.moof_index;
        }
    }
    if (!seqs.empty() && seqs.front() != 1) {
        PushIssue(out, C::kFragmentSequenceGap, "分片起始序号不是 1",
                  "首个 moof 的 sequence_number = " + std::to_string(seqs.front()) +
                  "，通常说明这不是完整的分片文件（拼接/截取产物）。",
                  "确认是否为完整文件；HLS/DASH 分片单独打开时这是预期行为。",
                  model::IssueSeverity::Info);
    }
    int gaps = 0, backwards = 0;
    uint32_t first_gap_seq = 0, first_gap_prev = 0;
    for (size_t i = 1; i < seqs.size(); ++i) {
        if (seqs[i] == seqs[i - 1] || seqs[i] < seqs[i - 1]) {
            if (backwards == 0) { first_gap_seq = seqs[i]; first_gap_prev = seqs[i - 1]; }
            ++backwards;
        } else if (seqs[i] != seqs[i - 1] + 1) {
            if (gaps == 0) { first_gap_seq = seqs[i]; first_gap_prev = seqs[i - 1]; }
            ++gaps;
        }
    }
    if (backwards > 0) {
        PushIssue(out, C::kFragmentSequenceGap, "分片序号重复或回退",
                  "有 " + std::to_string(backwards) + " 处 moof sequence_number 重复/回退（" +
                  std::to_string(first_gap_prev) + " -> " + std::to_string(first_gap_seq) +
                  "）。播放器无法判断分片顺序，会出现时间轴跳变或播放中断。",
                  "检查分片拼接脚本；序号必须单调递增且唯一。",
                  IssueSeverity::Error, -1, backwards);
    }
    if (gaps > 0) {
        PushIssue(out, C::kFragmentSequenceGap, "分片序号不连续",
                  "有 " + std::to_string(gaps) + " 处 moof sequence_number 跳号（" +
                  std::to_string(first_gap_prev) + " -> " + std::to_string(first_gap_seq) +
                  "），中间分片可能缺失。",
                  "补齐缺失分片再播放；若是有意拼接多段，确认时间轴（tfdt）是否也做了拼接。",
                  IssueSeverity::Warning, -1, gaps);
    }

    // ---- tfdt 解码时间连续性（按 track 分开看）----
    std::map<uint32_t, const model::Mp4FragmentInfo*> prev_of_track;
    int time_gaps = 0, time_backwards = 0;
    bool first_origin_nonzero = false;
    for (const auto& f : out.fragments) {
        if (!f.has_tfdt) continue;
        auto it = prev_of_track.find(f.track_id);
        if (it == prev_of_track.end()) {
            if (f.base_media_decode_time != 0) first_origin_nonzero = true;
        } else {
            const model::Mp4FragmentInfo* p = it->second;
            const uint64_t expected = p->base_media_decode_time + p->duration;
            const int64_t delta = static_cast<int64_t>(f.base_media_decode_time) -
                                  static_cast<int64_t>(expected);
            if (delta < 0 || static_cast<uint64_t>(std::llabs(delta)) > opt.fragment_time_tolerance) {
                if (delta < 0) ++time_backwards; else ++time_gaps;
            }
        }
        prev_of_track[f.track_id] = &f;
    }
    if (time_backwards > 0) {
        PushIssue(out, C::kFragmentTimeGap, "分片解码时间回退",
                  "有 " + std::to_string(time_backwards) +
                  " 处分片的 tfdt 早于「上一分片起始时间 + 上一分片时长」，"
                  "解码时间轴出现回退，播放器会丢帧或直接报错。",
                  "重新生成分片；tfdt 必须严格等于前一分片的结束时间。",
                  IssueSeverity::Error, -1, time_backwards);
    }
    if (time_gaps > 0) {
        PushIssue(out, C::kFragmentTimeGap, "分片解码时间不连续",
                  "有 " + std::to_string(time_gaps) +
                  " 处分片的 tfdt 与上一分片结束时间对不上（存在时间空洞），"
                  "表现为分段处短暂停顿、时长统计偏大或 seek 落点偏移。",
                  "重新生成分片，保证 tfdt 连续；拼接多段时要做时间轴平移（如 ffmpeg 的 "
                  "-output_ts_offset 或 mp4box -cat 的时间轴处理）。",
                  IssueSeverity::Warning, -1, time_gaps);
    }
    if (first_origin_nonzero) {
        PushIssue(out, C::kFragmentTimeOrigin, "分片时间原点非 0",
                  "首个分片的 tfdt 不是 0，说明文件是长视频的一段（直播切片/截取）。",
                  "单独播放时会显示非零起始时间，属预期行为；需要完整时间轴请用完整文件。",
                  model::IssueSeverity::Info);
    }

    // ---- trun 数据偏移是否可定位 ----
    int no_data_offset = 0;
    for (const auto& f : out.fragments) {
        const bool can_locate = f.base_data_offset_present || f.default_base_is_moof ||
                                f.trun_data_offset_present;
        if (!can_locate) ++no_data_offset;
    }
    if (no_data_offset > 0) {
        PushIssue(out, C::kFragmentDataOffset, "分片样本数据位置无法定位",
                  "有 " + std::to_string(no_data_offset) +
                  " 个 traf 既没有 tfhd.base_data_offset、也没有 default-base-is-moof、"
                  "trun 也没写 data_offset，播放器无法算出样本数据的文件偏移。",
                  "重新封装；CMAF 要求 base_data_offset 或 default-base-is-moof 至少有一个。",
                  IssueSeverity::Error, -1, no_data_offset);
    }
}

}  // namespace

void Mp4SampleTableAnalyzer::Validate(model::Mp4SampleTableResult& out,
                                      const Mp4SampleTableOptions& options) {
    out.issues.clear();
    if (!out.valid) return;

    namespace C = model::Mp4IssueCode;

    // ---- 文件级: faststart ----
    if (!out.IsFastStart()) {
        std::string pos;
        if (out.moov_size > 0) {
            pos = "moov 位于 " + Hex(out.moov_offset) + "（" + HumanBytes(out.moov_size) +
                  "），首个 mdat 位于 " + Hex(out.first_mdat_offset);
        } else {
            pos = "未找到 moov";
        }
        PushIssue(out, C::kNotFastStart, "moov 在 mdat 之后（未 faststart）",
                  pos + "。索引在媒体数据之后，播放器必须先把整个文件下载完才能拿到 moov，"
                        "点播首屏会长时间黑屏，拖动进度条也会等很久。",
                  "重新封装前置 moov：ffmpeg -i in.mp4 -c copy -movflags +faststart out.mp4"
                  "（或 qt-faststart in.mp4 out.mp4）。",
                  model::IssueSeverity::Warning);
    }

    for (auto& track : out.tracks) {
        ValidateTrack(out, track, options);
    }
    ValidateAvStart(out, options);
    ValidateFragments(out, options);
}



// ---------------------------------------------------------------------------
// 解析部分: utils::IsobmffParser 的输出 -> model::Mp4SampleTableResult
//
// 这一段原先由 Bento4 (AP4_File / AP4_Track::GetSample) 承担。自研 IsobmffParser
// 已经把 stts/ctts/stss/stsz/stsc/stco/co64/elst 与 fMP4 的 traf/tfhd/tfdt/trun
// 都解析成裸表，这里只负责"按规范把表展开成逐样本"。
// ---------------------------------------------------------------------------

namespace {

using utils::IsobmffTrack;

uint32_t SampleSizeAt(const utils::StszTable& stsz, uint32_t index) {
    if (stsz.default_size != 0) return stsz.default_size;
    if (index < stsz.sizes.size()) return stsz.sizes[index];
    return 0;
}

uint64_t TotalSampleBytes(const utils::StszTable& stsz, uint32_t sample_count) {
    if (stsz.default_size != 0) {
        return static_cast<uint64_t>(stsz.default_size) * sample_count;
    }
    uint64_t total = 0;
    const uint32_t n = std::min(sample_count, static_cast<uint32_t>(stsz.sizes.size()));
    for (uint32_t i = 0; i < n; ++i) total += stsz.sizes[i];
    return total;
}

// 按 stts/ctts/stsc/stco 展开逐样本信息。
// 大文件受 options.max_samples_per_track 限制（只展开前 N 个，表级校验照做）。
void ExpandSamples(const IsobmffTrack& src, model::Mp4TrackSampleTable& t,
                   uint32_t max_samples) {
    const uint32_t n = t.stsz_sample_count;
    if (n == 0) return;

    const uint32_t limit = (max_samples > 0) ? std::min(n, max_samples) : n;
    t.samples_truncated = (limit < n);
    t.samples.reserve(limit);

    // stss 是 1-based 样本号；无 stss 时规范规定"每个样本都是同步样本"
    std::vector<bool> sync;
    if (src.has_stss) {
        sync.assign(limit + 1, false);
        for (uint32_t s : src.stss) {
            if (s >= 1 && s <= limit) sync[s] = true;
        }
    }

    const uint32_t chunk_count = static_cast<uint32_t>(src.chunk_offsets.size());
    size_t stsc_idx = 0;
    size_t stts_idx = 0, stts_used = 0;
    size_t ctts_idx = 0, ctts_used = 0;
    int64_t dts = 0;
    uint32_t produced = 0;

    for (uint32_t c = 1; c <= chunk_count && produced < limit; ++c) {
        // 找到覆盖当前 chunk 的 stsc 条目（first_chunk 升序，1-based）
        while (stsc_idx + 1 < src.stsc.size() &&
               src.stsc[stsc_idx + 1].first_chunk <= c) {
            ++stsc_idx;
        }
        if (stsc_idx >= src.stsc.size()) break;
        const uint32_t samples_per_chunk = src.stsc[stsc_idx].samples_per_chunk;
        const uint32_t desc_index = src.stsc[stsc_idx].sample_description_index;

        uint64_t offset = src.chunk_offsets[c - 1];
        for (uint32_t k = 0; k < samples_per_chunk && produced < limit; ++k, ++produced) {
            uint32_t duration = 0;
            while (stts_idx < src.stts.size() &&
                   stts_used >= src.stts[stts_idx].sample_count) {
                stts_used = 0;
                ++stts_idx;
            }
            if (stts_idx < src.stts.size()) {
                duration = src.stts[stts_idx].sample_delta;
                ++stts_used;
            }

            int32_t cts_delta = 0;
            if (src.has_ctts) {
                while (ctts_idx < src.ctts.size() &&
                       ctts_used >= src.ctts[ctts_idx].sample_count) {
                    ctts_used = 0;
                    ++ctts_idx;
                }
                if (ctts_idx < src.ctts.size()) {
                    cts_delta = src.ctts[ctts_idx].sample_offset;
                    ++ctts_used;
                }
            }

            const uint32_t size = SampleSizeAt(src.stsz, produced);
            model::Mp4Sample s;
            s.index = produced;
            s.chunk_index = c - 1;
            s.index_in_chunk = k;
            s.offset = offset;
            s.size = size;
            s.dts = dts;
            s.cts_delta = cts_delta;
            s.cts = dts + static_cast<int64_t>(cts_delta);
            s.duration = duration;
            s.keyframe = src.has_stss ? sync[produced + 1] : true;
            s.description_index = desc_index;
            t.samples.push_back(std::move(s));

            offset += size;
            dts += static_cast<int64_t>(duration);
        }
    }

    // 展开数量不足又不是被上限截断的，说明表本身不自洽（stsc/stsz/stco 对不上）
    if (produced < limit) {
        t.sample_read_failed = true;
        t.sample_read_error_index = produced;
    }
    if (!t.samples.empty()) {
        t.first_sample_cts = t.samples.front().cts;
    }
}

void CollectTrack(const IsobmffTrack& src, model::Mp4TrackSampleTable& t,
                  const Mp4SampleTableOptions& options) {
    t.track_id = src.track_id;
    t.type = src.TypeName();
    t.codec = src.codec;
    t.media_timescale = src.media_timescale;
    t.media_duration = src.media_duration;

    t.has_stts = src.has_stts;
    t.has_ctts = src.has_ctts;
    t.has_stss = src.has_stss;
    t.has_stsz = src.has_stsz;
    t.has_stz2 = src.has_stz2;
    t.has_stsc = src.has_stsc;
    t.has_stco = src.has_stco;
    t.has_co64 = src.has_co64;
    t.has_elst = src.has_elst;

    t.stsz_sample_count = src.stsz.sample_count;
    t.stts_sample_count = src.SttsSampleCount();
    t.ctts_sample_count = src.CttsSampleCount();
    t.stss_sync_count = static_cast<uint32_t>(src.stss.size());
    t.chunk_count = static_cast<uint32_t>(src.chunk_offsets.size());

    for (uint64_t off : src.chunk_offsets) {
        if (off > t.max_chunk_offset) t.max_chunk_offset = off;
    }
    t.total_sample_bytes = TotalSampleBytes(src.stsz, t.stsz_sample_count);

    for (const auto& e : src.elst) {
        model::Mp4EditListEntry entry;
        entry.segment_duration = e.segment_duration;
        entry.media_time = e.media_time;
        entry.media_rate = static_cast<uint16_t>(e.media_rate_integer);
        entry.is_empty_edit = (e.media_time < 0);
        t.edit_list.push_back(entry);
    }

    if (options.expand_samples) {
        ExpandSamples(src, t, options.max_samples_per_track);
    } else {
        t.samples_truncated = (t.stsz_sample_count > 0);
    }
}

model::Mp4FragmentInfo ConvertFragment(const utils::IsobmffFragment& f) {
    model::Mp4FragmentInfo out;
    out.moof_index = f.moof_index;
    out.index = f.index;
    out.sequence_number = f.sequence_number;
    out.track_id = f.track_id;
    out.offset = f.offset;
    out.size = f.size;
    out.has_tfdt = f.has_tfdt;
    out.base_media_decode_time = f.base_media_decode_time;
    out.sample_count = f.sample_count;
    out.duration = f.duration;
    out.total_size = f.total_size;
    out.base_data_offset_present = f.base_data_offset_present;
    out.default_base_is_moof = f.default_base_is_moof;
    out.sample_description_index_present = f.sample_description_index_present;
    out.default_sample_duration_present = f.default_sample_duration_present;
    out.default_sample_size_present = f.default_sample_size_present;
    out.duration_is_empty = f.duration_is_empty;
    out.base_data_offset = f.base_data_offset;
    out.trun_count = f.trun_count;
    out.trun_data_offset_present = f.trun_data_offset_present;
    out.trun_data_offset = f.trun_data_offset;
    return out;
}

}  // namespace

Mp4SampleTableAnalyzer::Mp4SampleTableAnalyzer() = default;

bool Mp4SampleTableAnalyzer::AnalyzeFile(const std::string& file_path,
                                         model::Mp4SampleTableResult& out,
                                         const Mp4SampleTableOptions& options) {
    options_ = options;
    out = model::Mp4SampleTableResult{};
    out.file_path = file_path;

    utils::IsobmffParser::Options parse_options;
    parse_options.skip_sample_tables = !options.expand_samples;

    utils::IsobmffFile parsed;
    if (!utils::IsobmffParser::Parse(file_path, parsed, parse_options)) {
        out.valid = false;
        out.error_message = parsed.error_message.empty()
                                ? std::string("无法按 ISOBMFF (MP4/MOV) 解析该文件")
                                : parsed.error_message;
        LOG_WARN("MP4 样本表分析失败: " + file_path + " -> " + out.error_message);
        return false;
    }

    out.valid = true;
    out.file_size = parsed.file_size;
    out.fragmented = parsed.fragmented;
    out.moov_before_mdat = parsed.moov_before_mdat;
    out.moov_offset = parsed.moov_offset;
    out.moov_size = parsed.moov_size;
    out.first_mdat_offset = parsed.first_mdat_offset;
    out.first_mdat_size = parsed.first_mdat_size;
    out.top_level_order = parsed.top_level_order;
    out.movie_timescale = parsed.movie_timescale;
    out.sidx_count = parsed.sidx_count;
    out.styp_count = parsed.styp_count;

    out.tracks.reserve(parsed.tracks.size());
    for (const auto& src : parsed.tracks) {
        model::Mp4TrackSampleTable t;
        CollectTrack(src, t, options);
        out.tracks.push_back(std::move(t));
    }

    out.fragments.reserve(parsed.fragments.size());
    for (const auto& f : parsed.fragments) {
        out.fragments.push_back(ConvertFragment(f));
    }

    Validate(out, options);
    return true;
}

Mp4SampleTableAnalyzer::~Mp4SampleTableAnalyzer() = default;

void Mp4SampleTableAnalyzer::Reset() {
    options_ = Mp4SampleTableOptions{};
}

}  // namespace analyzer
}  // namespace videoeye
