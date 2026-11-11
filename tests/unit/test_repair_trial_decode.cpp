#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairTrialDecode.h"
#include "tests/support/TestMediaFactory.h"

namespace {

// R3：tests/unit 下不得声明 videoeye::repair 具名子命名空间。
using namespace videoeye::repair;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_trial_" + tag + "_" + std::to_string(stamp) + "_" + std::to_string(rd() % 1000) +
                             "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

// 原地覆盖若干字节（长度不变，stsz / stco 全部保持原样）。
bool PatchBytes(const std::string& path, uint64_t offset, std::size_t len, uint8_t value) {
    std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!io)
        return false;
    io.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!io)
        return false;
    const std::vector<uint8_t> fill(len, value);
    io.write(reinterpret_cast<const char*>(fill.data()), static_cast<std::streamsize>(fill.size()));
    return static_cast<bool>(io);
}

// 逐样本绝对偏移：stsc → chunk → 累加 stsz。
std::vector<uint64_t> SampleOffsets(const videoeye::IsobmffTrack& track) {
    std::vector<uint64_t> offsets;
    if (!track.has_stco || track.stsz.sample_count == 0)
        return offsets;
    uint32_t sample = 0;
    for (std::size_t chunk = 0; chunk < track.chunk_offsets.size() && sample < track.stsz.sample_count; ++chunk) {
        uint32_t samples_per_chunk = 1;
        for (const auto& entry : track.stsc) {
            if (entry.first_chunk <= chunk + 1)
                samples_per_chunk = entry.samples_per_chunk;
        }
        uint64_t offset = track.chunk_offsets[chunk];
        for (uint32_t i = 0; i < samples_per_chunk && sample < track.stsz.sample_count; ++i, ++sample) {
            offsets.push_back(offset);
            const uint32_t size = track.stsz.default_size != 0
                                      ? track.stsz.default_size
                                      : (sample < track.stsz.sizes.size() ? track.stsz.sizes[sample] : 0);
            offset += size;
        }
    }
    return offsets;
}

const videoeye::IsobmffTrack* FirstVideoTrack(const videoeye::IsobmffFile& parsed) {
    for (const auto& track : parsed.tracks) {
        if (track.handler == "vide")
            return &track;
    }
    return nullptr;
}

// 判据只看容器声明的 sample entry（avc1），不看编码器名字：
// 本机没有 libx264 时会降级到 libopenh264，但它产的仍是 H.264/AVC。
const videoeye::IsobmffTrack* ParseAvcTrack(const std::string& path, videoeye::IsobmffFile* parsed) {
    if (!videoeye::IsobmffParser::Parse(path, *parsed))
        return nullptr;
    const videoeye::IsobmffTrack* video = FirstVideoTrack(*parsed);
    if (video == nullptr || video->codec != "avc1")
        return nullptr;
    return video;
}

} // namespace

TEST(RepairTrialDecode, AcceptsHealthyH264Keyframe) {
    const std::string dir = MakeTempDir("healthy");
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "healthy");
    ASSERT_TRUE(media.ok) << media.error;
    videoeye::IsobmffFile parsed;
    if (ParseAvcTrack(media.path, &parsed) == nullptr)
        GTEST_SKIP() << "样本不是 H.264/AVC: " << parsed.error_message;

    const RepairTrialDecodeResult result =
        TrialDecodeVideoAt(media.path, 0.0, RepairTrialDecodeOptions{}, nullptr, nullptr);
    EXPECT_TRUE(result.attempted) << result.error;
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_GT(result.frames_decoded, 0);
    EXPECT_EQ(result.corrupt_frames, 0);
    EXPECT_FALSE(result.evidence.empty());
}

// 判别力核心：容器仍把第一帧标成关键帧，但 IDR 载荷已被破坏。
// 只要试解码真的在解码（而不是只看 flags），这个用例必须红。
TEST(RepairTrialDecode, RejectsCorruptedIdrPayload) {
    const std::string dir = MakeTempDir("corrupt_idr");
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "corrupt");
    ASSERT_TRUE(media.ok) << media.error;
    videoeye::IsobmffFile parsed;
    const videoeye::IsobmffTrack* video = ParseAvcTrack(media.path, &parsed);
    if (video == nullptr)
        GTEST_SKIP() << "样本不是 H.264/AVC: " << parsed.error_message;
    const std::vector<uint64_t> offsets = SampleOffsets(*video);
    ASSERT_FALSE(offsets.empty()) << "无法定位视频样本偏移";
    const uint32_t first_size = video->stsz.default_size != 0
                                    ? video->stsz.default_size
                                    : (video->stsz.sizes.empty() ? 0u : video->stsz.sizes.front());
    ASSERT_GT(first_size, 96u);
    // 破坏 slice 载荷中部：4 字节长度前缀与 NAL 头**保持合法**（容器层与语法层
    // 都看不出异常），只有真解码才知道解不出来。这是试解码唯一的价值所在。
    ASSERT_TRUE(PatchBytes(media.path, offsets.front() + 64, 32, 0xFF));

    const RepairTrialDecodeResult result =
        TrialDecodeVideoAt(media.path, 0.0, RepairTrialDecodeOptions{}, nullptr, nullptr);
    EXPECT_TRUE(result.attempted) << result.error;
    EXPECT_FALSE(result.ok) << "IDR 载荷已破坏，试解码不得判通过";
    EXPECT_FALSE(result.error.empty());
}

TEST(RepairTrialDecode, CancelledRunDoesNotClaimSuccess) {
    const std::string dir = MakeTempDir("cancel");
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "cancel");
    ASSERT_TRUE(media.ok) << media.error;

    std::atomic<bool> cancel{true};
    RepairRunBudget budget(&cancel, 0);
    const RepairTrialDecodeResult result =
        TrialDecodeVideoAt(media.path, 0.0, RepairTrialDecodeOptions{}, &cancel, &budget);
    EXPECT_FALSE(result.attempted);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("取消"), std::string::npos) << result.error;
}

TEST(RepairTrialDecode, OutOfRangeSeekDoesNotReportSuccess) {
    const std::string dir = MakeTempDir("out_of_range");
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "range");
    ASSERT_TRUE(media.ok) << media.error;
    videoeye::IsobmffFile parsed;
    if (ParseAvcTrack(media.path, &parsed) == nullptr)
        GTEST_SKIP() << "样本不是 H.264/AVC: " << parsed.error_message;

    const RepairTrialDecodeResult result =
        TrialDecodeVideoAt(media.path, media.duration_seconds + 30.0, RepairTrialDecodeOptions{}, nullptr, nullptr);
    EXPECT_FALSE(result.ok) << result.evidence;
}
