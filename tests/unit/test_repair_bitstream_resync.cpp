#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/repair/RepairBitstreamResync.h"
#include "core/repair/RepairBudget.h"

namespace {

// R3：tests/unit 下不得声明 videoeye::repair 具名子命名空间。
using namespace videoeye::repair;
namespace fs = std::filesystem;

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "videoeye_bitstream_" + tag + "_" + std::to_string(stamp) + "_" +
                             std::to_string(rd() % 1000) + "_" + std::to_string(counter++);
    const fs::path dir = fs::temp_directory_path() / name;
    fs::create_directories(dir);
    return dir.string();
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

// 4 字节长度前缀 + NAL 载荷（与 avcC/hvcC 的 lengthSizeMinusOne == 3 一致）。
void AppendNal(std::vector<uint8_t>& out, const std::vector<uint8_t>& nal) {
    AppendBe32(out, static_cast<uint32_t>(nal.size()));
    out.insert(out.end(), nal.begin(), nal.end());
}

// 一个 H.264 访问单元：可选 SPS+PPS 前缀，加一个 VCL 分片。
void AppendH264Au(std::vector<uint8_t>& out, bool idr, bool with_params, uint8_t filler) {
    if (with_params) {
        AppendNal(out, {0x67, 0x42, 0x00, 0x1F, filler}); // SPS (type 7)
        AppendNal(out, {0x68, 0xCE, 0x38, 0x80, filler}); // PPS (type 8)
    }
    AppendNal(out, {static_cast<uint8_t>(idr ? 0x65 : 0x41), 0x88, 0x80, filler, filler});
}

// HEVC 访问单元：VPS/SPS/PPS + 一个 VCL（19 = IDR_W_RADL，1 = TRAIL_R）。
void AppendHevcAu(std::vector<uint8_t>& out, bool idr) {
    AppendNal(out, {0x40, 0x01, 0x0C}); // VPS (type 32)
    AppendNal(out, {0x42, 0x01, 0x01}); // SPS (type 33)
    AppendNal(out, {0x44, 0x01, 0xC0}); // PPS (type 34)
    AppendNal(out, {static_cast<uint8_t>(idr ? 0x26 : 0x02), 0x01, 0xAF});
}

void AppendBox(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    AppendBe32(out, static_cast<uint32_t>(8 + payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
}

RepairBitstreamResyncResult ScanFile(const std::string& path, const std::string& codec, std::size_t chunk_bytes = 0,
                                     uint64_t max_scan_bytes = 32ull * 1024ull * 1024ull) {
    RepairBitstreamResyncOptions options;
    options.max_scan_bytes = max_scan_bytes;
    if (chunk_bytes != 0)
        options.chunk_bytes = chunk_bytes;
    return ScanForRandomAccessPoints(path, 0, static_cast<uint64_t>(fs::file_size(path)), codec, options, nullptr,
                                     nullptr);
}

TEST(RepairBitstreamResync, FindsH264RandomAccessPointsAndSkipsGarbage) {
    const std::string dir = MakeTempDir("h264_rap");
    const std::string path = (fs::path(dir) / "payload.bin").string();

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), 300, 0xFF); // 非 AVCC 垃圾：长度字段巨大，必须被剪掉
    const std::size_t first_au = payload.size();
    AppendH264Au(payload, true, true, 0x11);
    AppendH264Au(payload, false, false, 0x22);
    payload.insert(payload.end(), 200, 0x00); // 长度 0：链必须在此断掉
    const std::size_t second_au = payload.size();
    AppendH264Au(payload, true, true, 0x33);
    ASSERT_TRUE(WriteFile(path, payload));

    const RepairBitstreamResyncResult result = ScanFile(path, "h264");
    ASSERT_TRUE(result.attempted);
    ASSERT_TRUE(result.found) << result.error;
    ASSERT_EQ(result.candidates.size(), 2u) << result.error;
    EXPECT_EQ(result.candidates[0].byte_offset, first_au);
    EXPECT_EQ(result.candidates[1].byte_offset, second_au);
    EXPECT_TRUE(result.candidates[0].has_parameter_sets);
    EXPECT_FALSE(result.candidates[0].evidence.empty());
    EXPECT_GT(result.bytes_scanned, 0u);
    EXPECT_GT(result.chunks_loaded, 0u);
}

TEST(RepairBitstreamResync, RejectsPayloadWithoutRandomAccessPoint) {
    const std::string dir = MakeTempDir("h264_no_rap");
    const std::string path = (fs::path(dir) / "payload.bin").string();

    std::vector<uint8_t> payload;
    for (int i = 0; i < 4; ++i)
        AppendH264Au(payload, false, false, static_cast<uint8_t>(0x10 + i)); // 只有 P 帧
    ASSERT_TRUE(WriteFile(path, payload));

    const RepairBitstreamResyncResult result = ScanFile(path, "h264");
    EXPECT_TRUE(result.attempted);
    EXPECT_FALSE(result.found);
    EXPECT_FALSE(result.error.empty());
}

TEST(RepairBitstreamResync, FindsHevcRandomAccessPoints) {
    const std::string dir = MakeTempDir("hevc_rap");
    const std::string path = (fs::path(dir) / "payload.bin").string();

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), 64, 0xFF);
    const std::size_t au_offset = payload.size();
    AppendHevcAu(payload, true);
    ASSERT_TRUE(WriteFile(path, payload));

    const RepairBitstreamResyncResult result = ScanFile(path, "hevc");
    ASSERT_TRUE(result.found) << result.error;
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].byte_offset, au_offset);
    EXPECT_TRUE(result.candidates[0].has_parameter_sets);
}

// 分块只影响读取方式，不影响结论：换块大小必须得到同样的候选序列。
TEST(RepairBitstreamResync, ChunkSizeDoesNotChangeCandidates) {
    const std::string dir = MakeTempDir("chunk_size");
    const std::string path = (fs::path(dir) / "payload.bin").string();

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), 5000, 0xFF);
    AppendH264Au(payload, true, true, 0x44);
    payload.insert(payload.end(), 70000, 0xFF); // 跨多个块
    AppendH264Au(payload, true, true, 0x55);
    ASSERT_TRUE(WriteFile(path, payload));

    const RepairBitstreamResyncResult small = ScanFile(path, "h264", 4096);
    const RepairBitstreamResyncResult large = ScanFile(path, "h264", 65536);
    ASSERT_TRUE(small.found) << small.error;
    ASSERT_TRUE(large.found) << large.error;
    ASSERT_EQ(small.candidates.size(), large.candidates.size());
    for (std::size_t i = 0; i < small.candidates.size(); ++i) {
        EXPECT_EQ(small.candidates[i].byte_offset, large.candidates[i].byte_offset) << "候选 " << i;
        EXPECT_EQ(small.candidates[i].has_parameter_sets, large.candidates[i].has_parameter_sets);
    }
}

TEST(RepairBitstreamResync, CancelledBudgetStopsScanWithoutClaimingSuccess) {
    const std::string dir = MakeTempDir("cancel");
    const std::string path = (fs::path(dir) / "payload.bin").string();

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), 1000, 0xFF);
    AppendH264Au(payload, true, true, 0x66);
    ASSERT_TRUE(WriteFile(path, payload));

    std::atomic<bool> cancel{true};
    RepairRunBudget budget(&cancel, 0);
    RepairBitstreamResyncOptions options;
    const RepairBitstreamResyncResult result =
        ScanForRandomAccessPoints(path, 0, static_cast<uint64_t>(payload.size()), "h264", options, nullptr, &budget);
    EXPECT_FALSE(result.found);
    EXPECT_FALSE(result.error.empty());
}

TEST(RepairBitstreamResync, MarksWindowTruncationInsteadOfSilentlyScanningPrefix) {
    const std::string dir = MakeTempDir("truncated");
    const std::string path = (fs::path(dir) / "payload.bin").string();

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), 400, 0xFF); // 候选远在 64 字节窗口之外
    AppendH264Au(payload, true, true, 0x77);
    ASSERT_TRUE(WriteFile(path, payload));

    RepairBitstreamResyncOptions options;
    options.max_scan_bytes = 64;
    const RepairBitstreamResyncResult result =
        ScanForRandomAccessPoints(path, 0, static_cast<uint64_t>(payload.size()), "h264", options, nullptr, nullptr);
    EXPECT_FALSE(result.found);
    EXPECT_TRUE(result.truncated_by_budget);
    EXPECT_FALSE(result.limitations.empty());
}

TEST(RepairBitstreamResync, MdatWindowReportsPayloadOffsetAndSize) {
    const std::string dir = MakeTempDir("mdat");
    const std::string path = (fs::path(dir) / "box.mp4").string();

    std::vector<uint8_t> file;
    AppendBox(file, "ftyp", {0x69, 0x73, 0x6F, 0x6D, 0x00, 0x00, 0x02, 0x00});
    const std::size_t mdat_offset = file.size();
    const std::vector<uint8_t> payload(128, 0x5A);
    AppendBox(file, "mdat", payload);
    ASSERT_TRUE(WriteFile(path, file));

    const RepairMdatWindow window = FindPrimaryMdatPayload(path);
    ASSERT_TRUE(window.found) << window.error;
    EXPECT_EQ(window.payload_offset, mdat_offset + 8);
    EXPECT_EQ(window.payload_size, payload.size());
    EXPECT_FALSE(window.multiple_mdats);
}

TEST(RepairBitstreamResync, MdatWindowFailsLoudlyWhenNoMdat) {
    const std::string dir = MakeTempDir("no_mdat");
    const std::string path = (fs::path(dir) / "box.mp4").string();

    std::vector<uint8_t> file;
    AppendBox(file, "ftyp", {0x69, 0x73, 0x6F, 0x6D, 0x00, 0x00, 0x02, 0x00});
    ASSERT_TRUE(WriteFile(path, file));

    const RepairMdatWindow window = FindPrimaryMdatPayload(path);
    EXPECT_FALSE(window.found);
    EXPECT_FALSE(window.error.empty());
}

TEST(RepairBitstreamResync, UnsupportedCodecIsNotAttempted) {
    const std::string dir = MakeTempDir("unsupported");
    const std::string path = (fs::path(dir) / "payload.bin").string();
    ASSERT_TRUE(WriteFile(path, std::vector<uint8_t>(64, 0xFF)));

    const RepairBitstreamResyncResult result = ScanFile(path, "av1");
    EXPECT_FALSE(result.attempted);
    EXPECT_FALSE(result.found);
    EXPECT_FALSE(result.error.empty());
}

} // namespace
