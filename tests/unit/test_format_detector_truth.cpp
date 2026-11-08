// 格式探测真值的字节级守护
//
// 背景：离线门禁 scripts/corpus_format_gate.py 用 4429 条真实语料把「认成什么格式」
// 钉在 INDEX.container 真值上，但它是 nightly 侧的；这里用**合成字节**把四类修复
// 钉进 PR 流程，且每类都配反向守护 —— 修一个误判时最容易犯的错是把相邻格式一起
// 抢走（尤其 TS 弱魔数：0x47 周期在 1 KiB 里有 ~564 个候选位置）。
//
// 修的四件事：
//   1. ISOBMFF 64 位扩展尺寸首盒（size=1 + mdat）不再被当成 Annex-B 裸流；
//   2. 无 ftyp 的老式 QuickTime（首盒 mdat/moov/wide）不再落回 Unknown；
//   3. RIFF 的 AMV 变体归入 AVI 家族；
//   4. WAV / GIF / PNG / JPEG / ID3 / PDF 等强签名优先于 TS 弱魔数 ——
//      FATE 的 wav-not-mpegts、tc217.gif、mpegts-png-prefix 里嵌了带完整 PAT 的
//      真 TS 数据，提高同步点门槛或校验 PAT 都压不住，只能靠强签名归属。

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

#include "core/analysis/orchestration/FormatDetector.h"

namespace fs = std::filesystem;

namespace {

using videoeye::FormatDetector;
using videoeye::model::ContainerFormat;

fs::path ScratchDir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir =
        fs::temp_directory_path() / (std::string("videoeye_format_truth_") + tag + "_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string Bytes(std::initializer_list<int> values) {
    std::string out;
    out.reserve(values.size());
    for (const int v : values) {
        out.push_back(static_cast<char>(v));
    }
    return out;
}

std::string Box32(uint32_t size, const std::string& type) {
    std::string out = Bytes({static_cast<int>((size >> 24) & 0xFF), static_cast<int>((size >> 16) & 0xFF),
                             static_cast<int>((size >> 8) & 0xFF), static_cast<int>(size & 0xFF)});
    out += type;
    return out;
}

std::string WriteBytes(const fs::path& dir, const std::string& name, std::string bytes) {
    const fs::path file = dir / name;
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return file.string();
}

// 在 1 KiB 里按固定步长埋 0x47 —— 模拟 FATE 那些「把真 TS 数据嵌进别的文件」的样本。
void EmbedSyncChain(std::string& bytes, int stride, int offset, int count) {
    for (int k = 0; k < count; ++k) {
        const size_t at = static_cast<size_t>(offset + k * stride);
        if (at < bytes.size()) {
            bytes[at] = static_cast<char>(0x47);
        }
    }
}

ContainerFormat Detect(const std::string& path) {
    return FormatDetector::Detect(path);
}

} // namespace

// ---- 1. 64 位扩展尺寸首盒 ------------------------------------------------

TEST(FormatDetectorTruthTest, ExtendedSizeMdatIsMp4NotRawEs) {
    const fs::path dir = ScratchDir("extended");
    std::string bytes = Bytes({0, 0, 0, 1}) + "mdat" + Bytes({0, 0, 0, 0, 0, 0, 0x10, 0x00});
    bytes.resize(4096, '\0');
    // 旧代码正是被载荷里的 MPEG 视频起始码骗去判 Raw ES 的。
    bytes.replace(16, 4, Bytes({0, 0, 1, 0xB3}));
    EXPECT_EQ(Detect(WriteBytes(dir, "extended.bin", bytes)), ContainerFormat::MP4);
}

TEST(FormatDetectorTruthTest, ExtendedSizeLargerThanFileIsNotMp4) {
    const fs::path dir = ScratchDir("extended_bogus");
    // largesize 声明 1 GiB，实际文件只有 1 KiB —— 长度不自洽，不能认成容器。
    std::string bytes = Bytes({0, 0, 0, 1}) + "mdat" + Bytes({0x40, 0, 0, 0, 0, 0, 0, 0});
    bytes.resize(1024, '\0');
    const auto fmt = Detect(WriteBytes(dir, "bogus.bin", bytes));
    EXPECT_NE(fmt, ContainerFormat::MP4);
    EXPECT_NE(fmt, ContainerFormat::MOV);
}

TEST(FormatDetectorTruthTest, AnnexBStreamStaysRawEs) {
    const fs::path dir = ScratchDir("annexb");
    std::string bytes = Bytes({0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1E, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80});
    bytes.resize(1024, '\0');
    EXPECT_EQ(Detect(WriteBytes(dir, "annexb.bin", bytes)), ContainerFormat::RAW_ES);
}

// ---- 2. 老式 QuickTime（无 ftyp） ----------------------------------------

TEST(FormatDetectorTruthTest, LegacyQuickTimeMdatIsMov) {
    const fs::path dir = ScratchDir("legacy_qt");
    std::string bytes = Box32(4096, "mdat");
    bytes.resize(4096, '\0');
    EXPECT_EQ(Detect(WriteBytes(dir, "legacy.qt", bytes)), ContainerFormat::MOV);
}

TEST(FormatDetectorTruthTest, LegacyQuickTimeWithShortFileIsNotMov) {
    const fs::path dir = ScratchDir("legacy_qt_short");
    std::string bytes = Box32(1u << 20, "mdat");
    bytes.resize(1024, '\0');
    const auto fmt = Detect(WriteBytes(dir, "short.qt", bytes));
    EXPECT_NE(fmt, ContainerFormat::MOV);
    EXPECT_NE(fmt, ContainerFormat::MP4);
}

// ---- 3. RIFF / AMV -------------------------------------------------------

TEST(FormatDetectorTruthTest, RiffAmvIsAvi) {
    const fs::path dir = ScratchDir("amv");
    std::string bytes = std::string("RIFF") + Bytes({0, 0, 0, 0}) + "AMV " + "LIST";
    bytes.resize(1024, '\0');
    EXPECT_EQ(Detect(WriteBytes(dir, "clip.amv", bytes)), ContainerFormat::AVI);
}

TEST(FormatDetectorTruthTest, RiffWaveIsNotAvi) {
    const fs::path dir = ScratchDir("riff_wave");
    std::string bytes = std::string("RIFF") + Bytes({0xF8, 0x07, 0, 0}) + "WAVE" + "fmt ";
    bytes.resize(1024, '\0');
    EXPECT_NE(Detect(WriteBytes(dir, "clip.wav", bytes)), ContainerFormat::AVI);
}

TEST(FormatDetectorTruthTest, StandardAviStillAvi) {
    const fs::path dir = ScratchDir("avi");
    std::string bytes = std::string("RIFF") + Bytes({0, 0, 0, 0}) + "AVI " + "LIST";
    bytes.resize(1024, '\0');
    EXPECT_EQ(Detect(WriteBytes(dir, "clip.avi", bytes)), ContainerFormat::AVI);
}

// ---- 4. 强签名优先于 TS 弱魔数 -------------------------------------------

TEST(FormatDetectorTruthTest, WavWithEmbeddedTsChainIsNotTs) {
    const fs::path dir = ScratchDir("wav_ts");
    std::string bytes = std::string("RIFF") + Bytes({0xF8, 0x07, 0, 0}) + "WAVE" + "fmt ";
    bytes.resize(1024, '\0');
    EmbedSyncChain(bytes, 192, 160, 5); // FATE wav-not-mpegts 的埋法
    EXPECT_NE(Detect(WriteBytes(dir, "wav-not-mpegts", bytes)), ContainerFormat::MPEG_TS);
}

TEST(FormatDetectorTruthTest, GifWithEmbeddedTsChainIsNotTs) {
    const fs::path dir = ScratchDir("gif_ts");
    std::string bytes = std::string("GIF89a") + Bytes({0xD9, 0, 0xD9, 0});
    bytes.resize(1024, '\0');
    EmbedSyncChain(bytes, 192, 59, 4); // FATE tc217.gif 的埋法
    EXPECT_NE(Detect(WriteBytes(dir, "tc217.gif", bytes)), ContainerFormat::MPEG_TS);
}

TEST(FormatDetectorTruthTest, PngWithEmbeddedTsChainIsNotTs) {
    const fs::path dir = ScratchDir("png_ts");
    std::string bytes = Bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}) + Box32(13, "IHDR");
    bytes.resize(1024, '\0');
    EmbedSyncChain(bytes, 188, 70, 6); // FATE mpegts-png-prefix 的埋法
    EXPECT_NE(Detect(WriteBytes(dir, "mpegts-png-prefix", bytes)), ContainerFormat::MPEG_TS);
}

TEST(FormatDetectorTruthTest, RealTsWithPatStillDetected) {
    const fs::path dir = ScratchDir("real_ts");
    std::string bytes(1024, '\0');
    for (int k = 0; k < 6; ++k) {
        const size_t at = static_cast<size_t>(k * 188);
        bytes[at] = static_cast<char>(0x47);
        bytes[at + 1] = static_cast<char>(0x40); // PUSI + PID 高 5 位 = 0
        bytes[at + 2] = 0x00;                    // PID = 0 (PAT)
        bytes[at + 3] = static_cast<char>(0x10); // adaptation_field_control = 1
    }
    EXPECT_EQ(Detect(WriteBytes(dir, "real.ts", bytes)), ContainerFormat::MPEG_TS);
}

TEST(FormatDetectorTruthTest, StandardFtypStillMp4) {
    const fs::path dir = ScratchDir("ftyp");
    std::string bytes = Box32(24, "ftyp") + "isom" + Bytes({0, 0, 2, 0}) + "isom" + "iso2";
    bytes.resize(1024, '\0');
    EXPECT_EQ(Detect(WriteBytes(dir, "clip.mp4", bytes)), ContainerFormat::MP4);
}
