#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/analyzer/BitstreamAnalyzer.h"
#include "core/model/BitstreamInfo.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::analyzer::BitstreamAnalyzer;
using videoeye::analyzer::ContainerMetadata;
using videoeye::model::BitstreamAnalysisResult;

// 与 tests/unit/test_h264_bitstream_parser.cpp 同一套真实 SPS：
// High / Level 3.1 / 1280x720 / 4:2:0 8bit / VUI(timing + colour)，
// 由 _smoke/gen_h264_sps.py 逐位生成。
const std::vector<uint8_t> kSpsHigh720p = {
    0x64, 0x00, 0x1F, 0xAC, 0x72, 0x14, 0x05, 0x00, 0x5B, 0xA6, 0xA0, 0x20,
    0x20, 0x28, 0x00, 0x00, 0x1F, 0x40, 0x00, 0x07, 0x53, 0x04, 0x78, 0xB1,
    0x6C, 0xB0,
};

const std::vector<uint8_t> kPps = {0xEB, 0x4B, 0x2C, 0x80};

// 拼一份真实 avcC（FFmpeg 交出的 extradata 形态；NAL 里带 1 字节 header）
std::vector<uint8_t> MakeAvcC() {
    std::vector<uint8_t> sps;
    sps.push_back(0x67);  // nal_ref_idc=3, type=7
    sps.insert(sps.end(), kSpsHigh720p.begin(), kSpsHigh720p.end());
    std::vector<uint8_t> pps;
    pps.push_back(0x68);  // type=8
    pps.insert(pps.end(), kPps.begin(), kPps.end());

    std::vector<uint8_t> out = {
        0x01,              // configurationVersion
        0x64, 0x00, 0x1F,  // profile=100(High) / compat / level=3.1
        0xFF,              // reserved(6b) + lengthSizeMinusOne=3
        0xE1,              // reserved(3b) + numSPS=1
    };
    out.push_back(static_cast<uint8_t>(sps.size() >> 8));
    out.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    out.insert(out.end(), sps.begin(), sps.end());
    out.push_back(0x01);  // numPPS=1
    out.push_back(static_cast<uint8_t>(pps.size() >> 8));
    out.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

ContainerMetadata MakeContainer(int width, int height, int bit_depth = 8) {
    ContainerMetadata meta;
    meta.codec_name = "h264";
    meta.width = width;
    meta.height = height;
    meta.bit_depth = bit_depth;
    meta.color_primaries = 1;             // BT.709
    meta.transfer_characteristics = 1;    // BT.709
    meta.matrix_coefficients = 1;         // BT.709
    meta.color_range = 1;
    return meta;
}

BitstreamAnalysisResult AnalyzeWith(const ContainerMetadata& meta) {
    const std::vector<uint8_t> avcc = MakeAvcC();
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    return analyzer.Analyze(avcc.data(), avcc.size(), AV_CODEC_ID_H264);
}

int CountSeverity(const BitstreamAnalysisResult& r, const std::string& severity) {
    int n = 0;
    for (const auto& item : r.inconsistencies) {
        if (item.severity == severity) ++n;
    }
    return n;
}

bool HasField(const BitstreamAnalysisResult& r, const std::string& field) {
    for (const auto& item : r.inconsistencies) {
        if (item.field == field) return true;
    }
    return false;
}

}  // namespace

// --------------------------------------------------------------------------
// extradata -> 参数集
// --------------------------------------------------------------------------
TEST(BitstreamAnalyzerTest, ParsesAvcCIntoSpsAndPps) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1280, 720));

    EXPECT_TRUE(r.analyzed);
    EXPECT_TRUE(r.has_h264);
    EXPECT_EQ(r.codec_name, "h264");
    EXPECT_EQ(r.codec_id_str, "AV_CODEC_ID_H264");
    ASSERT_TRUE(r.h264_sps.present);
    EXPECT_TRUE(r.h264_pps.present);
    EXPECT_EQ(r.width, 1280);
    EXPECT_EQ(r.height, 720);
    EXPECT_EQ(r.bit_depth, 8);
    // VUI 里有 colour_description，色彩三要素应当被回填
    EXPECT_EQ(r.color_primaries, 1);
    EXPECT_EQ(r.transfer_characteristics, 1);
    EXPECT_EQ(r.matrix_coefficients, 1);
}

TEST(BitstreamAnalyzerTest, UnsupportedCodecIsNotAnalyzed) {
    const std::vector<uint8_t> avcc = MakeAvcC();
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r =
        analyzer.Analyze(avcc.data(), avcc.size(), AV_CODEC_ID_MPEG4);

    // 不支持的编码必须显式标记未分析，别让上层把默认 0 当成真实参数
    EXPECT_FALSE(r.analyzed);
    EXPECT_TRUE(r.codec_name.empty());
}

TEST(BitstreamAnalyzerTest, EmptyExtradataIsNotAnalyzed) {
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(nullptr, 0, AV_CODEC_ID_H264);
    EXPECT_FALSE(r.analyzed);
}

// --------------------------------------------------------------------------
// 容器侧快照（UI 对比表的左列）
// --------------------------------------------------------------------------
TEST(BitstreamAnalyzerTest, ContainerSnapshotIsWrittenToResult) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1280, 720));

    ASSERT_TRUE(r.has_container);
    EXPECT_EQ(r.container_width, 1280);
    EXPECT_EQ(r.container_height, 720);
    EXPECT_EQ(r.container_bit_depth, 8);
    EXPECT_EQ(r.container_color_primaries, 1);
    EXPECT_EQ(r.container_transfer_characteristics, 1);
    EXPECT_EQ(r.container_matrix_coefficients, 1);
    EXPECT_EQ(r.container_color_range, 1);
}

// 结果里带的容器快照要能被序列化出来（面板的「复制 JSON」走的就是它）
TEST(BitstreamAnalyzerTest, ContainerSnapshotIsSerialized) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1280, 720));
    const std::string json = r.ToJson();

    EXPECT_NE(json.find("\"container\""), std::string::npos);
    EXPECT_NE(json.find("1280"), std::string::npos);
}

TEST(BitstreamAnalyzerTest, NoContainerMetadataMeansNoSnapshotAndNoWarnings) {
    const std::vector<uint8_t> avcc = MakeAvcC();
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r =
        analyzer.Analyze(avcc.data(), avcc.size(), AV_CODEC_ID_H264);

    EXPECT_TRUE(r.analyzed);
    EXPECT_EQ(r.width, 1280);
    EXPECT_FALSE(r.has_container);
    // 没有容器基准时不许凭空造告警
    EXPECT_TRUE(r.inconsistencies.empty());
}

// --------------------------------------------------------------------------
// 容器 vs 码流 对比分级
// --------------------------------------------------------------------------
TEST(BitstreamAnalyzerTest, MatchingContainerProducesNoInconsistency) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1280, 720));
    EXPECT_TRUE(r.inconsistencies.empty());
}

// conformance window / cropping 造成的几像素差属于正常口径差异 -> info
TEST(BitstreamAnalyzerTest, SmallGeometryDiffIsInfoNotWarning) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1282, 722));

    EXPECT_TRUE(HasField(r, "Width"));
    EXPECT_TRUE(HasField(r, "Height"));
    EXPECT_EQ(CountSeverity(r, "warning"), 0);
    EXPECT_EQ(CountSeverity(r, "info"), 2);
}

TEST(BitstreamAnalyzerTest, LargeGeometryDiffIsWarning) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(640, 360));

    EXPECT_TRUE(HasField(r, "Width"));
    EXPECT_TRUE(HasField(r, "Height"));
    EXPECT_EQ(CountSeverity(r, "warning"), 2);
}

TEST(BitstreamAnalyzerTest, BitDepthMismatchIsReported) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1280, 720, 10));
    EXPECT_TRUE(HasField(r, "Bit Depth"));
}
