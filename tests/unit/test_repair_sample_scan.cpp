#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/domain/model/RecoveredSample.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairSampleScan.h"

namespace {

// R3：tests/unit 下不得声明 videoeye::repair 具名子命名空间，
// 用 using 引入被测符号，测试辅助一律留在匿名命名空间。
using namespace videoeye::repair;
namespace model = videoeye::model;

namespace fs = std::filesystem;

struct TempFile {
    fs::path path;

    TempFile() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path dir = fs::temp_directory_path() / ("videoeye_sample_scan_" + std::to_string(stamp));
        fs::create_directories(dir);
        path = dir / "window.bin";
    }

    ~TempFile() {
        std::error_code ec;
        fs::remove_all(path.parent_path(), ec);
    }

    void Write(const std::vector<uint8_t>& bytes) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
};

std::vector<uint8_t> MakeBytes(std::size_t count) {
    std::vector<uint8_t> bytes(count);
    uint32_t state = 0x12345678u;
    for (auto& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<uint8_t>((state >> 16) & 0xFF);
    }
    return bytes;
}

model::RecoveredSample MakeSample(int stream_index, uint32_t segment_index, uint64_t size,
                                  model::SampleConfidence confidence = model::SampleConfidence::Probable) {
    model::RecoveredSample sample;
    sample.source_offset = 1024 + stream_index * 10000 + segment_index * 100;
    sample.size = size;
    sample.track_id = static_cast<uint32_t>(stream_index + 1);
    sample.stream_index = stream_index;
    sample.segment_index = segment_index;
    sample.decode_ordinal = segment_index;
    sample.timestamp_evidence = model::SampleTimestampEvidence::ContainerBox;
    sample.confidence = confidence;
    return sample;
}

// ---------------- 分块扫描 ----------------

TEST(RepairSampleScan, ChunkedScanMatchesWholeWindowForEveryChunkSize) {
    TempFile temp;
    const std::vector<uint8_t> bytes = MakeBytes(20003); // 故意不是块大小的整数倍
    temp.Write(bytes);

    ScanWindow window;
    window.file_offset = 0;
    window.size = bytes.size();
    window.track_id = 1;
    window.stream_index = 0;

    MemoryByteSource whole(bytes, window);
    std::vector<uint8_t> expected(bytes.size());
    ASSERT_TRUE(whole.Read(0, expected.data(), expected.size())) << whole.Error();

    for (const std::size_t chunk :
         {std::size_t{7}, std::size_t{13}, std::size_t{64}, std::size_t{4096}, std::size_t{65536}}) {
        ChunkedByteSource chunked(temp.path.string(), window, chunk, nullptr, nullptr);
        std::vector<uint8_t> actual(bytes.size());
        ASSERT_TRUE(chunked.Read(0, actual.data(), actual.size())) << "chunk=" << chunk << " " << chunked.Error();
        EXPECT_EQ(actual, expected) << "chunk=" << chunk;
        if (chunk < bytes.size())
            EXPECT_GT(chunked.ChunksLoaded(), 1u) << "chunk=" << chunk;
        // 有界：单块不许把整个窗口常驻。
        EXPECT_LE(chunked.BytesFetched(), bytes.size() + chunk * 2) << "chunk=" << chunk;
    }
}

TEST(RepairSampleScan, ChunkedScanReadsArbitraryCrossBlockRanges) {
    TempFile temp;
    const std::vector<uint8_t> bytes = MakeBytes(4096);
    temp.Write(bytes);
    ScanWindow window;
    window.size = bytes.size();

    ChunkedByteSource chunked(temp.path.string(), window, 61, nullptr, nullptr);
    for (const uint64_t offset :
         {uint64_t{0}, uint64_t{59}, uint64_t{60}, uint64_t{61}, uint64_t{120}, uint64_t{4000}}) {
        const std::size_t len = 37;
        ASSERT_LE(offset + len, bytes.size());
        std::vector<uint8_t> actual(len);
        ASSERT_TRUE(chunked.Read(offset, actual.data(), len)) << offset << " " << chunked.Error();
        EXPECT_TRUE(std::equal(actual.begin(), actual.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)))
            << "offset=" << offset;
    }
    EXPECT_TRUE(chunked.Error().empty());
}

TEST(RepairSampleScan, OutOfWindowOrOversizedReadIsRejected) {
    TempFile temp;
    const std::vector<uint8_t> bytes = MakeBytes(512);
    temp.Write(bytes);
    ScanWindow window;
    window.size = bytes.size();

    ChunkedByteSource chunked(temp.path.string(), window, 32, nullptr, nullptr, 64);
    std::vector<uint8_t> buffer(512);
    EXPECT_FALSE(chunked.Read(500, buffer.data(), 37));
    EXPECT_FALSE(chunked.Error().empty());

    ChunkedByteSource oversized(temp.path.string(), window, 32, nullptr, nullptr, 64);
    EXPECT_FALSE(oversized.Read(0, buffer.data(), 128));
    EXPECT_FALSE(oversized.Error().empty());
}

TEST(RepairSampleScan, CancelStopsScanAtTheVeryNextBlock) {
    TempFile temp;
    const std::vector<uint8_t> bytes = MakeBytes(4096);
    temp.Write(bytes);
    ScanWindow window;
    window.size = bytes.size();

    std::atomic<bool> cancel{false};
    RepairRunBudget budget(&cancel, 0);
    ChunkedByteSource chunked(temp.path.string(), window, 64, &budget, &cancel);
    std::vector<uint8_t> buffer(64);
    ASSERT_TRUE(chunked.Read(0, buffer.data(), buffer.size())) << chunked.Error();
    const uint64_t chunks_before = chunked.ChunksLoaded();

    cancel.store(true);
    EXPECT_FALSE(chunked.Read(4096 - 64, buffer.data(), buffer.size()));
    EXPECT_NE(chunked.Error().find("被取消"), std::string::npos) << chunked.Error();
    EXPECT_EQ(chunked.ChunksLoaded(), chunks_before);

    // 预算对象自己带取消标志时，不传独立 cancel 也必须停。
    std::atomic<bool> pre_cancelled{true};
    RepairRunBudget stopped(&pre_cancelled, 0);
    ChunkedByteSource never_started(temp.path.string(), window, 64, &stopped, nullptr);
    EXPECT_FALSE(never_started.Read(0, buffer.data(), buffer.size()));
    EXPECT_NE(never_started.Error().find("被取消"), std::string::npos) << never_started.Error();
}

// ---------------- 有界账本 ----------------

TEST(RepairSampleScan, LedgerAggregatesExactlyWhenDetailIsTruncatedByCount) {
    RepairSampleLedger::Limits limits;
    limits.max_detail_samples = 3;
    limits.max_detail_bytes = 1024 * 1024;
    RepairSampleLedger ledger(limits);

    for (uint32_t i = 0; i < 10; ++i)
        ledger.Add(MakeSample(static_cast<int>(i % 2), i % 3, 100));

    EXPECT_EQ(ledger.Total(), 10u);
    EXPECT_EQ(ledger.CountForTrack(0), 5u);
    EXPECT_EQ(ledger.CountForTrack(1), 5u);
    EXPECT_EQ(ledger.CountForSegment(0), 4u);
    EXPECT_EQ(ledger.CountForSegment(1), 3u);
    EXPECT_EQ(ledger.CountForSegment(2), 3u);
    EXPECT_EQ(ledger.Samples().size(), 3u);
    EXPECT_EQ(ledger.DroppedDetail(), 7u);
    EXPECT_TRUE(ledger.DetailTruncated());
    ASSERT_FALSE(ledger.Limitations().empty());
    EXPECT_NE(ledger.Limitations().front().find("聚合计数仍为全量实测值"), std::string::npos);

    const auto summary = ledger.Summary();
    EXPECT_TRUE(summary.available);
    EXPECT_EQ(summary.total_samples, 10u);
    EXPECT_EQ(summary.retained_detail, 3u);
    EXPECT_EQ(summary.dropped_detail, 7u);
    EXPECT_TRUE(summary.detail_truncated);
    EXPECT_EQ(summary.by_track_count, 10u);
    EXPECT_EQ(summary.by_segment_count, 10u);
}

TEST(RepairSampleScan, LedgerTruncatesDetailByByteLimitWithoutLosingCounts) {
    RepairSampleLedger::Limits limits;
    limits.max_detail_samples = 1000;
    limits.max_detail_bytes = 250;
    RepairSampleLedger ledger(limits);

    for (uint32_t i = 0; i < 6; ++i)
        ledger.Add(MakeSample(0, 0, 100));

    EXPECT_EQ(ledger.Total(), 6u);
    EXPECT_EQ(ledger.CountForTrack(0), 6u);
    EXPECT_EQ(ledger.CountForSegment(0), 6u);
    EXPECT_EQ(ledger.Samples().size(), 2u);
    EXPECT_EQ(ledger.DroppedDetail(), 4u);
    EXPECT_TRUE(ledger.DetailTruncated());
}

TEST(RepairSampleScan, LedgerSummarySeparatesVerifiedInferredDecodedAndDamaged) {
    RepairSampleLedger ledger;
    auto verified = MakeSample(0, 0, 10, model::SampleConfidence::Verified);
    verified.decode_status = model::SampleDecodeStatus::Ok;
    ledger.Add(verified);

    auto inferred = MakeSample(0, 0, 10, model::SampleConfidence::Inferred);
    inferred.timestamp_evidence = model::SampleTimestampEvidence::Inferred;
    inferred.inference_basis = "按标称帧时长推进";
    inferred.damage_flags.push_back("truncated_au");
    ledger.Add(inferred);

    const auto summary = ledger.Summary();
    EXPECT_EQ(summary.total_samples, 2u);
    EXPECT_EQ(summary.verified_samples, 1u);
    EXPECT_EQ(summary.inferred_samples, 1u);
    EXPECT_EQ(summary.decoded_ok_samples, 1u);
    EXPECT_EQ(summary.damaged_samples, 1u);
}

TEST(RepairSampleScan, SampleSequenceDigestIsStableAndSensitive) {
    const std::vector<model::RecoveredSample> samples = {MakeSample(0, 0, 10), MakeSample(0, 1, 20),
                                                         MakeSample(1, 2, 30)};
    const std::string digest = SampleSequenceDigest(samples);
    EXPECT_EQ(digest, SampleSequenceDigest(samples));

    auto reordered = samples;
    std::swap(reordered[0], reordered[2]);
    EXPECT_NE(digest, SampleSequenceDigest(reordered)) << "换序必须被检出";

    auto changed = samples;
    changed[1].pts_seconds = 1.5;
    EXPECT_NE(digest, SampleSequenceDigest(changed)) << "时间戳变化必须被检出";

    auto shortened = samples;
    shortened.pop_back();
    EXPECT_NE(digest, SampleSequenceDigest(shortened)) << "少一个样本必须被检出";
    EXPECT_TRUE(SampleSequenceDigest({}).empty() == false);
}

// ---------------- 上三层守恒（含反向守护） ----------------

TEST(RepairSampleScan, UpperLayerConservationAcceptsExactCounts) {
    RepairSampleLedger ledger;
    for (uint32_t i = 0; i < 4; ++i)
        ledger.Add(MakeSample(0, 0, 10));
    for (uint32_t i = 0; i < 3; ++i)
        ledger.Add(MakeSample(1, 1, 10));

    std::vector<model::RepairRecoveredSegment> segments(2);
    segments[0].index = 0;
    segments[0].packets_written = 4;
    segments[1].index = 1;
    segments[1].packets_written = 3;

    std::vector<model::RepairTrackConservation> tracks(2);
    tracks[0].output_stream_index = 0;
    tracks[0].packets_written = 4;
    tracks[1].output_stream_index = 1;
    tracks[1].packets_written = 3;

    const auto view = CheckUpperLayerConservation(ledger, segments, tracks, 7);
    const std::string first_problem = view.problems.empty() ? std::string() : view.problems.front();
    EXPECT_TRUE(view.consistent) << first_problem;
    EXPECT_EQ(view.expected_total, 7u);
    EXPECT_EQ(view.expected_segment_total, 7u);
    EXPECT_EQ(view.expected_track_total, 7u);
}

TEST(RepairSampleScan, UpperLayerConservationRejectsSegmentsWithoutSampleFacts) {
    const RepairSampleLedger ledger; // 样本层完全为空

    std::vector<model::RepairRecoveredSegment> segments(1);
    segments[0].index = 0;
    segments[0].packets_written = 12;

    std::vector<model::RepairTrackConservation> tracks(1);
    tracks[0].output_stream_index = 0;
    tracks[0].packets_written = 12;

    const auto view = CheckUpperLayerConservation(ledger, segments, tracks, 12);
    EXPECT_FALSE(view.consistent);
    ASSERT_FALSE(view.problems.empty());
    bool found_no_facts = false;
    for (const auto& problem : view.problems) {
        if (problem.find("不得谎报有恢复内容") != std::string::npos)
            found_no_facts = true;
    }
    EXPECT_TRUE(found_no_facts) << "无样本却声称有区间，必须单独报错";
}

TEST(RepairSampleScan, UpperLayerConservationRejectsMismatchedTrackAndTotal) {
    RepairSampleLedger ledger;
    for (uint32_t i = 0; i < 5; ++i)
        ledger.Add(MakeSample(0, 0, 10));

    std::vector<model::RepairRecoveredSegment> segments(1);
    segments[0].index = 0;
    segments[0].packets_written = 5;
    std::vector<model::RepairTrackConservation> tracks(1);
    tracks[0].output_stream_index = 0;
    tracks[0].packets_written = 4; // 故意少一包

    const auto view = CheckUpperLayerConservation(ledger, segments, tracks, 6);
    EXPECT_FALSE(view.consistent);
    EXPECT_GE(view.problems.size(), 2u);
}

} // namespace
