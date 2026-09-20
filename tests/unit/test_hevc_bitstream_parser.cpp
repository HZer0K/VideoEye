#include <gtest/gtest.h>
#include "core/analyzer/HevcBitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace {

// H.265 Profile/Tier/Level 名称转换测试
class HevcProfileTierLevelTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcProfileTierLevelTest, GetProfileName) {
    // Main profile
    std::string profile_name = videoeye::analyzer::HevcBitstreamParser::GetProfileName(0);
    EXPECT_EQ(profile_name, "Main");
    
    // Main 10 profile
    profile_name = videoeye::analyzer::HevcBitstreamParser::GetProfileName(1);
    EXPECT_EQ(profile_name, "Main 10");
    
    // Range Extension
    profile_name = videoeye::analyzer::HevcBitstreamParser::GetProfileName(4);
    EXPECT_EQ(profile_name, "Range Extension");
    
    // Unknown profile
    profile_name = videoeye::analyzer::HevcBitstreamParser::GetProfileName(999);
    EXPECT_EQ(profile_name, "Unknown");
}

TEST_F(HevcProfileTierLevelTest, GetTierName) {
    // Main tier
    std::string tier_name = videoeye::analyzer::HevcBitstreamParser::GetTierName(0);
    EXPECT_EQ(tier_name, "Main");
    
    // High tier
    tier_name = videoeye::analyzer::HevcBitstreamParser::GetTierName(1);
    EXPECT_EQ(tier_name, "High");
}

TEST_F(HevcProfileTierLevelTest, GetLevelVersion) {
    // Level 5.1
    std::string level_version = videoeye::analyzer::HevcBitstreamParser::GetLevelVersion(153); // 5*30 + 3
    EXPECT_EQ(level_version, "5.3");
    
    // Level 4.1
    level_version = videoeye::analyzer::HevcBitstreamParser::GetLevelVersion(123); // 4*30 + 3
    EXPECT_EQ(level_version, "4.3");
    
    // Undefined
    level_version = videoeye::analyzer::HevcBitstreamParser::GetLevelVersion(0);
    EXPECT_EQ(level_version, "Undefined");
}

// NAL 单元类型判断测试
class HevcNalUnitTypeTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcNalUnitTypeTest, IsVpsNalUnit) {
    videoeye::utils::NalUnit vps_unit;
    vps_unit.type = 32; // VPS type
    
    EXPECT_TRUE(videoeye::analyzer::HevcBitstreamParser::IsVpsNalUnit(vps_unit));
    EXPECT_FALSE(videoeye::analyzer::HevcBitstreamParser::IsSpsNalUnit(vps_unit));
    EXPECT_FALSE(videoeye::analyzer::HevcBitstreamParser::IsPpsNalUnit(vps_unit));
}

TEST_F(HevcNalUnitTypeTest, IsSpsNalUnit) {
    videoeye::utils::NalUnit sps_unit;
    sps_unit.type = 33; // SPS type
    
    EXPECT_TRUE(videoeye::analyzer::HevcBitstreamParser::IsSpsNalUnit(sps_unit));
    EXPECT_FALSE(videoeye::analyzer::HevcBitstreamParser::IsVpsNalUnit(sps_unit));
    EXPECT_FALSE(videoeye::analyzer::HevcBitstreamParser::IsPpsNalUnit(sps_unit));
}

TEST_F(HevcNalUnitTypeTest, IsPpsNalUnit) {
    videoeye::utils::NalUnit pps_unit;
    pps_unit.type = 34; // PPS type
    
    EXPECT_TRUE(videoeye::analyzer::HevcBitstreamParser::IsPpsNalUnit(pps_unit));
    EXPECT_FALSE(videoeye::analyzer::HevcBitstreamParser::IsVpsNalUnit(pps_unit));
    EXPECT_FALSE(videoeye::analyzer::HevcBitstreamParser::IsSpsNalUnit(pps_unit));
}

// ExtradataParser hvcC 格式检测测试
class HevcExtradataParserTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcExtradataParserTest, DetectHvcCFormat) {
    // 构造一个简化的 hvcC 配置记录
    std::vector<uint8_t> hvcc_data;
    
    // Box size (20 bytes minimum)
    hvcc_data.push_back(0);
    hvcc_data.push_back(0);
    hvcc_data.push_back(0);
    hvcc_data.push_back(20);
    
    // Box type: "hvcC"
    hvcc_data.push_back('h');
    hvcc_data.push_back('v');
    hvcc_data.push_back('c');
    hvcc_data.push_back('C');
    
    auto result = videoeye::utils::ExtradataParser::Parse(
        hvcc_data.data(), hvcc_data.size());
    
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, videoeye::utils::ExtradataFormat::HvcC);
}

// H.265 SPS 解析测试
class HevcSpfParsingTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcSpfParsingTest, ParseBasicSps) {
    // 这是一个简化的 SPS NAL 单元，仅用于测试基本解析流程
    std::vector<uint8_t> sps_nal;
    
    // HEVC NAL header: forbidden_zero_bit=0, nuh_reserved_zero_2bits=0, nal_unit_type=33 (SPS)
    sps_nal.push_back(0x00);
    sps_nal.push_back(0xA8); // 1010 1000 -> nal_unit_type=33
    
    // sps_seq_parameter_set_id = 0
    sps_nal.push_back(0x00);
    
    auto nal_unit = videoeye::analyzer::HevcBitstreamParser::ParseSpfFromNalUnit(
        videoeye::utils::NalUnit{33, sps_nal.size(), sps_nal, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.sps_seq_parameter_set_id, 0);
}

// Chroma format 解析测试
TEST_F(HevcSpfParsingTest, ParseChromaFormat) {
    std::vector<uint8_t> sps_420;
    
    // NAL header
    sps_420.push_back(0x00);
    sps_420.push_back(0xA8);
    
    // sps_seq_parameter_set_id
    sps_420.push_back(0x00);
    
    // chroma_format_idc = 1 (4:2:0)
    sps_420.push_back(0x01);
    
    auto nal_unit = videoeye::analyzer::HevcBitstreamParser::ParseSpfFromNalUnit(
        videoeye::utils::NalUnit{33, sps_420.size(), sps_420, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.chroma_format_idc, 1);
}

// CTU size 计算测试
TEST_F(HevcSpfParsingTest, CalculateCTUSize) {
    std::vector<uint8_t> sps_ctu;
    
    // NAL header
    sps_ctu.push_back(0x00);
    sps_ctu.push_back(0xA8);
    
    // sps_seq_parameter_set_id
    sps_ctu.push_back(0x00);
    
    // chroma_format_idc
    sps_ctu.push_back(0x01);
    
    // pic_width_in_ctu_minus1 = 23 (1520px / 64 = 23.75, so 23)
    sps_ctu.push_back(0x17);
    
    // pic_height_in_ctu_minus1 = 16 (864px / 64 = 13.5, so 16 for safety)
    sps_ctu.push_back(0x10);
    
    auto nal_unit = videoeye::analyzer::HevcBitstreamParser::ParseSpfFromNalUnit(
        videoeye::utils::NalUnit{33, sps_ctu.size(), sps_ctu, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.width(), 1536); // (23+1) * 64
    EXPECT_EQ(nal_unit.height(), 1088); // (16+1) * 64
}

// Bit depth 解析测试
TEST_F(HevcSpfParsingTest, ParseBitDepth) {
    std::vector<uint8_t> sps_10bit;
    
    // NAL header
    sps_10bit.push_back(0x00);
    sps_10bit.push_back(0xA8);
    
    // sps_seq_parameter_set_id
    sps_10bit.push_back(0x00);
    
    // chroma_format_idc
    sps_10bit.push_back(0x01);
    
    // pic_width_in_ctu_minus1
    sps_10bit.push_back(0x17);
    
    // pic_height_in_ctu_minus1
    sps_10bit.push_back(0x10);
    
    // bit_depth_luma_minus8 = 2 (10-bit video)
    sps_10bit.push_back(0x02);
    
    // bit_depth_chroma_minus8 = 2 (10-bit chroma)
    sps_10bit.push_back(0x02);
    
    auto nal_unit = videoeye::analyzer::HevcBitstreamParser::ParseSpfFromNalUnit(
        videoeye::utils::NalUnit{33, sps_10bit.size(), sps_10bit, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.BitDepthLuma(), 10); // 2 + 8
    EXPECT_EQ(nal_unit.BitDepthChroma(), 10); // 2 + 8
}

// VUI 信息解析测试
TEST_F(HevcSpfParsingTest, ParseVuiParameters) {
    std::vector<uint8_t> sps_with_vui;
    
    // NAL header
    sps_with_vui.push_back(0x00);
    sps_with_vui.push_back(0xA8);
    
    // sps_seq_parameter_set_id
    sps_with_vui.push_back(0x00);
    
    // chroma_format_idc
    sps_with_vui.push_back(0x01);
    
    // pic_width_in_ctu_minus1
    sps_with_vui.push_back(0x17);
    
    // pic_height_in_ctu_minus1
    sps_with_vui.push_back(0x10);
    
    // bit_depth_luma_minus8
    sps_with_vui.push_back(0x02);
    
    // bit_depth_chroma_minus8
    sps_with_vui.push_back(0x02);
    
    // log2_min_luma_coding_block_size_minus3 = 0
    sps_with_vui.push_back(0x00);
    
    // log2_diff_max_min_luma_coding_block_size = 0
    sps_with_vui.push_back(0x00);
    
    // scaling_list_enable_flag = 0
    sps_with_vui.push_back(0x00);
    
    // transform_skip_enabled_flag = 0
    sps_with_vui.push_back(0x00);
    
    // sample_adaptive_offset_enabled_flag = 1
    sps_with_vui.push_back(0x01);
    
    // vui_parameters_present_flag = 1
    sps_with_vui.push_back(0x01);
    
    auto nal_unit = videoeye::analyzer::HevcBitstreamParser::ParseSpfFromNalUnit(
        videoeye::utils::NalUnit{33, sps_with_vui.size(), sps_with_vui, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_TRUE(nal_unit.vui_parameters_present_flag);
}

// PPS 解析测试
class HevcPpsParsingTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcPpsParsingTest, ParseBasicPps) {
    std::vector<uint8_t> pps_nal;
    
    // NAL header
    pps_nal.push_back(0x00);
    pps_nal.push_back(0xD0); // 1101 0000 -> nal_unit_type=34 (PPS)
    
    // pic_parameter_set_id
    pps_nal.push_back(0x00);
    
    // seq_parameter_set_id
    pps_nal.push_back(0x00);
    
    auto nal_unit = videoeye::analyzer::HevcBitstreamParser::ParsePpsFromNalUnit(
        videoeye::utils::NalUnit{34, pps_nal.size(), pps_nal, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.pic_parameter_set_id, 0);
    EXPECT_EQ(nal_unit.seq_parameter_set_id, 0);
}

// VPS 解析测试
class HevcVpsParsingTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcVpsParsingTest, ParseBasicVps) {
    std::vector<uint8_t> vps_nal;
    
    // NAL header
    vps_nal.push_back(0x00);
    vps_nal.push_back(0x80); // 1000 0000 -> nal_unit_type=32 (VPS)
    
    // vps_video_parameter_set_id
    vps_nal.push_back(0x00);
    
    // vps_max_layers
    vps_nal.push_back(0x01);
    
    // vps_max_sub_layers
    vps_nal.push_back(0x01);
    
    auto nal_unit = videoeye::analyzer::HevcBitstreamParser::ParseVpsFromNalUnit(
        videoeye::utils::NalUnit{32, vps_nal.size(), vps_nal, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.vps_video_parameter_set_id, 0);
    EXPECT_EQ(nal_unit.vps_max_layers, 1);
}

} // namespace
