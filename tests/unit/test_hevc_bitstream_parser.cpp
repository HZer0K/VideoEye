#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/analyzer/HevcBitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace {

using videoeye::analyzer::HevcBitstreamParser;
using videoeye::utils::NalUnit;

// 测试数据由 _smoke/gen_hevc_ps.py 按 HEVC 7.3.2.2 / 7.3.2.2.1 / 7.3.2.3.1 逐位生成。
// NalUnit::data 只含 RBSP payload（NAL header 已由 ExtradataParser 剥离）。

// VPS: Main 10 / Level 5.1 / 单层 / timing 1000@60000
const std::vector<uint8_t> kVpsMain10 = {
    0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x90, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x99, 0x27, 0x03, 0x00, 0x00, 0x03,
    0x03, 0xE8, 0x00, 0x00, 0xEA, 0x60, 0x60,
};

// SPS A: Main 10 / 5.1 / 1920x1080 / 4:2:0 10bit / VUI(BT.2020 + PQ + timing)
const std::vector<uint8_t> kSpsMain10_1080p = {
    0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x99, 0xA0, 0x03, 0xC0, 0x80, 0x10, 0xE4, 0xD9, 0x49,
    0xE4, 0x91, 0xB6, 0x4B, 0xB9, 0xA8, 0x48, 0x80, 0x4D, 0xB2, 0x80, 0x00,
    0x01, 0xF4, 0x00, 0x00, 0x75, 0x30, 0x04,
};

// SPS B: Main / 4.0 / 1920x1088（conformance window bottom 4 → 1080）/ 8bit / 无 VUI
const std::vector<uint8_t> kSpsMain_1080pCrop = {
    0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x78, 0xA0, 0x03, 0xC0, 0x80, 0x11, 0x07, 0xCB,
    0x94, 0x9E, 0x49, 0x1B, 0x64, 0xBB, 0x20,
};

// PPS: id=0 / sps_id=0 / l0=3 / l1=2 / init_qp=26 / deblocking on
const std::vector<uint8_t> kPpsMain10 = {
    0xC0, 0x35, 0x3E, 0x0C, 0xC6, 0x80,
};

NalUnit MakeNal(uint8_t type, const std::vector<uint8_t>& payload) {
    return NalUnit{type, payload.size(), payload, false, (type >= 32 && type <= 35)};
}

// --------------------------------------------------------------------------
// Profile / Tier / Level 名称转换
// --------------------------------------------------------------------------
class HevcProfileTierLevelTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcProfileTierLevelTest, GetProfileName) {
    EXPECT_EQ(HevcBitstreamParser::GetProfileName(0), "Main");
    EXPECT_EQ(HevcBitstreamParser::GetProfileName(1), "Main 10");
    EXPECT_EQ(HevcBitstreamParser::GetProfileName(2), "Main Still Picture");
    EXPECT_EQ(HevcBitstreamParser::GetProfileName(4), "High Throughput");
    EXPECT_EQ(HevcBitstreamParser::GetProfileName(999), "Unknown");
}

TEST_F(HevcProfileTierLevelTest, GetTierName) {
    EXPECT_EQ(HevcBitstreamParser::GetTierName(0), "Main");
    EXPECT_EQ(HevcBitstreamParser::GetTierName(1), "High");
}

TEST_F(HevcProfileTierLevelTest, GetLevelVersion) {
    // general_level_idc = 30 × 主版本 + 3 × 次版本
    EXPECT_EQ(HevcBitstreamParser::GetLevelVersion(153), "5.1");
    EXPECT_EQ(HevcBitstreamParser::GetLevelVersion(123), "4.1");
    EXPECT_EQ(HevcBitstreamParser::GetLevelVersion(120), "4.0");
    EXPECT_EQ(HevcBitstreamParser::GetLevelVersion(0), "Undefined");
}

// --------------------------------------------------------------------------
// NAL 类型判断
// --------------------------------------------------------------------------
class HevcNalUnitTypeTest : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(HevcNalUnitTypeTest, IsVpsNalUnit) {
    const NalUnit vps = MakeNal(32, kVpsMain10);
    EXPECT_TRUE(HevcBitstreamParser::IsVpsNalUnit(vps));
    EXPECT_FALSE(HevcBitstreamParser::IsSpsNalUnit(vps));
    EXPECT_FALSE(HevcBitstreamParser::IsPpsNalUnit(vps));
}

TEST_F(HevcNalUnitTypeTest, IsSpsNalUnit) {
    const NalUnit sps = MakeNal(33, kSpsMain10_1080p);
    EXPECT_TRUE(HevcBitstreamParser::IsSpsNalUnit(sps));
    EXPECT_FALSE(HevcBitstreamParser::IsVpsNalUnit(sps));
    EXPECT_FALSE(HevcBitstreamParser::IsPpsNalUnit(sps));
}

TEST_F(HevcNalUnitTypeTest, IsPpsNalUnit) {
    const NalUnit pps = MakeNal(34, kPpsMain10);
    EXPECT_TRUE(HevcBitstreamParser::IsPpsNalUnit(pps));
    EXPECT_FALSE(HevcBitstreamParser::IsVpsNalUnit(pps));
    EXPECT_FALSE(HevcBitstreamParser::IsSpsNalUnit(pps));
}

// --------------------------------------------------------------------------
// 真实 VPS / SPS / PPS 解析
// --------------------------------------------------------------------------
TEST(HevcVpsParsingTest, ParseMain10Vps) {
    const auto vps = HevcBitstreamParser::ParseVpsFromNalUnit(MakeNal(32, kVpsMain10));

    ASSERT_TRUE(vps.present);
    EXPECT_EQ(vps.vps_video_parameter_set_id, 0);
    EXPECT_EQ(vps.vps_max_layers, 1);
    EXPECT_EQ(vps.vps_max_sub_layers, 1);
    EXPECT_EQ(vps.vps_temporal_id_nesting_flag, 1);
    EXPECT_EQ(HevcBitstreamParser::GetProfileName(vps.general_profile_idc), "Main 10");
    EXPECT_EQ(vps.general_tier_flag, 0);
    EXPECT_EQ(vps.general_level_idc, 153u);
    EXPECT_EQ(HevcBitstreamParser::GetLevelVersion(vps.general_level_idc), "5.1");

    ASSERT_TRUE(vps.vps_timing_info_present_flag);
    EXPECT_EQ(vps.vps_num_units_in_tick, 1000u);
    EXPECT_EQ(vps.vps_time_scale, 60000u);
    EXPECT_FALSE(vps.vps_poc_proportional_to_timestamp_flag);
}

TEST(HevcSpfParsingTest, ParseMain10SpsWithVui) {
    const auto sps = HevcBitstreamParser::ParseSpfFromNalUnit(MakeNal(33, kSpsMain10_1080p));

    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.sps_seq_parameter_set_id, 0);
    EXPECT_EQ(sps.general_profile_idc, 1);
    EXPECT_EQ(sps.general_level_idc, 153u);
    EXPECT_EQ(sps.ProfileName(), "Main 10");

    EXPECT_EQ(sps.chroma_format_idc, 1);
    EXPECT_EQ(sps.pic_width_in_luma_samples, 1920);
    EXPECT_EQ(sps.pic_height_in_luma_samples, 1080);
    EXPECT_EQ(sps.bit_depth_luma_minus8, 2);
    EXPECT_EQ(sps.bit_depth_chroma_minus8, 2);
    EXPECT_EQ(sps.BitDepthLuma(), 10);
    EXPECT_EQ(sps.BitDepthChroma(), 10);
    EXPECT_EQ(sps.width(), 1920);
    EXPECT_EQ(sps.height(), 1080);

    EXPECT_EQ(sps.log2_max_pic_order_cnt_lsb_minus4, 4);
    EXPECT_EQ(sps.num_short_term_ref_pic_sets, 1);
    EXPECT_EQ(sps.sample_adaptive_offset_enabled_flag, 1);
    EXPECT_EQ(sps.amp_enabled_flag, 1);
    EXPECT_EQ(sps.temporal_mvp_enabled_flag, 1);

    // VUI
    ASSERT_TRUE(sps.vui_parameters_present_flag);
    EXPECT_EQ(sps.video_full_range_flag, 0);
    EXPECT_EQ(sps.colour_primaries, 9);
    EXPECT_EQ(sps.transfer_characteristics, 16);
    EXPECT_EQ(sps.matrix_coefficients, 9);
    EXPECT_EQ(sps.chroma_sample_loc_type_top_field, 2);
    EXPECT_EQ(sps.chroma_sample_loc_type_bottom_field, 2);
    EXPECT_TRUE(sps.frame_field_info_present_flag);
    ASSERT_TRUE(sps.vui_timing_info_present_flag);
    EXPECT_EQ(sps.vui_num_units_in_tick, 1000u);
    EXPECT_EQ(sps.vui_time_scale, 60000u);
}

TEST(HevcSpfParsingTest, ParseMainSpsWithConformanceWindow) {
    const auto sps = HevcBitstreamParser::ParseSpfFromNalUnit(MakeNal(33, kSpsMain_1080pCrop));

    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.general_profile_idc, 0);
    EXPECT_EQ(sps.general_level_idc, 120u);
    EXPECT_EQ(sps.ProfileName(), "Main");
    EXPECT_EQ(sps.bit_depth_luma_minus8, 0);
    EXPECT_EQ(sps.BitDepthLuma(), 8);

    // 1088 行里裁掉底部 4 个单元（SubHeightC=2）→ 1080
    ASSERT_TRUE(sps.conformance_window_flag);
    EXPECT_EQ(sps.conf_win_bottom_offset, 4);
    EXPECT_EQ(sps.pic_width_in_luma_samples, 1920);
    EXPECT_EQ(sps.pic_height_in_luma_samples, 1088);
    EXPECT_EQ(sps.width(), 1920);
    EXPECT_EQ(sps.height(), 1080);

    EXPECT_FALSE(sps.vui_parameters_present_flag);
}

TEST(HevcPpsParsingTest, ParseMain10Pps) {
    const auto pps = HevcBitstreamParser::ParsePpsFromNalUnit(MakeNal(34, kPpsMain10));

    ASSERT_TRUE(pps.present);
    EXPECT_EQ(pps.pic_parameter_set_id, 0);
    EXPECT_EQ(pps.seq_parameter_set_id, 0);
    EXPECT_EQ(pps.num_ref_idx_l0_default_active_minus1, 2);
    EXPECT_EQ(pps.num_ref_idx_l1_default_active_minus1, 1);
    EXPECT_EQ(pps.init_qp_minus26, 0);
    EXPECT_EQ(pps.cabac_init_present_flag, 0);
    EXPECT_EQ(pps.tiles_enabled_flag, 0);
    EXPECT_EQ(pps.deblocking_filter_control_present_flag, 1);
    EXPECT_EQ(pps.loop_filter_across_slices_enabled_flag, 1);
    EXPECT_EQ(pps.weighted_pred_flag, 0);
    EXPECT_EQ(pps.pps_slice_chroma_qp_offsets_present_flag, 1);
}

// --------------------------------------------------------------------------
// 异常输入
// --------------------------------------------------------------------------
TEST(HevcBitstreamParserTest, RejectInvalidNalUnits) {
    // 类型不匹配
    EXPECT_FALSE(HevcBitstreamParser::ParseSpfFromNalUnit(MakeNal(32, kVpsMain10)).present);
    EXPECT_FALSE(HevcBitstreamParser::ParseVpsFromNalUnit(MakeNal(33, kSpsMain10_1080p)).present);
    EXPECT_FALSE(HevcBitstreamParser::ParsePpsFromNalUnit(MakeNal(33, kSpsMain10_1080p)).present);

    // 空 payload
    EXPECT_FALSE(HevcBitstreamParser::ParseSpfFromNalUnit(MakeNal(33, {})).present);

    // 截断的 SPS（只有 2 字节）
    const std::vector<uint8_t> truncated = {0x01, 0x01};
    EXPECT_FALSE(HevcBitstreamParser::ParseSpfFromNalUnit(MakeNal(33, truncated)).present);
}

} // namespace
