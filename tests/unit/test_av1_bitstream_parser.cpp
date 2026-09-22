#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/analyzer/Av1BitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace {

using videoeye::analyzer::Av1BitstreamParser;
using videoeye::utils::ExtradataParser;
using videoeye::utils::ExtradataFormat;
using videoeye::utils::ObuUnit;

// 测试数据由 _smoke/gen_av1_seqhdr.py 逐位生成，语法元素顺序以 dav1d 的
// parse_seq_hdr()（src/obu.c）为准，不参考本项目 parser 的既有实现 ——
// 否则就会退化成「parser 和测试互相配合的错误」。
//
// ObuUnit::data 只含 OBU payload：不含 obu_header(1B)、不含 leb128 长度、
// 不含 extension header。

// Profile 0, 4:2:0 8bit, 1920x1080, level 3.0, BT.709
const std::vector<uint8_t> kSeqHdrProfile0_1080p = {
    0x0A, 0x0E, 0x00, 0x00, 0x00, 0x25, 0x57, 0x7F, 0x86, 0xE7, 0xF4, 0xEC,
    0x80, 0x80, 0x80, 0x82,
};

// 同上，但只有 payload（用于直接测 ParseFromObuUnit）
const std::vector<uint8_t> kSeqHdrProfile0_1080p_Payload = {
    0x00, 0x00, 0x00, 0x25, 0x57, 0x7F, 0x86, 0xE7, 0xF4, 0xEC, 0x80, 0x80,
    0x80, 0x82,
};

// Profile 0, 4:2:0 10bit, 3840x2160, level 5.1 tier=1, BT.2020/PQ
const std::vector<uint8_t> kSeqHdrProfile0_4k10bitHdr = {
    0x0A, 0x0F, 0x00, 0x00, 0x00, 0x6E, 0xEF, 0xBF, 0xE1, 0xBC, 0xFE, 0x9D,
    0xD0, 0x91, 0x00, 0x90, 0x40,
};

// Profile 2, 4:2:2 12bit, 1920x1080, level 4.0
const std::vector<uint8_t> kSeqHdrProfile2_422_12bit = {
    0x0A, 0x0F, 0x40, 0x00, 0x00, 0x42, 0xAB, 0xBF, 0xC3, 0x73, 0xFA, 0x77,
    0xA1, 0x22, 0x01, 0x28, 0x80,
};

// Profile 1, 4:4:4 8bit, 640x480, level 2.1
const std::vector<uint8_t> kSeqHdrProfile1_444 = {
    0x0A, 0x0E, 0x20, 0x00, 0x00, 0x0C, 0xC4, 0xFF, 0xDF, 0x3F, 0xA7, 0x68,
    0x08, 0x08, 0x08, 0x80,
};

// Profile 0, 4:0:0 8bit, 320x240, full range
const std::vector<uint8_t> kSeqHdrMonochrome = {
    0x0A, 0x0D, 0x00, 0x00, 0x00, 0x04, 0x3C, 0xFF, 0xBC, 0xFE, 0x9D, 0xB0,
    0x10, 0x10, 0x1A,
};

// 带 timing_info + decoder_model_info + 2 个 operating point
const std::vector<uint8_t> kSeqHdrWithTiming = {
    0x0A, 0x20, 0x04, 0x00, 0x00, 0x0F, 0xA4, 0x00, 0x03, 0xA9, 0x82, 0x4B,
    0xC0, 0x00, 0x00, 0xFA, 0x67, 0x40, 0x40, 0x01, 0x10, 0x00, 0x22, 0xA9,
    0x9F, 0xF6, 0x79, 0xFD, 0x3B, 0x20, 0x20, 0x20, 0x20, 0x80,
};

// 带 OBU extension header: temporal_id=2 spatial_id=1, level 3.1
const std::vector<uint8_t> kSeqHdrWithExtension = {
    0x0E, 0x48, 0x0E, 0x00, 0x00, 0x00, 0x2C, 0xC6, 0xAB, 0xDF, 0x3F, 0xA7,
    0x64, 0x04, 0x04, 0x04, 0x10,
};

// 序列头 + PADDING。PADDING 长度 200，leb128 编码为 2 字节 0xC8 0x01；
// payload 全是 0，在测试里现场拼出来，免得占几十行字面量。
const std::vector<uint8_t> kPaddingObuHeader = {0x7A, 0xC8, 0x01};  // 200 字节 PADDING
const size_t kPaddingObuPayloadSize = 200;

// TD + SequenceHeader + Padding 三个 OBU 连在一起（Padding 只放 1 字节示意）
const std::vector<uint8_t> kObuStream3Units = {
    0x12, 0x00, 0x0A, 0x0E, 0x00, 0x00, 0x00, 0x25, 0x57, 0x7F, 0x86, 0xE7,
    0xF4, 0xEC, 0x80, 0x80, 0x80, 0x82, 0x7A, 0x01, 0x00,
};

// av1C: profile 0, level 4.0, 4:2:0 8bit
const std::vector<uint8_t> kAv1CProfile0_420_8bit = {0x81, 0x08, 0x0C, 0x00};

// av1C: profile 0, level 5.1 tier 1, 4:2:0 10bit
const std::vector<uint8_t> kAv1CProfile0_420_10bit = {0x81, 0x0D, 0xCC, 0x00};

// av1C + 尾部 configOBUs（内含序列头）
const std::vector<uint8_t> kAv1CWithSeqHeader = {
    0x81, 0x08, 0x0C, 0x00, 0x0A, 0x0E, 0x00, 0x00, 0x00, 0x25, 0x57, 0x7F,
    0x86, 0xE7, 0xF4, 0xEC, 0x80, 0x80, 0x80, 0x82,
};

// av1C: 单色 + initial_presentation_delay=5
const std::vector<uint8_t> kAv1CMonochrome = {0x81, 0x00, 0x1C, 0x14};

std::vector<ObuUnit> Extract(const std::vector<uint8_t>& bytes) {
    return ExtradataParser::ExtractObuUnits(bytes.data(), bytes.size());
}

videoeye::model::Av1SequenceHeaderInfo ParseFirst(const std::vector<uint8_t>& bytes) {
    std::vector<ObuUnit> obus = Extract(bytes);
    if (obus.empty()) return videoeye::model::Av1SequenceHeaderInfo();
    return Av1BitstreamParser::ParseFromObuUnit(obus[0]);
}

// --------------------------------------------------------------------------
// OBU 头与长度字段
// --------------------------------------------------------------------------
class Av1ObuExtractionTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(Av1ObuExtractionTest, ObuHeaderBitLayout) {
    // 0x0A = 0b0000_1010：forbidden=0, type=1, extension=0, has_size=1, reserved=0
    // 旧实现写成 `type = header >> 1`，会把这个 OBU 读成 type 5。
    std::vector<ObuUnit> obus = Extract(kSeqHdrProfile0_1080p);
    ASSERT_EQ(obus.size(), 1u);
    EXPECT_EQ(obus[0].type, 1);
    EXPECT_TRUE(obus[0].is_sequence_header);
    EXPECT_TRUE(Av1BitstreamParser::IsSequenceHeaderObu(obus[0]));
    EXPECT_FALSE(obus[0].has_extension_header);
    EXPECT_EQ(obus[0].size, 14u);
    ASSERT_EQ(obus[0].data.size(), 14u);
    EXPECT_EQ(obus[0].data, kSeqHdrProfile0_1080p_Payload);
}

TEST_F(Av1ObuExtractionTest, ObuWithExtensionHeader) {
    // 0x0E = type 1 + extension_flag + has_size；0x48 = temporal_id 2 / spatial_id 1
    std::vector<ObuUnit> obus = Extract(kSeqHdrWithExtension);
    ASSERT_EQ(obus.size(), 1u);
    EXPECT_EQ(obus[0].type, 1);
    EXPECT_TRUE(obus[0].has_extension_header);
    EXPECT_EQ(obus[0].temporal_id, 2);
    EXPECT_EQ(obus[0].spatial_id, 1);
    EXPECT_EQ(obus[0].size, 14u);
    ASSERT_EQ(obus[0].data.size(), 14u);
}

TEST_F(Av1ObuExtractionTest, MultiByteLeb128) {
    // PADDING 长度 200 -> leb128 编码为 0xC8 0x01。
    // 旧实现是 `size += (byte & 0x7F)` 累加，会算成 0x48 + 0x01 = 73。
    std::vector<uint8_t> stream = kSeqHdrProfile0_1080p;
    stream.insert(stream.end(), kPaddingObuHeader.begin(), kPaddingObuHeader.end());
    stream.insert(stream.end(), kPaddingObuPayloadSize, 0x00);

    std::vector<ObuUnit> obus = Extract(stream);
    ASSERT_EQ(obus.size(), 2u);
    EXPECT_EQ(obus[0].type, 1);
    EXPECT_EQ(obus[1].type, 15);          // OBU_PADDING
    EXPECT_EQ(obus[1].size, 200u);
    EXPECT_EQ(obus[1].data.size(), 200u);
}

TEST_F(Av1ObuExtractionTest, MultipleObusInOrder) {
    std::vector<ObuUnit> obus = Extract(kObuStream3Units);
    ASSERT_EQ(obus.size(), 3u);
    EXPECT_EQ(obus[0].type, 2);           // OBU_TEMPORAL_DELIMITER
    EXPECT_EQ(obus[1].type, 1);           // OBU_SEQUENCE_HEADER
    EXPECT_EQ(obus[2].type, 15);          // OBU_PADDING
    EXPECT_EQ(obus[0].size, 0u);
    EXPECT_EQ(obus[2].size, 1u);
}

TEST_F(Av1ObuExtractionTest, EmptyAndTruncatedInput) {
    EXPECT_TRUE(Extract(std::vector<uint8_t>{}).empty());

    // 声明了长度但数据被截断：能拿多少拿多少，不要崩
    const std::vector<uint8_t> truncated = {0x0A, 0x0E, 0x00, 0x00};
    std::vector<ObuUnit> obus = Extract(truncated);
    ASSERT_EQ(obus.size(), 1u);
    EXPECT_EQ(obus[0].size, 2u);

    // forbidden bit = 1 的字节不是合法 OBU 起始
    const std::vector<uint8_t> bad = {0x8A, 0x01, 0x00};
    EXPECT_TRUE(Extract(bad).empty());
}

// --------------------------------------------------------------------------
// Sequence Header 解析
// --------------------------------------------------------------------------
class Av1SequenceHeaderTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(Av1SequenceHeaderTest, Profile0_1080p_8bit) {
    videoeye::model::Av1SequenceHeaderInfo sh = ParseFirst(kSeqHdrProfile0_1080p);
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.profile, 0);
    EXPECT_EQ(sh.level, 4);                       // seq_level_idx：level 3.0
    EXPECT_EQ(sh.LevelString(), "3.0");
    EXPECT_EQ(sh.tier, 0);
    EXPECT_EQ(sh.FrameWidth(), 1920);
    EXPECT_EQ(sh.FrameHeight(), 1080);
    EXPECT_EQ(sh.BitDepth(), 8);
    EXPECT_FALSE(sh.color_config.monochrome);
    EXPECT_EQ(sh.color_config.subsampling_x, 1);
    EXPECT_EQ(sh.color_config.subsampling_y, 1);
    EXPECT_EQ(sh.ChromaFormatString(), "4:2:0");
    EXPECT_TRUE(sh.color_config.color_description_present_flag);
    EXPECT_EQ(sh.color_config.color_primaries, 1);         // BT.709
    EXPECT_EQ(sh.color_config.transfer_characteristics, 1);
    EXPECT_EQ(sh.color_config.matrix_coefficients, 1);
    EXPECT_EQ(sh.operating_points_count, 1);
    EXPECT_FALSE(sh.timing_info_present_flag);
}

TEST_F(Av1SequenceHeaderTest, Profile0_4k_10bit_Hdr) {
    videoeye::model::Av1SequenceHeaderInfo sh = ParseFirst(kSeqHdrProfile0_4k10bitHdr);
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.profile, 0);
    EXPECT_EQ(sh.level, 13);
    EXPECT_EQ(sh.LevelString(), "5.1");
    EXPECT_EQ(sh.tier, 1);                        // level 5.x 才带 seq_tier
    EXPECT_EQ(sh.FrameWidth(), 3840);
    EXPECT_EQ(sh.FrameHeight(), 2160);
    EXPECT_TRUE(sh.color_config.high_bitdepth);
    EXPECT_EQ(sh.BitDepth(), 10);
    EXPECT_EQ(sh.ChromaFormatString(), "4:2:0");
    EXPECT_EQ(sh.color_config.color_primaries, 9);         // BT.2020
    EXPECT_EQ(sh.color_config.transfer_characteristics, 16); // PQ
    EXPECT_EQ(sh.color_config.matrix_coefficients, 9);     // BT.2020 NCL
}

TEST_F(Av1SequenceHeaderTest, Profile2_422_12bit) {
    videoeye::model::Av1SequenceHeaderInfo sh = ParseFirst(kSeqHdrProfile2_422_12bit);
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.profile, 2);
    EXPECT_EQ(sh.level, 8);
    EXPECT_EQ(sh.LevelString(), "4.0");
    EXPECT_TRUE(sh.color_config.high_bitdepth);
    EXPECT_TRUE(sh.color_config.twelve_bit);
    EXPECT_EQ(sh.BitDepth(), 12);
    EXPECT_EQ(sh.color_config.subsampling_x, 1);
    EXPECT_EQ(sh.color_config.subsampling_y, 0);
    EXPECT_EQ(sh.ChromaFormatString(), "4:2:2");
    EXPECT_EQ(sh.FrameWidth(), 1920);
    EXPECT_EQ(sh.FrameHeight(), 1080);
}

TEST_F(Av1SequenceHeaderTest, Profile1_444_8bit) {
    videoeye::model::Av1SequenceHeaderInfo sh = ParseFirst(kSeqHdrProfile1_444);
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.profile, 1);
    EXPECT_EQ(sh.level, 1);
    EXPECT_EQ(sh.LevelString(), "2.1");
    EXPECT_EQ(sh.BitDepth(), 8);
    // profile 1 不读 monochrome，默认 4:4:4
    EXPECT_FALSE(sh.color_config.monochrome);
    EXPECT_EQ(sh.ChromaFormatString(), "4:4:4");
    EXPECT_EQ(sh.FrameWidth(), 640);
    EXPECT_EQ(sh.FrameHeight(), 480);
}

TEST_F(Av1SequenceHeaderTest, Monochrome) {
    videoeye::model::Av1SequenceHeaderInfo sh = ParseFirst(kSeqHdrMonochrome);
    ASSERT_TRUE(sh.present);
    EXPECT_TRUE(sh.color_config.monochrome);
    EXPECT_EQ(sh.color_config.full_range_flag, 1);
    EXPECT_EQ(sh.ChromaFormatString(), "4:0:0");
    EXPECT_EQ(sh.BitDepth(), 8);
    EXPECT_EQ(sh.FrameWidth(), 320);
    EXPECT_EQ(sh.FrameHeight(), 240);
}

TEST_F(Av1SequenceHeaderTest, TimingInfoAndOperatingPoints) {
    videoeye::model::Av1SequenceHeaderInfo sh = ParseFirst(kSeqHdrWithTiming);
    ASSERT_TRUE(sh.present);
    EXPECT_TRUE(sh.timing_info_present_flag);
    EXPECT_EQ(sh.num_units_in_tick, 1001u);
    EXPECT_EQ(sh.timescale, 60000u);
    EXPECT_EQ(sh.num_ticks_per_picture, 4u);
    EXPECT_TRUE(sh.decoder_model_info_present_flag);
    EXPECT_EQ(sh.decoder_buffer_delay_length, 16);
    EXPECT_EQ(sh.operating_points_count, 2);
    EXPECT_EQ(sh.level, 8);                       // level 4.0
    EXPECT_EQ(sh.LevelString(), "4.0");
    EXPECT_EQ(sh.tier, 1);
    // 跨过 timing / decoder model / 两个 operating point 之后仍要对齐到宽高
    EXPECT_EQ(sh.FrameWidth(), 1280);
    EXPECT_EQ(sh.FrameHeight(), 720);
}

TEST_F(Av1SequenceHeaderTest, WithExtensionHeader) {
    videoeye::model::Av1SequenceHeaderInfo sh = ParseFirst(kSeqHdrWithExtension);
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.level, 5);
    EXPECT_EQ(sh.LevelString(), "3.1");
    EXPECT_EQ(sh.FrameWidth(), 854);
    EXPECT_EQ(sh.FrameHeight(), 480);
}

TEST_F(Av1SequenceHeaderTest, ParseFromPayloadDirectly) {
    ObuUnit obu(1, kSeqHdrProfile0_1080p_Payload.size(),
                kSeqHdrProfile0_1080p_Payload, false, true);
    videoeye::model::Av1SequenceHeaderInfo sh = Av1BitstreamParser::ParseFromObuUnit(obu);
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.FrameWidth(), 1920);
    EXPECT_EQ(sh.FrameHeight(), 1080);
}

// --------------------------------------------------------------------------
// av1C 配置记录
// --------------------------------------------------------------------------
class Av1CodecConfigTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(Av1CodecConfigTest, Profile0_420_8bit) {
    auto result = ExtradataParser::ParseWithFormat(
        ExtradataFormat::Av1C, kAv1CProfile0_420_8bit.data(), kAv1CProfile0_420_8bit.size());
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.config.profile, 0);
    EXPECT_EQ(result.config.level_idc, 8u);       // seq_level_idx_0 = 4.0
    EXPECT_EQ(result.config.general_tier_flag, 0);
    EXPECT_EQ(result.config.high_bitdepth, 0);
    EXPECT_EQ(result.config.bit_depth_minus_8, 0);
    EXPECT_EQ(result.config.monochrome, 0);
    EXPECT_EQ(result.config.chroma_subsampling_x, 1);
    EXPECT_EQ(result.config.chroma_subsampling_y, 1);
    // av1C 里没有宽高，不应该像旧实现那样编出垃圾分辨率
    EXPECT_EQ(result.width, 0);
    EXPECT_EQ(result.height, 0);
    EXPECT_TRUE(result.obu_units.empty());
}

TEST_F(Av1CodecConfigTest, Profile0_420_10bit_Tier1) {
    auto result = ExtradataParser::ParseWithFormat(
        ExtradataFormat::Av1C, kAv1CProfile0_420_10bit.data(), kAv1CProfile0_420_10bit.size());
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.config.profile, 0);
    EXPECT_EQ(result.config.level_idc, 13u);      // 5.1
    EXPECT_EQ(result.config.general_tier_flag, 1);
    EXPECT_EQ(result.config.high_bitdepth, 1);
    EXPECT_EQ(result.config.bit_depth_minus_8, 2);
}

TEST_F(Av1CodecConfigTest, ConfigObusAreExtracted) {
    auto result = ExtradataParser::ParseWithFormat(
        ExtradataFormat::Av1C, kAv1CWithSeqHeader.data(), kAv1CWithSeqHeader.size());
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.obu_units.size(), 1u);
    EXPECT_EQ(result.obu_units[0].type, 1);

    videoeye::model::Av1SequenceHeaderInfo sh =
        Av1BitstreamParser::ParseFromObuUnit(result.obu_units[0]);
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.FrameWidth(), 1920);
    EXPECT_EQ(sh.FrameHeight(), 1080);
}

TEST_F(Av1CodecConfigTest, MonochromeAndInitialPresentationDelay) {
    auto result = ExtradataParser::ParseWithFormat(
        ExtradataFormat::Av1C, kAv1CMonochrome.data(), kAv1CMonochrome.size());
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.config.monochrome, 1);
    EXPECT_EQ(result.config.initial_presentation_delay_bits, 5);
}

TEST_F(Av1CodecConfigTest, TooSmallIsRejected) {
    const std::vector<uint8_t> tiny = {0x81, 0x08, 0x0C};
    auto result = ExtradataParser::ParseWithFormat(
        ExtradataFormat::Av1C, tiny.data(), tiny.size());
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error_message.empty());
}

// --------------------------------------------------------------------------
// 名称转换与异常输入
// --------------------------------------------------------------------------
class Av1MiscTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(Av1MiscTest, ProfileName) {
    EXPECT_EQ(Av1BitstreamParser::GetProfileName(0), "Profile 0");
    EXPECT_EQ(Av1BitstreamParser::GetProfileName(1), "Profile 1");
    EXPECT_EQ(Av1BitstreamParser::GetProfileName(2), "Profile 2");
    EXPECT_EQ(Av1BitstreamParser::GetProfileName(3), "Unknown");
}

TEST_F(Av1MiscTest, LevelString) {
    EXPECT_EQ(Av1BitstreamParser::GetLevelString(0), "2.0");
    EXPECT_EQ(Av1BitstreamParser::GetLevelString(1), "2.1");
    EXPECT_EQ(Av1BitstreamParser::GetLevelString(4), "3.0");
    EXPECT_EQ(Av1BitstreamParser::GetLevelString(13), "5.1");
    EXPECT_EQ(Av1BitstreamParser::GetLevelString(23), "7.3");
    EXPECT_EQ(Av1BitstreamParser::GetLevelString(24), "Unknown");
    EXPECT_EQ(Av1BitstreamParser::GetLevelString(-1), "Unknown");
}

TEST_F(Av1MiscTest, NonSequenceHeaderObuIsIgnored) {
    ObuUnit padding(15, 4, std::vector<uint8_t>{0x00, 0x00, 0x00, 0x00}, false, false);
    videoeye::model::Av1SequenceHeaderInfo sh = Av1BitstreamParser::ParseFromObuUnit(padding);
    EXPECT_FALSE(sh.present);
    EXPECT_FALSE(Av1BitstreamParser::IsSequenceHeaderObu(padding));
}

TEST_F(Av1MiscTest, EmptyPayloadIsIgnored) {
    ObuUnit empty(1, 0, std::vector<uint8_t>{}, false, true);
    EXPECT_FALSE(Av1BitstreamParser::ParseFromObuUnit(empty).present);
}

TEST_F(Av1MiscTest, ReservedProfileIsRejected) {
    // profile 字段 = 7，规范里是保留值
    std::vector<uint8_t> payload = kSeqHdrProfile0_1080p_Payload;
    payload[0] |= 0xE0;
    ObuUnit obu(1, payload.size(), payload, false, true);
    EXPECT_FALSE(Av1BitstreamParser::ParseFromObuUnit(obu).present);
}

TEST_F(Av1MiscTest, ParseSequenceHeaderFromExtradata) {
    videoeye::model::Av1SequenceHeaderInfo sh = Av1BitstreamParser::ParseSequenceHeader(
        kAv1CWithSeqHeader.data(), kAv1CWithSeqHeader.size());
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.FrameWidth(), 1920);

    // 没有序列头的纯 av1C 只能返回未解析，不能拿默认值冒充
    videoeye::model::Av1SequenceHeaderInfo none = Av1BitstreamParser::ParseSequenceHeader(
        kAv1CProfile0_420_8bit.data(), kAv1CProfile0_420_8bit.size());
    EXPECT_FALSE(none.present);

    EXPECT_FALSE(Av1BitstreamParser::ParseSequenceHeader(nullptr, 0).present);
}

}  // namespace
