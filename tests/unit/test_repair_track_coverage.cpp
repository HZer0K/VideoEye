#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/RecoveredSample.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairSampleScan.h"

namespace {

// R3：tests/unit 下不得声明 videoeye::repair 具名子命名空间，用 using 引入被测符号。
using namespace videoeye::repair;
namespace model = videoeye::model;

model::RecoveredSample MakeSample(int stream_index, double pts_seconds) {
    model::RecoveredSample sample;
    sample.stream_index = stream_index;
    sample.track_id = static_cast<uint32_t>(stream_index + 1);
    sample.size = 512;
    sample.pts_seconds = pts_seconds;
    sample.dts_seconds = pts_seconds;
    sample.timestamp_evidence = model::SampleTimestampEvidence::ContainerBox;
    sample.confidence = model::SampleConfidence::Probable;
    return sample;
}

// 恒定间隔的样本序列（不含孔洞）。skip(from,to) 用来挖一个缺口。
void AddConstantRate(RepairSampleLedger* ledger, int stream_index, double start, double end, double step,
                     double skip_from = -1.0, double skip_to = -1.0) {
    for (double t = start; t <= end + 1e-9; t += step) {
        if (skip_from >= 0.0 && t > skip_from + 1e-9 && t < skip_to - 1e-9)
            continue;
        ledger->Add(MakeSample(stream_index, t));
    }
}

const model::RepairTrackCoverage* FindTrack(const model::RepairTrackCoverageSummary& coverage, int stream_index) {
    for (const auto& track : coverage.tracks) {
        if (track.output_stream_index == stream_index)
            return &track;
    }
    return nullptr;
}

// 逐轨缺口是「哪条轨缺了哪一段」的唯一出处：全局 lost_ranges 只有一段秒数，
// 说不了「音频其实全程连续」。
TEST(RepairTrackCoverage, ReportsGapOnlyOnTheTrackThatLostContent) {
    RepairSampleLedger ledger;
    AddConstantRate(&ledger, 0, 0.0, 10.0, 1.0 / 30.0, 4.0, 6.0); // 视频 30 fps，4–6 s 缺失
    AddConstantRate(&ledger, 1, 0.0, 10.0, 0.02);                 // 音频全程连续

    const auto coverage = ledger.Coverage();
    ASSERT_TRUE(coverage.available);

    const auto* video = FindTrack(coverage, 0);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->gap_count, 1u);
    ASSERT_EQ(video->gaps.size(), 1u);
    EXPECT_NEAR(video->gaps.front().start_seconds, 4.0, 0.05);
    EXPECT_NEAR(video->gaps.front().end_seconds, 6.0, 0.05);
    EXPECT_NEAR(video->gap_seconds, 2.0, 0.1);
    EXPECT_FALSE(video->gaps_truncated);

    const auto* audio = FindTrack(coverage, 1);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->gap_count, 0u) << "音频没有缺内容，不能跟着视频一起报损失";
    EXPECT_TRUE(audio->gaps.empty());

    EXPECT_TRUE(coverage.common_span_known);
    EXPECT_NEAR(coverage.common_start_seconds, 0.0, 0.05);
    EXPECT_NEAR(coverage.common_end_seconds, 10.0, 0.05);
}

// 判据必须带相对项：10 s 一帧的延时摄影，「间隔 10 s」是正常的，不是丢失。
TEST(RepairTrackCoverage, LowFrameRateIsNotTreatedAsLoss) {
    RepairSampleLedger ledger;
    AddConstantRate(&ledger, 0, 0.0, 190.0, 10.0); // 20 帧，每帧间隔 10 s

    const auto coverage = ledger.Coverage();
    ASSERT_TRUE(coverage.available);
    const auto* track = FindTrack(coverage, 0);
    ASSERT_NE(track, nullptr);
    EXPECT_GT(track->typical_interval_seconds, 5.0) << "典型间隔应估到 10 s 量级";
    EXPECT_EQ(track->gap_count, 0u) << "正常的长间隔不能被报成缺口";
}

// 阈值下限：30 fps 下丢十几帧（0.4 s）不值得报成「损失区间」。
TEST(RepairTrackCoverage, SubSecondHoleIsNotReported) {
    RepairSampleLedger ledger;
    AddConstantRate(&ledger, 0, 0.0, 5.0, 1.0 / 30.0, 4.0, 4.4);

    const auto coverage = ledger.Coverage();
    const auto* track = FindTrack(coverage, 0);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->gap_count, 0u);
}

// B 帧重排会让 PTS 短暂回退；时间戳整体回退（分段重置）也是「重新开始」而不是丢失。
// 两种都不能算成缺口 —— 用「已见最大值」而不是 abs(差) 做基准就是为了这个。
TEST(RepairTrackCoverage, ReorderedOrResetTimestampsDoNotCreateGaps) {
    RepairSampleLedger ledger;
    const double reordered[] = {0.0, 0.133, 0.033, 0.066, 0.2, 0.333, 0.233, 0.266, 0.4};
    for (const double pts : reordered)
        ledger.Add(MakeSample(0, pts));
    for (double t = 0.4333; t <= 1.0 + 1e-9; t += 1.0 / 30.0)
        ledger.Add(MakeSample(0, t));
    // 时间戳重置：从 1.0 s 回到 0.05 s 重新开始（分段录制 / 时间基重置）。
    for (const double pts : {0.05, 0.0833, 0.1167, 0.15})
        ledger.Add(MakeSample(0, pts));

    const auto coverage = ledger.Coverage();
    const auto* track = FindTrack(coverage, 0);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->gap_count, 0u) << "重排回退与时间戳重置都不是丢内容";
    EXPECT_DOUBLE_EQ(track->first_seconds, 0.0);
    EXPECT_NEAR(track->last_seconds, 1.0, 0.01);
}

// 明细有界，但计数必须全量精确 —— 这正是不许在明细缓存上算区间的原因。
TEST(RepairTrackCoverage, GapCountsStayExactWhenDetailsTruncate) {
    RepairSampleLedger ledger;
    constexpr int kGapCount = 200;
    for (int i = 0; i < kGapCount; ++i) {
        const double base = i * 10.0;
        ledger.Add(MakeSample(0, base));
        ledger.Add(MakeSample(0, base + 0.033));
        ledger.Add(MakeSample(0, base + 0.066));
    }

    const auto coverage = ledger.Coverage();
    const auto* track = FindTrack(coverage, 0);
    ASSERT_NE(track, nullptr);
    // 每组的最后一个样本到下一组第一个样本之间才有缺口；首个样本不产生间隔，
    // 所以是 kGapCount - 1 段。
    EXPECT_EQ(track->gap_count, static_cast<uint64_t>(kGapCount) - 1);
    EXPECT_TRUE(track->gaps_truncated);
    EXPECT_EQ(track->gaps.size(), TrackCoverageStats::kMaxGapDetails);
    EXPECT_GT(track->gap_seconds, 1000.0);
    ASSERT_FALSE(coverage.limitations.empty()) << "明细截断必须写进限制项";
}

// 共同覆盖区间是交集：音画起点不同时，不能拿最长的那条轨当「同时在播」。
TEST(RepairTrackCoverage, CommonSpanIsTheIntersectionOfTrackSpans) {
    RepairSampleLedger ledger;
    AddConstantRate(&ledger, 0, 0.0, 10.0, 1.0 / 30.0);
    AddConstantRate(&ledger, 1, 1.0, 9.0, 0.02);

    const auto coverage = ledger.Coverage();
    ASSERT_TRUE(coverage.common_span_known);
    EXPECT_NEAR(coverage.common_start_seconds, 1.0, 0.05);
    EXPECT_NEAR(coverage.common_end_seconds, 9.0, 0.05);
}

TEST(RepairTrackCoverage, CommonSpanUnknownWhenTracksDoNotOverlap) {
    RepairSampleLedger ledger;
    AddConstantRate(&ledger, 0, 0.0, 5.0, 1.0 / 30.0);
    AddConstantRate(&ledger, 1, 8.0, 12.0, 0.02);

    const auto coverage = ledger.Coverage();
    EXPECT_FALSE(coverage.common_span_known);
    EXPECT_FALSE(coverage.common_span_note.empty()) << "未知原因必须写出来";
    // 逐轨数据本身仍然有效：交集为空不等于覆盖统计不可用。
    EXPECT_TRUE(coverage.available);
    const auto* video = FindTrack(coverage, 0);
    ASSERT_NE(video, nullptr);
    EXPECT_GT(video->samples, 0u);
}

// 没有时间戳的样本既不算覆盖也不算缺口，必须单独计数并写进报告。
TEST(RepairTrackCoverage, SamplesWithoutTimestampAreCountedSeparately) {
    RepairSampleLedger ledger;
    AddConstantRate(&ledger, 0, 0.0, 1.0, 1.0 / 30.0);
    for (int i = 0; i < 5; ++i) {
        auto sample = MakeSample(0, 0.0);
        sample.pts_seconds.reset();
        ledger.Add(sample);
    }

    const auto coverage = ledger.Coverage();
    const auto* track = FindTrack(coverage, 0);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->samples_without_timestamp, 5u);
    EXPECT_EQ(track->samples, 36u);
    EXPECT_DOUBLE_EQ(track->first_seconds, 0.0);
}

TEST(RepairTrackCoverage, EmptyLedgerIsUnavailableWithReason) {
    RepairSampleLedger ledger;
    const auto coverage = ledger.Coverage();
    EXPECT_FALSE(coverage.available);
    EXPECT_FALSE(coverage.unavailable_reason.empty());
    EXPECT_FALSE(coverage.complete) << "complete 只能由调用方按是否走到源末尾填，默认不成立";
}

} // namespace
