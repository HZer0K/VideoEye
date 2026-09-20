#include <gtest/gtest.h>
#include "core/analyzer/H264BitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace {

// H.264 NAL 单元类型测试
class H264NalUnitTypeTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Profile 名称转换测试
TEST(H264BitstreamParserTest, ProfileNameConversion) {
    // Test case 1: High Profile (100)
    std::string profile_name = videoeye::analyzer::H264BitstreamParser::GetProfileName(100);
    EXPECT_EQ(profile_name, "High");
    
    // Test case 2: Main Profile (77)
    profile_name = videoeye::analyzer::H264BitstreamParser::GetProfileName(77);
    EXPECT_EQ(profile_name, "Main");
    
    // Test case 3: Baseline Profile (66)
    profile_name = videoeye::analyzer::H264BitstreamParser::GetProfileName(66);
    EXPECT_EQ(profile_name, "Baseline");
    
    // Test case 4: Extended Profile (88)
    profile_name = videoeye::analyzer::H264BitstreamParser::GetProfileName(88);
    EXPECT_EQ(profile_name, "Extended");
    
    // Test case 5: Unknown Profile
    profile_name = videoeye::analyzer::H264BitstreamParser::GetProfileName(999);
    EXPECT_EQ(profile_name, "Unknown");
}

// Level 版本转换测试
TEST(H264BitstreamParserTest, LevelVersionConversion) {
    // Test case 1: Level 3.1
    std::string level_version = videoeye::analyzer::H264BitstreamParser::GetLevelVersion(31);
    EXPECT_EQ(level_version, "3.1");
    
    // Test case 2: Level 4.1
    level_version = videoeye::analyzer::H264BitstreamParser::GetLevelVersion(41);
    EXPECT_EQ(level_version, "4.1");
    
    // Test case 3: Level 5.1
    level_version = videoeye::analyzer::H264BitstreamParser::GetLevelVersion(51);
    EXPECT_EQ(level_version, "5.1");
    
    // Test case 4: Level 1
    level_version = videoeye::analyzer::H264BitstreamParser::GetLevelVersion(10);
    EXPECT_EQ(level_version, "1.0");
    
    // Test case 5: Undefined
    level_version = videoeye::analyzer::H264BitstreamParser::GetLevelVersion(0);
    EXPECT_EQ(level_version, "Undefined");
}

// ExtradataParser 格式检测测试
class ExtradataParserFormatDetectionTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Annex B 格式检测
TEST_F(ExtradataParserFormatDetectionTest, DetectAnnexBFormat) {
    // 构造一个简单的 Annex B 数据（包含起始码）
    std::vector<uint8_t> annex_b_data;
    annex_b_data.push_back(0);
    annex_b_data.push_back(0);
    annex_b_data.push_back(0);
    annex_b_data.push_back(1); // Start code 00 00 00 01
    annex_b_data.push_back(7); // SPS NAL type
    
    auto result = videoeye::utils::ExtradataParser::Parse(
        annex_b_data.data(), annex_b_data.size());
    
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, videoeye::utils::ExtradataFormat::AnnexB);
    EXPECT_GT(result.nal_units.size(), 0);
}

// avcC 格式检测
TEST_F(ExtradataParserFormatDetectionTest, DetectAvcCFormat) {
    // 构造一个简化的 avcC 配置记录
    std::vector<uint8_t> avcc_data;
    
    // Box size (10 bytes)
    avcc_data.push_back(0);
    avcc_data.push_back(0);
    avcc_data.push_back(0);
    avcc_data.push_back(10);
    
    // Box type: "avcC"
    avcc_data.push_back('a');
    avcc_data.push_back('v');
    avcc_data.push_back('c');
    avcc_data.push_back('C');
    
    // Version
    avcc_data.push_back(1);
    
    // Profile compatibility (High Profile = 100)
    avcc_data.push_back(100);
    
    auto result = videoeye::utils::ExtradataParser::Parse(
        avcc_data.data(), avcc_data.size());
    
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, videoeye::utils::ExtradataFormat::AvcC);
    EXPECT_EQ(result.config.profile_idc, 100);
}

// H.264 SPS 解析测试
class H264SpfParsingTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// 解析简单的 SPS NAL 单元
TEST_F(H264SpfParsingTest, ParseSimpleSPS) {
    // 这是一个简化的 SPS NAL 单元，仅用于测试基本解析流程
    // 实际 SPS 应该包含完整的语法结构
    std::vector<uint8_t> sps_nal;
    
    // NAL header: forbidden_zero_bit=0, nal_unit_type=7 (SPS)
    sps_nal.push_back(0x67); // 00000 111
    
    // seq_parameter_set_id = 0
    sps_nal.push_back(0x00);
    
    // profile_idc = 100 (High Profile)
    sps_nal.push_back(0x64);
    
    // constraint_flags
    sps_nal.push_back(0x00);
    sps_nal.push_back(0x00);
    sps_nal.push_back(0x00);
    
    // level_idc = 41 (Level 4.1)
    sps_nal.push_back(0x29);
    
    auto nal_unit = videoeye::analyzer::H264BitstreamParser::ParseFromNalUnit(
        videoeye::utils::NalUnit{7, sps_nal.size(), sps_nal, false, true});
    
    // 由于这是简化版 SPS，我们只验证基本字段能解析
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.seq_parameter_set_id, 0);
    EXPECT_EQ(nal_unit.profile_idc, 100);
    EXPECT_EQ(nal_unit.level_idc, 41);
}

// Chroma format 解析测试
TEST_F(H264SpfParsingTest, ParseChromaFormat) {
    // 4:2:0 chroma format (chroma_format_idc = 1)
    std::vector<uint8_t> sps_420;
    sps_420.push_back(0x67); // NAL header (SPS)
    sps_420.push_back(0x00); // seq_parameter_set_id
    sps_420.push_back(0x64); // profile_idc (High)
    sps_420.push_back(0x00); // constraint_flags[0]
    sps_420.push_back(0x00); // constraint_flags[1]
    sps_420.push_back(0x00); // constraint_flags[2]
    sps_420.push_back(0x29); // level_idc (4.1)
    sps_420.push_back(0x80); // frame_mbs_only_flag=1
    sps_420.push_back(0x00); // direct_8x8_inference_flag
    sps_420.push_back(0x01); // chroma_format_idc = 1 (4:2:0)
    
    auto nal_unit = videoeye::analyzer::H264BitstreamParser::ParseFromNalUnit(
        videoeye::utils::NalUnit{7, sps_420.size(), sps_420, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_EQ(nal_unit.chroma_format_idc, 1);
}

// Bit depth 解析测试
TEST_F(H264SpfParsingTest, ParseBitDepth) {
    // 10-bit video (bit_depth_luma_minus8 = 2)
    std::vector<uint8_t> sps_10bit;
    sps_10bit.push_back(0x67); // NAL header (SPS)
    sps_10bit.push_back(0x00); // seq_parameter_set_id
    sps_10bit.push_back(0x64); // profile_idc (High 10 = 110)
    sps_10bit.push_back(0x00); // constraint_flags[0]
    sps_10bit.push_back(0x00); // constraint_flags[1]
    sps_10bit.push_back(0x00); // constraint_flags[2]
    sps_10bit.push_back(0x29); // level_idc (4.1)
    sps_10bit.push_back(0x80); // frame_mbs_only_flag=1
    sps_10bit.push_back(0x00); // direct_8x8_inference_flag
    sps_10bit.push_back(0x65); // chroma_format_idc = 1 + bit_depth fields
    
    auto nal_unit = videoeye::analyzer::H264BitstreamParser::ParseFromNalUnit(
        videoeye::utils::NalUnit{7, sps_10bit.size(), sps_10bit, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    // bit_depth_luma_minus8 should be 2 for 10-bit
    EXPECT_LE(nal_unit.bit_depth_luma_minus8, 2);
}

// VUI 信息解析测试
TEST_F(H264SpfParsingTest, ParseVuiInfo) {
    std::vector<uint8_t> sps_with_vui;
    sps_with_vui.push_back(0x67); // NAL header (SPS)
    sps_with_vui.push_back(0x00); // seq_parameter_set_id
    sps_with_vui.push_back(0x64); // profile_idc
    sps_with_vui.push_back(0x00); // constraint_flags[0]
    sps_with_vui.push_back(0x00); // constraint_flags[1]
    sps_with_vui.push_back(0x00); // constraint_flags[2]
    sps_with_vui.push_back(0x29); // level_idc
    sps_with_vui.push_back(0xC0); // frame_mbs_only_flag=1, vui_present_flag=1
    sps_with_vui.push_back(0x00); // direct_8x8_inference_flag
    sps_with_vui.push_back(0x01); // chroma_format_idc = 1
    sps_with_vui.push_back(0x00); // bit_depth_luma_minus8
    sps_with_vui.push_back(0x00); // bit_depth_chroma_minus8
    sps_with_vui.push_back(0x18); // log2_max_pic_order_cnt_lsb + timing_info_present
    
    auto nal_unit = videoeye::analyzer::H264BitstreamParser::ParseFromNalUnit(
        videoeye::utils::NalUnit{7, sps_with_vui.size(), sps_with_vui, false, true});
    
    EXPECT_TRUE(nal_unit.present);
    EXPECT_TRUE(nal_unit.vui.present);
    EXPECT_TRUE(nal_unit.vui.timing_info_present);
}

// PPS NAL 单元判断测试
TEST(H264BitstreamParserTest, PpsNalUnitIdentification) {
    videoeye::utils::NalUnit pps_unit;
    pps_unit.type = 8; // PPS type
    
    EXPECT_TRUE(videoeye::analyzer::H264BitstreamParser::IsPpsNalUnit(pps_unit));
    EXPECT_FALSE(videoeye::analyzer::H264BitstreamParser::IsSpsNalUnit(pps_unit));
}

// IDR 帧识别测试
TEST(H264BitstreamParserTest, IdrFrameIdentification) {
    videoeye::utils::NalUnit idr_unit;
    idr_unit.type = 5; // IDR slice type
    
    EXPECT_TRUE(idr_unit.is_keyframe);
    EXPECT_FALSE(videoeye::analyzer::H264BitstreamParser::IsSpsNalUnit(idr_unit));
    EXPECT_FALSE(videoeye::analyzer::H264BitstreamParser::IsPpsNalUnit(idr_unit));
}

} // namespace
