// Mp4SampleTableAnalyzer 单元测试
//
// 只测校验逻辑（Validate）：手工构造合成样本表，不依赖 Bento4 / Qt / 真实文件。
// 解析部分（Bento4 读 stbl / moof）靠实机打开验证，见 docs/MP4_SAMPLE_TABLE.md。

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "core/analyzer/Mp4SampleTableAnalyzer.h"

using namespace videoeye;

namespace {

using analyzer::Mp4SampleTableAnalyzer;
using analyzer::Mp4SampleTableOptions;
using model::IssueSeverity;
using model::Mp4ConsistencyIssue;
using model::Mp4Sample;
using model::Mp4SampleTableResult;
using model::Mp4TrackSampleTable;

Mp4SampleTableResult MakeResult(uint64_t file_size, bool moov_before_mdat) {
    Mp4SampleTableResult r;
    r.valid = true;
    r.file_size = file_size;
    r.moov_before_mdat = moov_before_mdat;
    r.moov_offset = 1000;
    r.moov_size = 4096;
    r.first_mdat_offset = 100;
    r.first_mdat_size = 900;
    r.movie_timescale = 1000;
    return r;
}

Mp4TrackSampleTable MakeTrack(uint32_t track_id, const std::string& type,
                              uint32_t timescale = 1000) {
    Mp4TrackSampleTable t;
    t.track_id = track_id;
    t.type = type;
    t.media_timescale = timescale;
    t.media_duration = 0;
    t.has_stts = true;
    t.has_stsz = true;
    t.has_stsc = true;
    t.has_stco = true;
    return t;
}

// 追加 n 个等长样本，offset 依次递增
void AppendSamples(Mp4TrackSampleTable& t, uint32_t n, uint32_t size,
                   uint64_t start_offset, uint32_t duration = 100) {
    for (uint32_t i = 0; i < n; ++i) {
        Mp4Sample s;
        s.index = i;
        s.offset = start_offset + static_cast<uint64_t>(i) * size;
        s.size = size;
        s.duration = duration;
        s.dts = static_cast<int64_t>(i) * duration;
        s.cts = s.dts;
        s.keyframe = (i == 0);
        s.chunk_index = 0;
        s.index_in_chunk = i;
        t.samples.push_back(s);
    }
    t.stsz_sample_count = n;
    t.stts_sample_count = n;
    t.stss_sync_count = 1;
    t.has_stss = true;
    t.chunk_count = 1;
    t.max_chunk_offset = start_offset;
    t.total_sample_bytes = static_cast<uint64_t>(n) * size;
}

bool HasCode(const Mp4SampleTableResult& r, const std::string& code) {
    return r.HasIssue(code);
}

const Mp4ConsistencyIssue* FindCode(const Mp4SampleTableResult& r, const std::string& code) {
    return r.FindIssue(code);
}

}  // namespace

// ---- faststart ----

TEST(Mp4SampleTable, FastStartFileHasNoLayoutIssue) {
    Mp4SampleTableResult r = MakeResult(100000, /*moov_before_mdat=*/true);
    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(r.IsFastStart());
    EXPECT_FALSE(HasCode(r, model::Mp4IssueCode::kNotFastStart));
}

TEST(Mp4SampleTable, NonFastStartRaisesFirstScreenHint) {
    Mp4SampleTableResult r = MakeResult(100000, /*moov_before_mdat=*/false);
    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_FALSE(r.IsFastStart());
    const Mp4ConsistencyIssue* issue = FindCode(r, model::Mp4IssueCode::kNotFastStart);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Warning);
    // 提示里要能看出 moov / mdat 的具体位置，方便用户判断是不是真的没 faststart
    EXPECT_NE(issue->detail.find("moov"), std::string::npos);
    EXPECT_NE(issue->suggestion.find("faststart"), std::string::npos);
}

TEST(Mp4SampleTable, FragmentedFileCountsAsFastStart) {
    Mp4SampleTableResult r = MakeResult(100000, /*moov_before_mdat=*/false);
    r.fragmented = true;
    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(r.IsFastStart());
    EXPECT_FALSE(HasCode(r, model::Mp4IssueCode::kNotFastStart));
}

// ---- 表间一致性 ----

TEST(Mp4SampleTable, SampleCountMismatchIsError) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 10, 100, 0);
    t.stsz_sample_count = 10;
    t.stts_sample_count = 8;  // stts 只覆盖 8 个
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    const Mp4ConsistencyIssue* issue =
        FindCode(r, model::Mp4IssueCode::kSampleCountMismatch);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Error);
    EXPECT_EQ(issue->track_id, 1);
}

TEST(Mp4SampleTable, ConsistentTablesProduceNoCountIssue) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 10, 100, 0);
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_FALSE(HasCode(r, model::Mp4IssueCode::kSampleCountMismatch));
    EXPECT_FALSE(HasCode(r, model::Mp4IssueCode::kMissingChunkOffsets));
    EXPECT_TRUE(r.issues.empty());
}

TEST(Mp4SampleTable, MissingChunkOffsetsIsError) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    t.stsz_sample_count = 10;
    t.stts_sample_count = 10;
    t.chunk_count = 0;
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(HasCode(r, model::Mp4IssueCode::kMissingChunkOffsets));
}

TEST(Mp4SampleTable, StcoOverflowRequiresCo64) {
    Mp4SampleTableResult r = MakeResult(0x200000000ull, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 4, 100, 0);
    t.max_chunk_offset = 0x1'0000'0100ull;  // > 4 GB
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(HasCode(r, model::Mp4IssueCode::kStcoOverflow));
}

TEST(Mp4SampleTable, StcoAndCo64ConflictIsError) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 4, 100, 0);
    t.has_co64 = true;
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(HasCode(r, model::Mp4IssueCode::kBothStcoAndCo64));
}

// ---- 样本级 ----

TEST(Mp4SampleTable, OffsetBeyondFileSizeIsError) {
    Mp4SampleTableResult r = MakeResult(/*file_size=*/1000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 10, 100, 0);  // 恰好占满 0..999，不越界
    r.tracks.push_back(t);
    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_FALSE(HasCode(r, model::Mp4IssueCode::kChunkOffsetOutOfRange));

    // 人为破坏最后一个样本的偏移（模拟 stco 被写坏 / 文件被截断）
    Mp4SampleTableResult broken = MakeResult(1000, true);
    Mp4TrackSampleTable bt = MakeTrack(1, "video");
    AppendSamples(bt, 10, 100, 0);
    bt.samples.back().offset = 950;  // 950 + 100 = 1050 > 1000
    broken.tracks.push_back(bt);
    Mp4SampleTableAnalyzer::Validate(broken);

    const Mp4ConsistencyIssue* issue =
        FindCode(broken, model::Mp4IssueCode::kChunkOffsetOutOfRange);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Error);
    // flags 打在结果里的样本上（Validate 改的是 broken.tracks 里的副本，不是局部变量 bt）
    EXPECT_TRUE(
        broken.tracks[0].samples.back().HasFlag(model::Mp4SampleFlags::kOffsetOutOfRange));
    EXPECT_TRUE(issue->has_sample_index);
    EXPECT_EQ(issue->sample_index, 9u);
}

TEST(Mp4SampleTable, NegativeCompositionTimeIsWarning) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video", /*timescale=*/1000);
    AppendSamples(t, 5, 100, 0);
    t.samples[2].cts = -150;  // -150 ms
    t.samples[2].cts_delta = -150;
    t.has_ctts = true;
    t.ctts_sample_count = 5;
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    const Mp4ConsistencyIssue* issue = FindCode(r, model::Mp4IssueCode::kNegativeCts);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Warning);
    EXPECT_EQ(issue->occurrence_count, 1);
    EXPECT_TRUE(r.tracks[0].samples[2].HasFlag(model::Mp4SampleFlags::kNegativeCts));
    EXPECT_FALSE(r.tracks[0].samples[1].IsAnomalous());
}

TEST(Mp4SampleTable, DtsRegressionIsError) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 5, 100, 0);
    t.samples[3].dts = 50;  // 比前一帧小
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    const Mp4ConsistencyIssue* issue = FindCode(r, model::Mp4IssueCode::kDtsNotMonotonic);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Error);
    EXPECT_TRUE(r.tracks[0].samples[3].HasFlag(model::Mp4SampleFlags::kDtsNotMonotonic));
}

TEST(Mp4SampleTable, ChunkInternalOffsetGapIsWarning) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 5, 100, 0);
    t.samples[2].offset += 8;  // 与上一帧结束位置之间多了 8 字节空洞
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(HasCode(r, model::Mp4IssueCode::kOffsetDiscontinuity));
    EXPECT_TRUE(r.tracks[0].samples[2].HasFlag(model::Mp4SampleFlags::kChunkDiscontinuity));
}

TEST(Mp4SampleTable, FirstSampleNotSyncIsWarning) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 5, 100, 0);
    t.samples.front().keyframe = false;
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(HasCode(r, model::Mp4IssueCode::kFirstSampleNotSync));
}

// ---- edit list ----

TEST(Mp4SampleTable, ElstFirstFrameShiftRespectsThreshold) {
    Mp4SampleTableOptions opt;
    opt.elst_shift_tolerance_ms = 33.0;

    // 首帧被跳过 500 ms（timescale=1000 → media_time=500）
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "video", 1000);
    AppendSamples(t, 5, 100, 0);
    t.has_elst = true;
    model::Mp4EditListEntry e;
    e.segment_duration = 5000;
    e.media_time = 500;
    t.edit_list.push_back(e);
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r, opt);
    const Mp4ConsistencyIssue* issue =
        FindCode(r, model::Mp4IssueCode::kElstFirstFrameShift);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Warning);
    EXPECT_GT(issue->metric_value, opt.elst_shift_tolerance_ms);
    EXPECT_DOUBLE_EQ(r.tracks[0].composition_delay_ms, 500.0);

    // 仅 10 ms：低于阈值，不报
    Mp4SampleTableResult small = MakeResult(100000, true);
    Mp4TrackSampleTable st = MakeTrack(1, "video", 1000);
    AppendSamples(st, 5, 100, 0);
    st.has_elst = true;
    model::Mp4EditListEntry se;
    se.media_time = 10;
    st.edit_list.push_back(se);
    small.tracks.push_back(st);
    Mp4SampleTableAnalyzer::Validate(small, opt);
    EXPECT_FALSE(HasCode(small, model::Mp4IssueCode::kElstFirstFrameShift));
}

TEST(Mp4SampleTable, EmptyEditIsInfoOnly) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable t = MakeTrack(1, "audio", 48000);
    AppendSamples(t, 5, 100, 0);
    t.has_elst = true;
    model::Mp4EditListEntry e;
    e.media_time = -1;
    e.is_empty_edit = true;
    t.edit_list.push_back(e);
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    const Mp4ConsistencyIssue* issue = FindCode(r, model::Mp4IssueCode::kElstEmptyEdit);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Info);
}

TEST(Mp4SampleTable, AvStartMismatchIsWarning) {
    Mp4SampleTableOptions opt;
    opt.av_start_tolerance_ms = 40.0;

    Mp4SampleTableResult r = MakeResult(100000, true);
    Mp4TrackSampleTable v = MakeTrack(1, "video", 1000);
    AppendSamples(v, 5, 100, 0);
    Mp4TrackSampleTable a = MakeTrack(2, "audio", 48000);
    AppendSamples(a, 5, 100, 100000);
    // 视频首帧晚了 500 ms
    for (auto& s : v.samples) s.cts += 500;
    v.samples.front().cts = 500;
    r.tracks.push_back(v);
    r.tracks.push_back(a);

    Mp4SampleTableAnalyzer::Validate(r, opt);
    const Mp4ConsistencyIssue* issue = FindCode(r, model::Mp4IssueCode::kAvStartMismatch);
    ASSERT_NE(issue, nullptr);
    EXPECT_NEAR(issue->metric_value, 500.0, 1.0);
    EXPECT_EQ(issue->severity, IssueSeverity::Warning);
}

// ---- fMP4 分片 ----

TEST(Mp4SampleTable, FragmentSequenceJumpIsWarning) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    r.fragmented = true;
    for (uint32_t i = 0; i < 3; ++i) {
        model::Mp4FragmentInfo f;
        f.moof_index = i;
        f.index = i;
        f.sequence_number = (i == 2) ? 4 : (i + 1);  // 1, 2, 4
        f.track_id = 1;
        f.has_tfdt = true;
        f.base_media_decode_time = i * 1000;
        f.duration = 1000;
        f.sample_count = 10;
        f.base_data_offset_present = true;
        r.fragments.push_back(f);
    }

    Mp4SampleTableAnalyzer::Validate(r);
    const Mp4ConsistencyIssue* issue = FindCode(r, model::Mp4IssueCode::kFragmentSequenceGap);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Warning);
    EXPECT_EQ(issue->occurrence_count, 1);
}

TEST(Mp4SampleTable, FragmentSequenceBackwardsIsError) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    r.fragmented = true;
    const uint32_t seqs[3] = {1, 3, 2};
    for (uint32_t i = 0; i < 3; ++i) {
        model::Mp4FragmentInfo f;
        f.moof_index = i;
        f.index = i;
        f.sequence_number = seqs[i];
        f.track_id = 1;
        f.has_tfdt = true;
        f.base_media_decode_time = i * 1000;
        f.duration = 1000;
        f.base_data_offset_present = true;
        r.fragments.push_back(f);
    }

    Mp4SampleTableAnalyzer::Validate(r);
    const Mp4ConsistencyIssue* issue = FindCode(r, model::Mp4IssueCode::kFragmentSequenceGap);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->severity, IssueSeverity::Error);
}

TEST(Mp4SampleTable, FragmentDecodeTimeGapIsWarning) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    r.fragmented = true;
    const uint64_t tfdt[3] = {0, 1000, 3000};  // 第二段应始于 2000，实际 3000
    for (uint32_t i = 0; i < 3; ++i) {
        model::Mp4FragmentInfo f;
        f.moof_index = i;
        f.index = i;
        f.sequence_number = i + 1;
        f.track_id = 1;
        f.has_tfdt = true;
        f.base_media_decode_time = tfdt[i];
        f.duration = 1000;
        f.base_data_offset_present = true;
        r.fragments.push_back(f);
    }

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(HasCode(r, model::Mp4IssueCode::kFragmentTimeGap));
    EXPECT_FALSE(HasCode(r, model::Mp4IssueCode::kFragmentSequenceGap));
}

TEST(Mp4SampleTable, FragmentWithoutDataOffsetIsError) {
    Mp4SampleTableResult r = MakeResult(100000, true);
    r.fragmented = true;
    model::Mp4FragmentInfo f;
    f.moof_index = 0;
    f.sequence_number = 1;
    f.track_id = 1;
    f.has_tfdt = true;
    f.sample_count = 10;
    r.fragments.push_back(f);

    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_TRUE(HasCode(r, model::Mp4IssueCode::kFragmentDataOffset));
}

// ---- 幂等性 ----

TEST(Mp4SampleTable, ValidateIsIdempotent) {
    Mp4SampleTableResult r = MakeResult(1000, false);
    Mp4TrackSampleTable t = MakeTrack(1, "video");
    AppendSamples(t, 10, 100, 0);
    t.samples.back().offset = 950;
    r.tracks.push_back(t);

    Mp4SampleTableAnalyzer::Validate(r);
    const size_t first = r.issues.size();
    ASSERT_GT(first, 0u);
    Mp4SampleTableAnalyzer::Validate(r);
    EXPECT_EQ(r.issues.size(), first);
    EXPECT_EQ(r.CountIssues(IssueSeverity::Error), 1);
}
