#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/analysis/codec/BitstreamAnalyzer.h"
#include "core/analysis/codec/InBandParameterSets.h"
#include "core/domain/model/BitstreamInfo.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::BitstreamAnalyzer;
using videoeye::ContainerMetadata;
using videoeye::InBandParameterSetProbe;
using videoeye::model::AnalysisCapability;
using videoeye::model::BitstreamAnalysisResult;
using videoeye::model::BitstreamParseOutcome;

constexpr const char* kCodecCorpusDir = VIDEOEYE_CODEC_CORPUS_DIR;

std::vector<uint8_t> ReadCodecCorpus(const std::string& filename) {
    std::ifstream in(std::string(kCodecCorpusDir) + "/" + filename, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 与 tests/unit/test_h264_bitstream_parser.cpp 同一套真实 SPS：
// High / Level 3.1 / 1280x720 / 4:2:0 8bit / VUI(timing + colour)，
// 由 _smoke/gen_h264_sps.py 逐位生成。
const std::vector<uint8_t> kSpsHigh720p = {
    0x64, 0x00, 0x1F, 0xAC, 0x72, 0x14, 0x05, 0x00, 0x5B, 0xA6, 0xA0, 0x20, 0x20,
    0x28, 0x00, 0x00, 0x1F, 0x40, 0x00, 0x07, 0x53, 0x04, 0x78, 0xB1, 0x6C, 0xB0,
};

// High 10 / 1920x1088，frame crop bottom 4（4:2:0 的 CropUnitY=2）→ 1920x1080。
const std::vector<uint8_t> kSpsHigh10Cropped1080p = {
    0x6E, 0x00, 0x33, 0xA6, 0xCC, 0xA4, 0x01, 0xE0, 0x08, 0x9F, 0x95,
};

const std::vector<uint8_t> kPps = {0xEB, 0x4B, 0x2C, 0x80};

// Main 10 / 1920x1080 / 10bit / VUI(BT.2020 + PQ)，用于打通 hvcC -> SPS -> 摘要。
const std::vector<uint8_t> kHevcVpsMain10 = {
    0x0C, 0x01, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x03, 0x00, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x03, 0x00, 0x99, 0x27, 0x03, 0x00, 0x00, 0x03, 0x03, 0xE8, 0x00, 0x00, 0xEA, 0x60, 0x60,
};
const std::vector<uint8_t> kHevcSpsMain10 = {
    0x01, 0x02, 0x00, 0x00, 0x03, 0x00, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x99, 0xA0, 0x03, 0xC0, 0x80, 0x10, 0xE4, 0xD9, 0x49, 0xE4, 0x91, 0xB6, 0x4B, 0xB9, 0xA8,
    0x48, 0x80, 0x4D, 0xB2, 0x80, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x75, 0x30, 0x04,
};
const std::vector<uint8_t> kHevcPpsMain10 = {0xC0, 0x35, 0x3E, 0x0C, 0xC6, 0x80};

// 真实 libvvenc 编码的 vvcC（640x360 / 10bit / 4:2:0 / L2.1），带 SPS + PPS 两组。
// 与 tests/unit/test_vvc_bitstream_parser.cpp 同款；这里是 VVC 能力提升与容器比对
// 的判据来源（真实编码器输出，不是"生成器和解析器互相理解错"的合成数据）。
const std::vector<uint8_t> kVvcCRealLibvvenc = {
    0xFF, 0x00, 0x65, 0x5F, 0x01, 0x02, 0x23, 0x80, 0x00, 0x00, 0x02, 0x80, 0x01, 0x68, 0x00, 0x00, 0x02, 0x8F, 0x00,
    0x01, 0x00, 0xF3, 0x00, 0x79, 0x00, 0xAB, 0x02, 0x23, 0x80, 0x00, 0x00, 0x80, 0x14, 0x08, 0x05, 0xA4, 0x6A, 0x00,
    0x73, 0x7A, 0x21, 0x34, 0x52, 0x7B, 0xCE, 0x13, 0x65, 0x63, 0x04, 0x08, 0x27, 0x00, 0x13, 0x10, 0x10, 0x41, 0x04,
    0x20, 0x61, 0x08, 0x42, 0xC4, 0x42, 0x16, 0x48, 0x42, 0xD4, 0x21, 0x7A, 0x3D, 0x5A, 0x92, 0xF2, 0x49, 0xA9, 0x2C,
    0x91, 0x16, 0xA2, 0x2F, 0x11, 0x26, 0xA2, 0x24, 0x52, 0x44, 0x49, 0x92, 0x22, 0x4D, 0x46, 0x58, 0x88, 0x42, 0xC9,
    0x08, 0x5A, 0x84, 0x2F, 0x08, 0x49, 0xA8, 0x42, 0x45, 0x24, 0x21, 0x26, 0x48, 0x42, 0x5D, 0x49, 0x08, 0x48, 0x48,
    0x88, 0x42, 0x45, 0x11, 0x08, 0x49, 0x88, 0x84, 0x25, 0xD4, 0x44, 0x21, 0x22, 0x8C, 0x84, 0x24, 0xC6, 0x42, 0x12,
    0xEA, 0x32, 0x10, 0xA0, 0x41, 0x61, 0x08, 0x20, 0x31, 0x10, 0x81, 0x92, 0x20, 0xD4, 0x80, 0x4C, 0xC1, 0x04, 0x10,
    0x58, 0x40, 0x08, 0x2C, 0x40, 0x20, 0x21, 0x02, 0x01, 0x01, 0x50, 0x81, 0x00, 0x80, 0xD2, 0x10, 0x20, 0x10, 0x16,
    0x20, 0x40, 0x20, 0x22, 0x08, 0x04, 0x05, 0x90, 0x40, 0x20, 0x24, 0x20, 0x10, 0x32, 0x02, 0x02, 0x21, 0x01, 0x01,
    0x64, 0x20, 0x20, 0x24, 0x40, 0x40, 0xD0, 0x20, 0x24, 0x81, 0x03, 0x82, 0x06, 0x20, 0x10, 0xB2, 0x04, 0x08, 0x84,
    0x08, 0x16, 0x42, 0x04, 0x09, 0x10, 0x20, 0xD0, 0x40, 0x92, 0x08, 0x38, 0x41, 0x90, 0x22, 0xD0, 0x82, 0x48, 0x43,
    0x88, 0x68, 0x4B, 0x91, 0xCA, 0x81, 0x05, 0x84, 0x00, 0x82, 0xC4, 0x02, 0x02, 0x10, 0x20, 0x10, 0x15, 0x08, 0x10,
    0x08, 0x1F, 0x7A, 0x5D, 0x8A, 0xCE, 0x48, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xF0, 0xC4, 0x90,
    0x00, 0x01, 0x00, 0x0C, 0x00, 0x81, 0x00, 0x00, 0x0A, 0x04, 0x02, 0xD2, 0x22, 0x41, 0xFB, 0x82,
};

// 拼一份真实 avcC（FFmpeg 交出的 extradata 形态；NAL 里带 1 字节 header）
std::vector<uint8_t> MakeAvcCWithSps(const std::vector<uint8_t>& sps_payload, uint8_t profile_idc, uint8_t level_idc) {
    std::vector<uint8_t> sps;
    sps.push_back(0x67); // nal_ref_idc=3, type=7
    sps.insert(sps.end(), sps_payload.begin(), sps_payload.end());
    std::vector<uint8_t> pps;
    pps.push_back(0x68); // type=8
    pps.insert(pps.end(), kPps.begin(), kPps.end());

    std::vector<uint8_t> out = {
        0x01, profile_idc, 0x00, level_idc,
        0xFF, // reserved(6b) + lengthSizeMinusOne=3
        0xE1, // reserved(3b) + numSPS=1
    };
    out.push_back(static_cast<uint8_t>(sps.size() >> 8));
    out.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    out.insert(out.end(), sps.begin(), sps.end());
    out.push_back(0x01); // numPPS=1
    out.push_back(static_cast<uint8_t>(pps.size() >> 8));
    out.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

std::vector<uint8_t> MakeAvcC() {
    return MakeAvcCWithSps(kSpsHigh720p, 100, 31);
}

std::vector<uint8_t> MakeHvcC() {
    std::vector<uint8_t> out;
    out.reserve(23 + kHevcVpsMain10.size() + kHevcSpsMain10.size() + kHevcPpsMain10.size() + 21);
    out.push_back(0x01);            // configurationVersion
    out.push_back(0x02);            // profile_space(0)|tier(0)|profile_idc=2
    out.insert(out.end(), 4, 0x60); // profile_compatibility_flags
    out.insert(out.end(), 6, 0x00); // constraint_indicator_flags
    out.push_back(153);             // general_level_idc = 5.1
    out.push_back(0xF0);
    out.push_back(0x00);
    out.push_back(0xFC);
    out.push_back(0xFD); // chromaFormat = 1
    out.push_back(0xFA); // bitDepthLumaMinus8 = 2
    out.push_back(0xFA); // bitDepthChromaMinus8 = 2
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x0F); // temporal layers / lengthSizeMinusOne
    out.push_back(0x03); // numOfArrays

    auto append_array = [&out](uint8_t nal_type, const std::vector<uint8_t>& payload) {
        out.push_back(static_cast<uint8_t>(0x80 | nal_type));
        out.push_back(0x00);
        out.push_back(0x01);
        const size_t nal_size = payload.size() + 2;
        out.push_back(static_cast<uint8_t>((nal_size >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(nal_size & 0xFF));
        out.push_back(static_cast<uint8_t>(nal_type << 1));
        out.push_back(0x01);
        out.insert(out.end(), payload.begin(), payload.end());
    };
    append_array(32, kHevcVpsMain10);
    append_array(33, kHevcSpsMain10);
    append_array(34, kHevcPpsMain10);
    return out;
}

ContainerMetadata MakeContainer(int width, int height, int bit_depth = 8) {
    ContainerMetadata meta;
    meta.codec_name = "h264";
    meta.width = width;
    meta.height = height;
    meta.bit_depth = bit_depth;
    meta.color_primaries = 1;          // BT.709
    meta.transfer_characteristics = 1; // BT.709
    meta.matrix_coefficients = 1;      // BT.709
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
        if (item.severity == severity)
            ++n;
    }
    return n;
}

bool HasField(const BitstreamAnalysisResult& r, const std::string& field) {
    for (const auto& item : r.inconsistencies) {
        if (item.field == field)
            return true;
    }
    return false;
}

} // namespace

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

TEST(BitstreamAnalyzerTest, H264CroppedSpsIsAppliedExactlyOnce) {
    const std::vector<uint8_t> avcc = MakeAvcCWithSps(kSpsHigh10Cropped1080p, 110, 51);
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(MakeContainer(1920, 1080, 10));
    const BitstreamAnalysisResult r = analyzer.Analyze(avcc.data(), avcc.size(), AV_CODEC_ID_H264);

    ASSERT_TRUE(r.h264_sps.present);
    EXPECT_EQ(r.h264_sps.height(), 1080);
    EXPECT_EQ(r.width, r.h264_sps.width());
    EXPECT_EQ(r.height, r.h264_sps.height());
    EXPECT_EQ(r.width, 1920);
    EXPECT_EQ(r.height, 1080);
    EXPECT_EQ(r.bit_depth, 10);
    EXPECT_FALSE(HasField(r, "Height"));
}

TEST(BitstreamAnalyzerTest, HevcHvcCSummaryUsesParsedSpsFields) {
    const std::vector<uint8_t> hvcc = MakeHvcC();
    ContainerMetadata meta;
    meta.codec_name = "hevc";
    meta.width = 1920;
    meta.height = 1080;
    meta.bit_depth = 10;
    meta.color_primaries = 9;
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    const BitstreamAnalysisResult r = analyzer.Analyze(hvcc.data(), hvcc.size(), AV_CODEC_ID_HEVC);

    ASSERT_TRUE(r.hevc_sps.present);
    EXPECT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_EQ(r.width, 1920);
    EXPECT_EQ(r.height, 1080);
    EXPECT_EQ(r.bit_depth, 10);
    EXPECT_EQ(r.color_primaries, 9);
    EXPECT_TRUE(r.inconsistencies.empty());
}

TEST(BitstreamAnalyzerTest, HevcVpsOnlyIsPartialAndDoesNotInventBitDepth) {
    std::vector<uint8_t> annexb = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01};
    annexb.insert(annexb.end(), kHevcVpsMain10.begin(), kHevcVpsMain10.end());

    ContainerMetadata meta;
    meta.codec_name = "hevc";
    meta.bit_depth = 10;
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    const BitstreamAnalysisResult r = analyzer.Analyze(annexb.data(), annexb.size(), AV_CODEC_ID_HEVC);

    ASSERT_TRUE(r.hevc_vps.present);
    EXPECT_FALSE(r.hevc_sps.present);
    EXPECT_EQ(r.capability, AnalysisCapability::Partial);
    EXPECT_EQ(r.bit_depth, 0);
    EXPECT_FALSE(HasField(r, "Bit Depth"));
}

TEST(BitstreamAnalyzerTest, RealX264AnnexBReportsCroppedGeometryAndHrd) {
    const std::vector<uint8_t> sample = ReadCodecCorpus("real_x264_192x108_hrd.h264");
    ASSERT_FALSE(sample.empty());

    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(sample.data(), sample.size(), AV_CODEC_ID_H264);

    ASSERT_TRUE(r.h264_sps.present);
    EXPECT_EQ(r.h264_sps.profile_idc, 66);
    EXPECT_EQ(r.h264_sps.ProfileName(), "Baseline");
    EXPECT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_EQ(r.width, 192);
    EXPECT_EQ(r.height, 108);
    EXPECT_EQ(r.bit_depth, 8);
    EXPECT_EQ(r.color_primaries, 1);
    EXPECT_EQ(r.transfer_characteristics, 1);
    EXPECT_EQ(r.matrix_coefficients, 1);
    EXPECT_TRUE(r.h264_sps.vui.hrd_present);
}

TEST(BitstreamAnalyzerTest, RealX265RextTenBitAnnexBReportsGeometryColorAndHrd) {
    const std::vector<uint8_t> sample = ReadCodecCorpus("real_x265_rext_10bit_192x108_hrd.hevc");
    ASSERT_FALSE(sample.empty());

    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(sample.data(), sample.size(), AV_CODEC_ID_HEVC);

    ASSERT_TRUE(r.hevc_sps.present);
    EXPECT_EQ(r.hevc_sps.general_profile_idc, 4);
    EXPECT_EQ(r.hevc_sps.ProfileName(), "Main Rext");
    EXPECT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_EQ(r.width, 192);
    EXPECT_EQ(r.height, 108);
    EXPECT_EQ(r.bit_depth, 10);
    EXPECT_EQ(r.color_primaries, 1);
    EXPECT_EQ(r.transfer_characteristics, 1);
    EXPECT_EQ(r.matrix_coefficients, 1);
    EXPECT_TRUE(r.hevc_sps.vui_hrd_parameters_present_flag);
}

TEST(BitstreamAnalyzerTest, UnsupportedCodecIsNotAnalyzed) {
    const std::vector<uint8_t> avcc = MakeAvcC();
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(avcc.data(), avcc.size(), AV_CODEC_ID_MPEG4);

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
// 能力状态（阶段 4.1：消除"假完成"）
// 五种口径：NoExtradata / UnsupportedCodec / TypeOnly / ParseFailed / FullParse
// --------------------------------------------------------------------------
TEST(BitstreamAnalyzerTest, H264FullParseIsSupported) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1280, 720));
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::FullParse);
    EXPECT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_FALSE(r.partial);
    EXPECT_TRUE(r.capability_note.empty());
}

TEST(BitstreamAnalyzerTest, EmptyExtradataIsUnavailable) {
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(nullptr, 0, AV_CODEC_ID_H264);
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::NoExtradata);
    EXPECT_EQ(r.capability, AnalysisCapability::Unavailable);
    EXPECT_TRUE(r.partial);
    EXPECT_FALSE(r.capability_note.empty());
}

TEST(BitstreamAnalyzerTest, UnsupportedCodecIsUnavailable) {
    const std::vector<uint8_t> avcc = MakeAvcC();
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(avcc.data(), avcc.size(), AV_CODEC_ID_MPEG4);
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::UnsupportedCodec);
    EXPECT_EQ(r.capability, AnalysisCapability::Unavailable);
    EXPECT_TRUE(r.partial);
    EXPECT_FALSE(r.capability_note.empty());
}

TEST(BitstreamAnalyzerTest, TypeOnlyExtradataIsPartial) {
    // AnnexB 里只有 SEI NAL：识别出编码类型，但没有任何参数集
    const std::vector<uint8_t> annexb = {
        0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x04, 0x00, 0x00, 0x80, // SEI（type 6）
    };
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(annexb.data(), annexb.size(), AV_CODEC_ID_H264);
    EXPECT_TRUE(r.analyzed);
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::TypeOnly);
    EXPECT_EQ(r.capability, AnalysisCapability::Partial);
    EXPECT_TRUE(r.partial);
    EXPECT_FALSE(r.capability_note.empty());
}

TEST(BitstreamAnalyzerTest, TruncatedSpsIsParseFailed) {
    // AnnexB 里带 SPS NAL 但只截了 2 字节：见过参数集却解析不出 → ParseFailed
    std::vector<uint8_t> annexb = {0x00, 0x00, 0x00, 0x01, 0x67};
    annexb.push_back(kSpsHigh720p[0]);
    annexb.push_back(kSpsHigh720p[1]);
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(annexb.data(), annexb.size(), AV_CODEC_ID_H264);
    EXPECT_TRUE(r.analyzed);
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::ParseFailed);
    EXPECT_EQ(r.capability, AnalysisCapability::Failed);
    EXPECT_TRUE(r.partial);
    EXPECT_FALSE(r.capability_note.empty());
}

// VVC：vvcC-only（无 SPS 数组）只有配置记录的 PTL / max 尺寸兜底，没有 SPS，
// 仍是 Partial，不参与严格一致性比对 —— 与 AV1 的 av1C-only 口径一致。
TEST(BitstreamAnalyzerTest, VvcConfigOnlyRecordIsPartialAndSkipsComparison) {
    const std::vector<uint8_t> kVvcCRecordOnly = {
        0xFF, 0x00, 0x01, 0x40, 0x01, 0x02, 0x33, 0x80, 0x00, 0x07, 0x80, 0x04, 0x38, 0x00, 0x00, 0x00,
    };
    // 容器故意报不一致的尺寸/位深：Partial 能力必须跳过严格比对，不产生告警
    ContainerMetadata meta = MakeContainer(1280, 720, 8);
    meta.codec_name = "vvc";
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    const BitstreamAnalysisResult r = analyzer.Analyze(kVvcCRecordOnly.data(), kVvcCRecordOnly.size(), AV_CODEC_ID_VVC);

    EXPECT_TRUE(r.analyzed);
    EXPECT_TRUE(r.has_vvc);
    EXPECT_TRUE(r.vvc_config.present);
    EXPECT_FALSE(r.vvc_sps.present); // 记录里没有 SPS 数组
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::FullParse);
    EXPECT_EQ(r.capability, AnalysisCapability::Partial);
    EXPECT_TRUE(r.partial);
    EXPECT_EQ(r.width, 1920);
    EXPECT_EQ(r.height, 1080);
    EXPECT_TRUE(r.inconsistencies.empty());
}

// P0-2 回归：格式识别成 vvcC 但记录被截断 —— 不许再把"全 0 的 config"当成
// 有效参数集（旧实现无条件 present=true，让 AnyParameterSet() 恒真 -> FullParse）。
TEST(BitstreamAnalyzerTest, VvcTruncatedConfigRecordIsParseFailedNotFullParse) {
    // 真实 vvcC 只留前 4 字节：够 LooksLikeConfigRecord 的 0xF8 判据，
    // 但字段不全 -> ParseVvcC 早退，valid=false。
    const std::vector<uint8_t> truncated(kVvcCRealLibvvenc.begin(), kVvcCRealLibvvenc.begin() + 4);
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(truncated.data(), truncated.size(), AV_CODEC_ID_VVC);

    EXPECT_TRUE(r.analyzed);
    EXPECT_TRUE(r.has_vvc_config);      // 输入确实是 vvcC
    EXPECT_FALSE(r.vvc_config.present); // 但没解析成功，不许置 present
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::ParseFailed);
    EXPECT_EQ(r.capability, AnalysisCapability::Failed);
    EXPECT_TRUE(r.partial);
    EXPECT_FALSE(r.capability_note.empty());
}

// VVC 从"一刀切 Partial"改为按证据分级：SPS + PPS 都解出来、组合落在验证过的
// Profile/色度/位深矩阵里，才判 Supported（2026-10 起，配套畸形语料
// tests/corpus/codec/vvc_*.bin 与 test_vvc_frame_order_tracker.cpp）。
TEST(BitstreamAnalyzerTest, VvcFullParseWithSpsAndPpsIsSupported) {
    BitstreamAnalyzer analyzer; // 不设容器 metadata：只验能力状态
    const BitstreamAnalysisResult r =
        analyzer.Analyze(kVvcCRealLibvvenc.data(), kVvcCRealLibvvenc.size(), AV_CODEC_ID_VVC);

    EXPECT_TRUE(r.analyzed);
    EXPECT_TRUE(r.has_vvc);
    ASSERT_TRUE(r.vvc_sps.present);
    ASSERT_TRUE(r.vvc_pps.present); // 缺 PPS 时尺寸可能被覆盖，能力必须留在 Partial
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::FullParse);
    EXPECT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_FALSE(r.partial);
    EXPECT_EQ(r.width, 640);
    EXPECT_EQ(r.height, 360);
    EXPECT_EQ(r.bit_depth, 10);
}

// 容器与真实码流一致时没有告警（能力已升为 Supported，字段参与严格比对）。
TEST(BitstreamAnalyzerTest, VvcMatchingContainerReportsNoInconsistency) {
    ContainerMetadata meta;
    meta.codec_name = "vvc";
    meta.width = 640;
    meta.height = 360;
    meta.bit_depth = 10;
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    const BitstreamAnalysisResult r =
        analyzer.Analyze(kVvcCRealLibvvenc.data(), kVvcCRealLibvvenc.size(), AV_CODEC_ID_VVC);

    EXPECT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_FALSE(HasField(r, "Width"));
    EXPECT_FALSE(HasField(r, "Height"));
    EXPECT_FALSE(HasField(r, "Bit Depth"));
}

// 反例：能力升为 Supported 之后，容器报的尺寸/位深与码流不一致就必须**看得见**。
// 这条同时兜住"能力升级了但比对开关没跟上"——那种情况下面板会显示
// Supported 却对明显的不一致保持沉默。
TEST(BitstreamAnalyzerTest, VvcContainerMismatchIsReportedWhenSupported) {
    ContainerMetadata meta;
    meta.codec_name = "vvc";
    meta.width = 1920;
    meta.height = 1080;
    meta.bit_depth = 8;
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    const BitstreamAnalysisResult r =
        analyzer.Analyze(kVvcCRealLibvvenc.data(), kVvcCRealLibvvenc.size(), AV_CODEC_ID_VVC);

    ASSERT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_TRUE(HasField(r, "Width"));
    EXPECT_TRUE(HasField(r, "Height"));
    EXPECT_TRUE(HasField(r, "Bit Depth"));
}

// 位深来源不可信时不许误报：vvcC-only（Partial，位深只有配置记录兜底）即便与
// 容器位深不同，也不产生 Bit Depth 告警。
TEST(BitstreamAnalyzerTest, VvcBitDepthUnknownDoesNotReportFalseMismatch) {
    const std::vector<uint8_t> kVvcCRecordOnly = {
        0xFF, 0x00, 0x01, 0x40, 0x01, 0x02, 0x33, 0x80, 0x00, 0x07, 0x80, 0x04, 0x38, 0x00, 0x00, 0x00,
    };
    ContainerMetadata meta;
    meta.codec_name = "vvc";
    meta.bit_depth = 8; // 与 vvcC 声称的 10bit 不同
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    const BitstreamAnalysisResult r = analyzer.Analyze(kVvcCRecordOnly.data(), kVvcCRecordOnly.size(), AV_CODEC_ID_VVC);

    EXPECT_EQ(r.capability, AnalysisCapability::Partial);
    EXPECT_FALSE(HasField(r, "Bit Depth"));
}

// 能力状态要进 JSON（面板"复制 JSON"与导出走的就是 ToJson）
TEST(BitstreamAnalyzerTest, CapabilityFieldsAreSerialized) {
    const BitstreamAnalysisResult r = AnalyzeWith(MakeContainer(1280, 720));
    const std::string json = r.ToJson();
    EXPECT_NE(json.find("\"parse_outcome\""), std::string::npos);
    EXPECT_NE(json.find("\"capability\""), std::string::npos);
    EXPECT_NE(json.find("\"partial\""), std::string::npos);
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
    const BitstreamAnalysisResult r = analyzer.Analyze(avcc.data(), avcc.size(), AV_CODEC_ID_H264);

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

// ===========================================================================
// AV1
//
// 与 tests/unit/test_av1_bitstream_parser.cpp 共用同一批逐位生成的序列头，
// 这里只关心两件事：解析结果有没有走到统一字段、以及能力门禁有没有生效。
// ===========================================================================

// Profile 0 / 4:2:0 8bit / 1920x1080 / level 3.0 / BT.709，color_range=0(limited)
const std::vector<uint8_t> kAv1SeqHdr1080p = {
    0x0A, 0x0E, 0x00, 0x00, 0x00, 0x25, 0x57, 0x7F, 0x86, 0xE7, 0xF4, 0xEC, 0x80, 0x80, 0x80, 0x82,
};

// Profile 0 / 4:0:0 8bit / 320x240 / full range —— 色度不合格的那一条
const std::vector<uint8_t> kAv1SeqHdrMonochrome = {
    0x0A, 0x0D, 0x00, 0x00, 0x00, 0x04, 0x3C, 0xFF, 0xBC, 0xFE, 0x9D, 0xB0, 0x10, 0x10, 0x1A,
};

ContainerMetadata MakeAv1Container(int width, int height, int color_range) {
    ContainerMetadata meta;
    meta.codec_name = "av1";
    meta.width = width;
    meta.height = height;
    meta.bit_depth = 8;
    meta.color_primaries = 1;          // BT.709 —— 与上面两条序列头一致
    meta.transfer_characteristics = 1; // BT.709
    meta.matrix_coefficients = 1;      // BT.709
    meta.color_range = color_range;
    return meta;
}

BitstreamAnalysisResult AnalyzeAv1InBand(const std::vector<uint8_t>& obus, const ContainerMetadata& meta) {
    BitstreamAnalyzer analyzer;
    analyzer.SetContainerMetadata(meta);
    return analyzer.AnalyzeInBand(obus.data(), obus.size(), AV_CODEC_ID_AV1);
}

TEST(BitstreamAnalyzerTest, Av1SequenceHeaderIsSupportedAndFeedsSummary) {
    const BitstreamAnalysisResult r = AnalyzeAv1InBand(kAv1SeqHdr1080p, MakeAv1Container(1920, 1080, 1));

    ASSERT_TRUE(r.av1_seq_header.present);
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::FullParse);
    EXPECT_EQ(r.capability, AnalysisCapability::Supported);
    EXPECT_EQ(r.width, 1920);
    EXPECT_EQ(r.height, 1080);
    EXPECT_EQ(r.bit_depth, 8);
    EXPECT_EQ(r.color_primaries, 1);

    // ❗ 色域范围：码流里是 0(limited) -> 统一字段按 AVColorRange 口径记 1。
    // 以前这一项从来没被填过，于是"容器 full / 码流 limited"这类错配一条都报不出来。
    EXPECT_EQ(r.color_range, 1);
    EXPECT_TRUE(r.inconsistencies.empty());
}

TEST(BitstreamAnalyzerTest, Av1ColorRangeMismatchIsReported) {
    // 容器声明 full(2)、码流是 limited(1)：搞反会让画面发灰，是最该被抓到的一类
    const BitstreamAnalysisResult r = AnalyzeAv1InBand(kAv1SeqHdr1080p, MakeAv1Container(1920, 1080, 2));

    EXPECT_EQ(r.color_range, 1);
    EXPECT_TRUE(HasField(r, "Color Range"));
    // 宽高一致，不该有别的噪声
    EXPECT_FALSE(HasField(r, "Width"));
    EXPECT_FALSE(HasField(r, "Height"));
}

TEST(BitstreamAnalyzerTest, Av1MonochromeIsPartialAndSkipsComparison) {
    // 4:0:0 没有面板比对通路 —— 与 H.264/HEVC 拦 chroma_format_idc==0 同口径。
    const BitstreamAnalysisResult r = AnalyzeAv1InBand(kAv1SeqHdrMonochrome, MakeAv1Container(320, 240, 2));

    ASSERT_TRUE(r.av1_seq_header.present);
    EXPECT_EQ(r.capability, AnalysisCapability::Partial);
    EXPECT_TRUE(r.partial);
    EXPECT_NE(r.capability_note.find("色度采样"), std::string::npos);
    // Partial 不参与严格比对：哪怕容器写的是 full、码流也是 full，也不该有告警
    EXPECT_TRUE(r.inconsistencies.empty());
}

// ===========================================================================
// 带内参数集（extradata 为空的封装：MPEG-TS / 裸流 / fMP4 媒体分片）
//
// 这一组锁定"没有 extradata 也能拿到参数集"这条链路：
//   * BitstreamAnalyzer::AnalyzeInBand —— 解一次码流本体；
//   * InBandParameterSetProbe         —— 攒包、决定何时解、上限与放弃原因。
// 在此之前这类封装一律落 NoExtradata，码流分析整块不可用。
// ===========================================================================

TEST(InBandParameterSetTest, AnalyzeInBandParsesAnnexBH264) {
    const std::vector<uint8_t> sample = ReadCodecCorpus("real_x264_192x108_hrd.h264");
    ASSERT_FALSE(sample.empty());

    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.AnalyzeInBand(sample.data(), sample.size(), AV_CODEC_ID_H264);

    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::FullParse);
    EXPECT_EQ(r.parameter_set_source, "in_band");
    EXPECT_TRUE(r.h264_sps.present);
    EXPECT_EQ(r.width, 192);
    EXPECT_EQ(r.height, 108);
}

TEST(InBandParameterSetTest, ExtradataEntryStillReportsNoExtradata) {
    // 两个入口的语义不能混：extradata 为空是"容器没携带"，
    // 与"带内这段没出现参数集"是两种不同结论。
    BitstreamAnalyzer analyzer;
    const BitstreamAnalysisResult r = analyzer.Analyze(nullptr, 0, AV_CODEC_ID_H264);
    EXPECT_EQ(r.parse_outcome, BitstreamParseOutcome::NoExtradata);
    EXPECT_TRUE(r.parameter_set_source.empty());
}

TEST(InBandParameterSetTest, ProbeCollectsUntilParameterSetAppears) {
    // 前面垫一个无关 NAL（SEI），参数集在第二个包才出现 ——
    // 这正是"录播从 GOP 中间开始"的形态：头几个包不含 SPS。
    InBandParameterSetProbe probe;
    probe.Reset(AV_CODEC_ID_H264);
    ASSERT_TRUE(probe.enabled());

    const std::vector<uint8_t> filler = {0x00, 0x00, 0x00, 0x01, 0x06, 0x05, 0xFF, 0x80};
    EXPECT_FALSE(probe.Feed(filler.data(), filler.size()));
    EXPECT_FALSE(probe.done());
    EXPECT_FALSE(probe.given_up());

    const std::vector<uint8_t> sample = ReadCodecCorpus("real_x264_192x108_hrd.h264");
    ASSERT_FALSE(sample.empty());
    EXPECT_TRUE(probe.Feed(sample.data(), sample.size()));
    EXPECT_TRUE(probe.done());
    EXPECT_EQ(probe.packets(), 2);
    EXPECT_EQ(probe.result().parameter_set_source, "in_band");
    EXPECT_EQ(probe.result().width, 192);
}

TEST(InBandParameterSetTest, ProbeFindsParameterSetSplitAcrossPackets) {
    // 逐 3 字节喂：起始码与 NAL header 一定会被切开，
    // 重叠扫描必须让它们还能被认出来（少了重叠就会整段漏掉）。
    InBandParameterSetProbe probe;
    probe.Reset(AV_CODEC_ID_H264);

    const std::vector<uint8_t> sample = ReadCodecCorpus("real_x264_192x108_hrd.h264");
    ASSERT_FALSE(sample.empty());

    bool ok = false;
    for (size_t off = 0; off < sample.size() && !ok; off += 3) {
        const size_t n = std::min<size_t>(3, sample.size() - off);
        ok = probe.Feed(sample.data() + off, n);
    }
    EXPECT_TRUE(ok);
    EXPECT_TRUE(probe.done());
    EXPECT_EQ(probe.result().width, 192);
}

TEST(InBandParameterSetTest, ProbeDisabledForCodecWithoutParser) {
    // 没有参数集解析器的编码不值得攒包：攒到最后也解不出东西。
    InBandParameterSetProbe probe;
    probe.Reset(AV_CODEC_ID_MPEG2VIDEO);
    EXPECT_FALSE(probe.enabled());

    const uint8_t pkt[8] = {0x00, 0x00, 0x01, 0xB3, 0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(probe.Feed(pkt, sizeof(pkt)));
    EXPECT_FALSE(probe.result().analyzed);
}

TEST(InBandParameterSetTest, ProbeGivesUpAtByteCap) {
    InBandParameterSetProbe::Options opt;
    opt.max_bytes = 16;
    opt.max_packets = 0; // 只看字节上限
    InBandParameterSetProbe probe;
    probe.Reset(AV_CODEC_ID_H264, opt);

    // 只有 PPS、没有 SPS：起始码会命中"参数集类型"，但解析不出完整参数集，
    // 于是走补齐重试路径 —— 字节上限先到，必须 given_up 而不是无限攒。
    const std::vector<uint8_t> pps_only = {0x00, 0x00, 0x00, 0x01, 0x68, 0xEB, 0x4B, 0x2C};
    bool done = false;
    for (int i = 0; i < 8 && !done; ++i) {
        done = probe.Feed(pps_only.data(), pps_only.size());
    }
    EXPECT_FALSE(done);
    EXPECT_TRUE(probe.given_up());
    EXPECT_FALSE(probe.GiveUpReason().empty());
    EXPECT_TRUE(probe.result().parameter_set_source.empty());
}

TEST(InBandParameterSetTest, ProbeReportsReasonAtPacketCap) {
    InBandParameterSetProbe::Options opt;
    opt.max_packets = 2;
    InBandParameterSetProbe probe;
    probe.Reset(AV_CODEC_ID_H264, opt);

    // 纯非 IDR 切片（NAL 类型 1）：扫满包数上限后放弃，且原因要能读懂
    const std::vector<uint8_t> slice = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x02, 0x03};
    EXPECT_FALSE(probe.Feed(slice.data(), slice.size()));
    EXPECT_FALSE(probe.Feed(slice.data(), slice.size()));
    EXPECT_FALSE(probe.given_up()); // 第 2 个包刚好到上限，还没触发
    EXPECT_FALSE(probe.Feed(slice.data(), slice.size()));
    EXPECT_TRUE(probe.given_up());
    EXPECT_NE(probe.GiveUpReason().find("参数集"), std::string::npos);
}
