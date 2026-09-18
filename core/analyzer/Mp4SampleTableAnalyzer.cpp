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

#ifdef HAVE_BENTO4
#include "Ap4.h"
#include "Ap4Co64Atom.h"
#include "Ap4ElstAtom.h"
#include "Ap4FileByteStream.h"
#include "Ap4MfhdAtom.h"
#include "Ap4SidxAtom.h"
#include "Ap4StsdAtom.h"
#include "Ap4TfdtAtom.h"
#include "Ap4TfhdAtom.h"
#include "Ap4TrunAtom.h"
#endif

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

#ifdef HAVE_BENTO4

namespace {

std::string FourCCToString(AP4_UI32 type) {
    char buf[5] = {0, 0, 0, 0, 0};
    buf[0] = static_cast<char>((type >> 24) & 0xFF);
    buf[1] = static_cast<char>((type >> 16) & 0xFF);
    buf[2] = static_cast<char>((type >> 8) & 0xFF);
    buf[3] = static_cast<char>(type & 0xFF);
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c < 0x20 || c > 0x7E) buf[i] = '.';
    }
    return std::string(buf);
}

const char* TrackTypeName(AP4_Track::Type type) {
    switch (type) {
        case AP4_Track::TYPE_VIDEO:     return "video";
        case AP4_Track::TYPE_AUDIO:     return "audio";
        case AP4_Track::TYPE_HINT:      return "hint";
        case AP4_Track::TYPE_TEXT:      return "text";
        case AP4_Track::TYPE_JPEG:      return "jpeg";
        case AP4_Track::TYPE_RTP:       return "rtp";
        case AP4_Track::TYPE_SUBTITLES: return "subtitle";
        case AP4_Track::TYPE_SYSTEM:    return "system";
        default:                        return "unknown";
    }
}

// Bento4 没有暴露 stts/ctts 的 entries，用「探测最大可解析序号」得到表覆盖的样本数。
// 注意: AP4_SttsAtom::GetDts / AP4_CttsAtom::GetCtsOffset 的 sample 参数是 **1-based**
// （源码里 "sample indexes start at 1"，传 0 直接返回 OUT_OF_RANGE），所以这里
// 的 probe 也必须用 1-based 序号，否则第一个样本就会探测失败，得到"表覆盖 0 个样本"
// 的荒谬结果，进而误报 sample_count_mismatch。
// 先指数扩张找上界，再二分；两个接口内部都有顺序访问缓存，探测开销可忽略。
template <typename Probe>
uint32_t ProbeSampleCount(const Probe& ok) {  // ok(n): n 为 1-based 样本序号
    if (!ok(1)) return 0;
    uint64_t count = 1;
    uint64_t bound = 2;
    while (bound <= (1ull << 31) && ok(bound)) {
        count = bound;
        bound <<= 1;
    }
    if (bound > (1ull << 31)) return static_cast<uint32_t>(count);
    uint64_t lo = count, hi = bound;  // ok(lo)=true, ok(hi)=false
    while (lo + 1 < hi) {
        const uint64_t mid = (lo + hi) / 2;
        if (ok(mid)) lo = mid; else hi = mid;
    }
    return static_cast<uint32_t>(lo);
}

uint32_t ProbeSttsSampleCount(AP4_SttsAtom* atom) {
    if (!atom) return 0;
    return ProbeSampleCount([&](uint64_t n) {
        AP4_UI64 dts = 0;
        AP4_UI32 duration = 0;
        return AP4_SUCCEEDED(atom->GetDts(static_cast<AP4_Ordinal>(n), dts, &duration));
    });
}

uint32_t ProbeCttsSampleCount(AP4_CttsAtom* atom) {
    if (!atom) return 0;
    return ProbeSampleCount([&](uint64_t n) {
        AP4_UI32 offset = 0;
        return AP4_SUCCEEDED(atom->GetCtsOffset(static_cast<AP4_Ordinal>(n), offset));
    });
}

const AP4_UI32 kAtomTypeMfhd = AP4_ATOM_TYPE('m', 'f', 'h', 'd');
const AP4_UI32 kAtomTypeMoof = AP4_ATOM_TYPE('m', 'o', 'o', 'f');
const AP4_UI32 kAtomTypeTfdt = AP4_ATOM_TYPE('t', 'f', 'd', 't');
const AP4_UI32 kAtomTypeSidx = AP4_ATOM_TYPE('s', 'i', 'd', 'x');
const AP4_UI32 kAtomTypeStyp = AP4_ATOM_TYPE('s', 't', 'y', 'p');
const AP4_UI32 kAtomTypeMdat = AP4_ATOM_TYPE('m', 'd', 'a', 't');
const AP4_UI32 kAtomTypeMoov = AP4_ATOM_TYPE('m', 'o', 'o', 'v');

void CollectFragments(AP4_Atom* moof_atom, uint64_t moof_offset, uint64_t moof_size,
                      uint32_t moof_index, model::Mp4SampleTableResult& out) {
    AP4_ContainerAtom* moof = AP4_DYNAMIC_CAST(AP4_ContainerAtom, moof_atom);
    if (!moof) return;

    uint32_t sequence_number = 0;
    AP4_MfhdAtom* mfhd = AP4_DYNAMIC_CAST(AP4_MfhdAtom, moof->GetChild(kAtomTypeMfhd));
    if (mfhd) sequence_number = mfhd->GetSequenceNumber();

    for (AP4_List<AP4_Atom>::Item* item = moof->GetChildren().FirstItem(); item;
         item = item->GetNext()) {
        AP4_Atom* child = item->GetData();
        if (!child || child->GetType() != AP4_ATOM_TYPE_TRAF) continue;
        AP4_ContainerAtom* traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, child);
        if (!traf) continue;

        model::Mp4FragmentInfo f;
        f.moof_index = moof_index;
        f.index = static_cast<uint32_t>(out.fragments.size());
        f.sequence_number = sequence_number;
        f.offset = moof_offset;
        f.size = moof_size;

        AP4_TfhdAtom* tfhd = AP4_DYNAMIC_CAST(AP4_TfhdAtom, traf->GetChild(AP4_ATOM_TYPE_TFHD));
        uint32_t default_duration = 0;
        uint32_t default_size = 0;
        if (tfhd) {
            f.track_id = tfhd->GetTrackId();
            f.base_data_offset = tfhd->GetBaseDataOffset();
            const AP4_UI32 flags = tfhd->GetFlags();
            f.base_data_offset_present = (flags & AP4_TFHD_FLAG_BASE_DATA_OFFSET_PRESENT) != 0;
            f.default_base_is_moof = (flags & AP4_TFHD_FLAG_DEFAULT_BASE_IS_MOOF) != 0;
            f.sample_description_index_present =
                (flags & AP4_TFHD_FLAG_SAMPLE_DESCRIPTION_INDEX_PRESENT) != 0;
            f.default_sample_duration_present =
                (flags & AP4_TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT) != 0;
            f.default_sample_size_present =
                (flags & AP4_TFHD_FLAG_DEFAULT_SAMPLE_SIZE_PRESENT) != 0;
            f.duration_is_empty = (flags & AP4_TFHD_FLAG_DURATION_IS_EMPTY) != 0;
            default_duration = tfhd->GetDefaultSampleDuration();
            default_size = tfhd->GetDefaultSampleSize();
        }

        AP4_TfdtAtom* tfdt = AP4_DYNAMIC_CAST(AP4_TfdtAtom, traf->GetChild(kAtomTypeTfdt));
        if (tfdt) {
            f.has_tfdt = true;
            f.base_media_decode_time = tfdt->GetBaseMediaDecodeTime();
        }

        for (AP4_List<AP4_Atom>::Item* ti = traf->GetChildren().FirstItem(); ti; ti = ti->GetNext()) {
            AP4_Atom* tchild = ti->GetData();
            if (!tchild || tchild->GetType() != AP4_ATOM_TYPE_TRUN) continue;
            AP4_TrunAtom* trun = AP4_DYNAMIC_CAST(AP4_TrunAtom, tchild);
            if (!trun) continue;
            ++f.trun_count;
            const AP4_UI32 trun_flags = trun->GetFlags();
            if (f.trun_count == 1) {
                f.trun_data_offset_present = (trun_flags & AP4_TRUN_FLAG_DATA_OFFSET_PRESENT) != 0;
                f.trun_data_offset = trun->GetDataOffset();
            }
            const AP4_Array<AP4_TrunAtom::Entry>& entries = trun->GetEntries();
            for (unsigned int i = 0; i < entries.ItemCount(); ++i) {
                ++f.sample_count;
                f.duration += (trun_flags & AP4_TRUN_FLAG_SAMPLE_DURATION_PRESENT)
                                  ? entries[i].sample_duration
                                  : default_duration;
                f.total_size += (trun_flags & AP4_TRUN_FLAG_SAMPLE_SIZE_PRESENT)
                                    ? entries[i].sample_size
                                    : default_size;
            }
        }

        out.fragments.push_back(f);
    }
}

void CollectTrack(AP4_Track* track, const Mp4SampleTableOptions& options,
                  model::Mp4TrackSampleTable& t) {
    t.track_id = track->GetId();
    t.type = TrackTypeName(track->GetType());
    t.media_timescale = track->GetMediaTimeScale();
    t.media_duration = track->GetMediaDuration();

    AP4_TrakAtom* trak = track->UseTrakAtom();
    AP4_ContainerAtom* stbl = nullptr;
    if (trak) {
        AP4_Atom* stbl_atom = trak->FindChild("mdia/minf/stbl");
        stbl = AP4_DYNAMIC_CAST(AP4_ContainerAtom, stbl_atom);
    }

    AP4_StszAtom* stsz = nullptr;
    if (stbl) {
        stsz = AP4_DYNAMIC_CAST(AP4_StszAtom, stbl->GetChild(AP4_ATOM_TYPE_STSZ));
        AP4_StssAtom* stss = AP4_DYNAMIC_CAST(AP4_StssAtom, stbl->GetChild(AP4_ATOM_TYPE_STSS));
        AP4_SttsAtom* stts = AP4_DYNAMIC_CAST(AP4_SttsAtom, stbl->GetChild(AP4_ATOM_TYPE_STTS));
        AP4_CttsAtom* ctts = AP4_DYNAMIC_CAST(AP4_CttsAtom, stbl->GetChild(AP4_ATOM_TYPE('c', 't', 't', 's')));
        AP4_StscAtom* stsc = AP4_DYNAMIC_CAST(AP4_StscAtom, stbl->GetChild(AP4_ATOM_TYPE_STSC));
        AP4_StcoAtom* stco = AP4_DYNAMIC_CAST(AP4_StcoAtom, stbl->GetChild(AP4_ATOM_TYPE_STCO));
        AP4_Co64Atom* co64 = AP4_DYNAMIC_CAST(AP4_Co64Atom, stbl->GetChild(AP4_ATOM_TYPE_CO64));

        t.has_stsz = (stsz != nullptr);
        t.has_stz2 = (stbl->GetChild(AP4_ATOM_TYPE_STZ2) != nullptr);
        t.has_stss = (stss != nullptr);
        t.has_stts = (stts != nullptr);
        t.has_ctts = (ctts != nullptr);
        t.has_stsc = (stsc != nullptr);
        t.has_stco = (stco != nullptr);
        t.has_co64 = (co64 != nullptr);

        if (stsz) t.stsz_sample_count = stsz->GetSampleCount();
        if (stss) t.stss_sync_count = stss->GetEntries().ItemCount();
        t.stts_sample_count = ProbeSttsSampleCount(stts);
        t.ctts_sample_count = ProbeCttsSampleCount(ctts);

        if (stco) {
            t.chunk_count = stco->GetChunkCount();
            const AP4_UI32* offsets = stco->GetChunkOffsets();
            for (AP4_Cardinal i = 0; i < stco->GetChunkCount(); ++i) {
                t.max_chunk_offset = std::max<uint64_t>(t.max_chunk_offset, offsets[i]);
            }
        } else if (co64) {
            t.chunk_count = co64->GetChunkCount();
            const AP4_UI64* offsets = co64->GetChunkOffsets();
            for (AP4_Cardinal i = 0; i < co64->GetChunkCount(); ++i) {
                t.max_chunk_offset = std::max<uint64_t>(t.max_chunk_offset, offsets[i]);
            }
        }

        AP4_StsdAtom* stsd = AP4_DYNAMIC_CAST(AP4_StsdAtom, stbl->GetChild(AP4_ATOM_TYPE_STSD));
        if (stsd) {
            AP4_SampleDescription* sd = stsd->GetSampleDescription(0);
            if (sd) t.codec = FourCCToString(sd->GetFormat());
        }
    }

    if (trak) {
        AP4_ElstAtom* elst = AP4_DYNAMIC_CAST(AP4_ElstAtom, trak->FindChild("edts/elst"));
        if (elst) {
            t.has_elst = true;
            const AP4_Array<AP4_ElstEntry>& entries = elst->GetEntries();
            t.edit_list.reserve(entries.ItemCount());
            for (unsigned int i = 0; i < entries.ItemCount(); ++i) {
                model::Mp4EditListEntry e;
                e.segment_duration = entries[i].m_SegmentDuration;
                e.media_time = entries[i].m_MediaTime;
                e.media_rate = entries[i].m_MediaRate;
                e.is_empty_edit = (entries[i].m_MediaTime < 0);
                t.edit_list.push_back(e);
            }
        }
    }

    // 全部样本字节数（不截断，stsz 直接累加）
    if (stsz) {
        const uint32_t n = t.stsz_sample_count;
        uint64_t total = 0;
        for (uint32_t i = 0; i < n; ++i) {
            AP4_Size size = 0;
            if (AP4_SUCCEEDED(stsz->GetSampleSize(i, size))) total += size;
        }
        t.total_sample_bytes = total;
    }

    // 逐样本展开
    const uint32_t n = t.stsz_sample_count;
    const uint32_t limit = options.expand_samples
                               ? std::min<uint32_t>(n, options.max_samples_per_track)
                               : 0;
    if (limit > 0) {
        AP4_AtomSampleTable* atom_table =
            AP4_DYNAMIC_CAST(AP4_AtomSampleTable, track->GetSampleTable());
        t.samples.reserve(limit);
        for (uint32_t i = 0; i < limit; ++i) {
            AP4_Sample sample;
            if (AP4_FAILED(track->GetSample(i, sample))) {
                t.sample_read_failed = true;
                t.sample_read_error_index = i;
                break;
            }
            model::Mp4Sample s;
            s.index = i;
            s.offset = sample.GetOffset();
            s.size = sample.GetSize();
            s.dts = static_cast<int64_t>(sample.GetDts());
            s.cts_delta = sample.GetCtsDelta();
            s.cts = s.dts + static_cast<int64_t>(s.cts_delta);
            s.duration = sample.GetDuration();
            s.keyframe = sample.IsSync();
            s.description_index = sample.GetDescriptionIndex();
            if (atom_table) {
                AP4_Ordinal chunk = 0, pos_in_chunk = 0;
                if (AP4_SUCCEEDED(atom_table->GetSampleChunkPosition(i, chunk, pos_in_chunk))) {
                    s.chunk_index = static_cast<uint32_t>(chunk);
                    s.index_in_chunk = static_cast<uint32_t>(pos_in_chunk);
                }
            }
            t.samples.push_back(s);
        }
        t.samples_truncated = (t.samples.size() < n);
        if (!t.samples.empty()) t.first_sample_cts = t.samples.front().cts;
    }
}

}  // namespace

bool Mp4SampleTableAnalyzer::AnalyzeFile(const std::string& file_path,
                                         model::Mp4SampleTableResult& out,
                                         const Mp4SampleTableOptions& options) {
    out = model::Mp4SampleTableResult{};
    out.file_path = file_path;
    options_ = options;

    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(std::filesystem::path(file_path), ec);
    if (!ec) out.file_size = static_cast<uint64_t>(size);

    AP4_ByteStream* stream = nullptr;
    if (AP4_FAILED(AP4_FileByteStream::Create(file_path.c_str(),
                                              AP4_FileByteStream::STREAM_MODE_READ, stream))) {
        out.error_message = "无法打开文件（Bento4 打开失败）";
        LOG_WARN("Mp4SampleTableAnalyzer: " + out.error_message + " " + file_path);
        return false;
    }

    AP4_File* file = nullptr;
    try {
        file = new AP4_File(*stream, false);
    } catch (...) {
        file = nullptr;
    }
    if (!file) {
        out.error_message = "MP4 解析失败（不是有效的 ISOBMFF 文件？）";
        stream->Release();
        LOG_WARN("Mp4SampleTableAnalyzer: " + out.error_message + " " + file_path);
        return false;
    }

    out.moov_before_mdat = file->IsMoovBeforeMdat();
    AP4_Movie* movie = file->GetMovie();
    if (!movie) {
        // 没有 moov：可能是纯 fMP4 分片（缺 init 段）或损坏文件，仍继续扫顶层 box
        LOG_WARN("Mp4SampleTableAnalyzer: 未找到 moov，仅有分片或文件损坏");
    } else {
        out.movie_timescale = movie->GetTimeScale();
    }

    // ---- 顶层 box ----
    uint64_t cursor = 0;
    uint32_t moof_index = 0;
    for (AP4_List<AP4_Atom>::Item* item = file->GetChildren().FirstItem(); item;
         item = item->GetNext()) {
        AP4_Atom* atom = item->GetData();
        if (!atom) continue;
        const AP4_UI32 type = atom->GetType();
        const uint64_t atom_size = atom->GetSize();
        const std::string type_name = FourCCToString(type);
        out.top_level_order.push_back(type_name);

        if (type == kAtomTypeMoov && out.moov_size == 0) {
            out.moov_offset = cursor;
            out.moov_size = atom_size;
        } else if (type == kAtomTypeMdat && out.first_mdat_size == 0) {
            out.first_mdat_offset = cursor;
            out.first_mdat_size = atom_size;
        } else if (type == kAtomTypeSidx) {
            ++out.sidx_count;
        } else if (type == kAtomTypeStyp) {
            ++out.styp_count;
        } else if (type == kAtomTypeMoof) {
            out.fragmented = true;
            CollectFragments(atom, cursor, atom_size, moof_index, out);
            ++moof_index;
        }
        cursor += atom_size;
    }

    // ---- 轨道样本表 ----
    if (movie) {
        for (AP4_List<AP4_Track>::Item* item = movie->GetTracks().FirstItem(); item;
             item = item->GetNext()) {
            AP4_Track* track = item->GetData();
            if (!track) continue;
            model::Mp4TrackSampleTable t;
            CollectTrack(track, options, t);
            out.tracks.push_back(std::move(t));
        }
    }

    delete file;
    stream->Release();

    out.valid = true;
    Validate(out, options);

    LOG_INFO("MP4 样本表分析: tracks=" + std::to_string(out.tracks.size()) +
             " fragments=" + std::to_string(out.fragments.size()) +
             " issues=" + std::to_string(out.issues.size()) +
             " faststart=" + (out.IsFastStart() ? "yes" : "no"));
    return true;
}

#else  // !HAVE_BENTO4

bool Mp4SampleTableAnalyzer::AnalyzeFile(const std::string& file_path,
                                         model::Mp4SampleTableResult& out,
                                         const Mp4SampleTableOptions& options) {
    out = model::Mp4SampleTableResult{};
    out.file_path = file_path;
    out.error_message = "Bento4 库未链接，无法做 MP4 样本表分析";
    LOG_WARN(out.error_message);
    (void)options;
    return false;
}

#endif  // HAVE_BENTO4

Mp4SampleTableAnalyzer::Mp4SampleTableAnalyzer() = default;

Mp4SampleTableAnalyzer::~Mp4SampleTableAnalyzer() = default;

void Mp4SampleTableAnalyzer::Reset() {
    options_ = Mp4SampleTableOptions{};
}

}  // namespace analyzer
}  // namespace videoeye
