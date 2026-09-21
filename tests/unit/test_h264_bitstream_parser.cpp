#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/analyzer/H264BitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace {

using videoeye::analyzer::H264BitstreamParser;
using videoeye::utils::NalUnit;

// 测试数据由 _smoke/gen_h264_sps.py 按 H.264 7.3.2.1.1 / 附录 E 逐位生成，
// 不是手工凑的常量。每个数组已带 emulation prevention 字节。
//
// 注意 NalUnit::data 的不变量：只含 RBSP payload，不含 1 字节 NAL header。

// A: High / Level 3.1 / 1280x720 / 4:2:0 8bit / VUI(timing + colour + restriction)
const std::vector<uint8_t> kSpsHigh720p = {
    0x64, 0x00, 0x1F, 0xAC, 0x72, 0x14, 0x05, 0x00, 0x5B, 0xA6, 0xA0, 0x20,
    0x20, 0x28, 0x00, 0x00, 0x1F, 0x40, 0x00, 0x07, 0x53, 0x04, 0x78, 0xB1,
    0x6C, 0xB0,
};

// B: High 10 / Level 5.1 / 1920x1088（crop bottom 4 → 1080）/ 4:2:0 10bit / 无 VUI
const std::vector<uint8_t> kSpsHigh10_1080p = {
    0x6E, 0x00, 0x33, 0xA6, 0xCC, 0xA4, 0x01, 0xE0, 0x08, 0x9F, 0x95,
};

// C: Main / Level 3.0 / 320x240 / 无 High 扩展字段 / VUI 仅 aspect_ratio
const std::vector<uint8_t> kSpsMain320x240 = {
    0x4D, 0x00, 0x1E, 0xA6, 0xC1, 0x41, 0xFB, 0x01, 0x00, 0x80,
};

// D: High / Level 4.0 / 176x144 / VUI timing 全 0（刻意制造 0x000003 反转义场景）
const std::vector<uint8_t> kSpsQcifEscaped = {
    0x64, 0x00, 0x28, 0xAC, 0xDA, 0x0B, 0x13, 0xA1, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x04,
};

// E: PPS —— id=0 / sps_id=0 / CABAC / l0=3 帧 / l1=2 帧 / bipred=2 / deblocking on
const std::vector<uint8_t> kPpsBasic = {
    0xEB, 0x4B, 0x2C, 0x80,
};

NalUnit MakeSps(const std::vector<uint8_t>& payload) {
    return NalUnit{7, payload.size(), payload, false, true};
}

NalUnit MakePps(const std::vector<uint8_t>& payload) {
    return NalUnit{8, payload.size(), payload, false, false};
}

// --------------------------------------------------------------------------
// Profile / Level 名称转换
// --------------------------------------------------------------------------
TEST(H264BitstreamParserTest, ProfileNameConversion) {
    EXPECT_EQ(H264BitstreamParser::GetProfileName(100), "High");
    EXPECT_EQ(H264BitstreamParser::GetProfileName(110), "High 10");
    EXPECT_EQ(H264BitstreamParser::GetProfileName(122), "High 4:2:2");
    EXPECT_EQ(H264BitstreamParser::GetProfileName(77), "Main");
    EXPECT_EQ(H264BitstreamParser::GetProfileName(66), "Baseline");
    EXPECT_EQ(H264BitstreamParser::GetProfileName(88), "Extended");
    EXPECT_EQ(H264BitstreamParser::GetProfileName(999), "Unknown");
}

TEST(H264BitstreamParserTest, LevelVersionConversion) {
    EXPECT_EQ(H264BitstreamParser::GetLevelVersion(31), "3.1");
    EXPECT_EQ(H264BitstreamParser::GetLevelVersion(41), "4.1");
    EXPECT_EQ(H264BitstreamParser::GetLevelVersion(51), "5.1");
    EXPECT_EQ(H264BitstreamParser::GetLevelVersion(10), "1.0");
    EXPECT_EQ(H264BitstreamParser::GetLevelVersion(9), "1b");
    EXPECT_EQ(H264BitstreamParser::GetLevelVersion(0), "Undefined");
}

// --------------------------------------------------------------------------
// ExtradataParser 格式检测
// --------------------------------------------------------------------------
TEST(ExtradataParserFormatDetectionTest, DetectAnnexBFormat) {
    const std::vector<uint8_t> annex_b_data = {0, 0, 0, 1, 7};

    auto result = videoeye::utils::ExtradataParser::Parse(
        annex_b_data.data(), annex_b_data.size());

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, videoeye::utils::ExtradataFormat::AnnexB);
    EXPECT_GT(result.nal_units.size(), 0u);
}

TEST(ExtradataParserFormatDetectionTest, DetectAvcCFormat) {
    // 真实 avcC（FFmpeg 交出的 extradata 不含 box header）
    const std::vector<uint8_t> avcc_data = {
        0x01,                                // configurationVersion
        0x64, 0x00, 0x29,                    // profile=100(High) compat level=4.1
        0xFF,                                // reserved(6b)=1 + lengthSizeMinusOne=3
        0xE1,                                // reserved(3b) + numSPS=1
        0x00, 0x04, 0x67, 0x64, 0x00, 0x29,  // 4 字节 SPS（含 NAL header）
        0x01,                                // numPPS=1
        0x00, 0x02, 0x68, 0xEB,              // 2 字节 PPS（含 NAL header）
    };

    auto result = videoeye::utils::ExtradataParser::Parse(
        avcc_data.data(), avcc_data.size());

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.format, videoeye::utils::ExtradataFormat::AvcC);
    EXPECT_EQ(result.config.profile_idc, 100);
    EXPECT_EQ(result.config.level_idc, 41u);
    EXPECT_EQ(result.config.length_size_minus_one, 3u);
    ASSERT_EQ(result.nal_units.size(), 2u);
    EXPECT_EQ(result.nal_units[0].type, 7);  // SPS
    EXPECT_EQ(result.nal_units[1].type, 8);  // PPS
    // NAL header 已被剥离：4 字节 SPS → 3 字节 payload，2 字节 PPS → 1 字节
    EXPECT_EQ(result.nal_units[0].size, 3u);
    EXPECT_EQ(result.nal_units[1].size, 1u);
    EXPECT_EQ(result.nal_units[0].data.size(), 3u);
}

// --------------------------------------------------------------------------
// 真实 SPS 解析
// --------------------------------------------------------------------------
TEST(H264SpfParsingTest, ParseHighProfile720p) {
    const auto sps = H264BitstreamParser::ParseFromNalUnit(MakeSps(kSpsHigh720p));

    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.profile_idc, 100);
    EXPECT_EQ(sps.level_idc, 31);
    EXPECT_EQ(sps.ProfileName(), "High");
    EXPECT_EQ(sps.LevelVersion(), "3.1");
    EXPECT_EQ(sps.seq_parameter_set_id, 0);
    EXPECT_EQ(sps.chroma_format_idc, 1);
    EXPECT_EQ(sps.BitDepthLuma(), 8);
    EXPECT_EQ(sps.BitDepthChroma(), 8);
    EXPECT_EQ(sps.log2_max_frame_num_minus4, 2);
    EXPECT_EQ(sps.pic_order_cnt_type, 0);
    EXPECT_EQ(sps.log2_max_pic_order_cnt_lsb_minus4, 3);
    EXPECT_EQ(sps.max_num_ref_frames, 4);
    EXPECT_EQ(sps.pic_width_in_mbs_minus1, 79);
    EXPECT_EQ(sps.pic_height_in_mbs_minus1, 44);
    EXPECT_TRUE(sps.frame_mbs_only_flag);
    EXPECT_EQ(sps.width(), 1280);
    EXPECT_EQ(sps.height(), 720);

    // VUI
    ASSERT_TRUE(sps.vui.present);
    EXPECT_TRUE(sps.vui.video_signal_type_present);
    EXPECT_TRUE(sps.vui.colour_description_present);
    EXPECT_EQ(sps.vui.color_primaries, 1);
    EXPECT_EQ(sps.vui.transfer_characteristics, 1);
    EXPECT_EQ(sps.vui.matrix_coefficients, 1);
    EXPECT_EQ(sps.vui.color_range, 0);

    ASSERT_TRUE(sps.vui.timing_info_present);
    EXPECT_EQ(sps.vui.num_units_in_tick, 1000u);
    EXPECT_EQ(sps.vui.time_scale, 60000u);
    EXPECT_TRUE(sps.vui.fixed_frame_rate);

    EXPECT_TRUE(sps.vui.bitstream_restriction_flag);
    EXPECT_EQ(sps.vui.max_num_reorder_frames, 2);
    EXPECT_EQ(sps.vui.max_dec_frame_buffering, 4);
    EXPECT_EQ(sps.vui.log2_max_mv_length_horizontal, 10);
    EXPECT_EQ(sps.vui.log2_max_mv_length_vertical, 10);
}

TEST(H264SpfParsingTest, ParseHigh10ProfileWithCropping) {
    const auto sps = H264BitstreamParser::ParseFromNalUnit(MakeSps(kSpsHigh10_1080p));

    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.profile_idc, 110);
    EXPECT_EQ(sps.level_idc, 51);
    EXPECT_EQ(sps.ProfileName(), "High 10");
    EXPECT_EQ(sps.LevelVersion(), "5.1");
    EXPECT_EQ(sps.chroma_format_idc, 1);
    EXPECT_EQ(sps.bit_depth_luma_minus8, 2);
    EXPECT_EQ(sps.bit_depth_chroma_minus8, 2);
    EXPECT_EQ(sps.BitDepthLuma(), 10);
    EXPECT_EQ(sps.BitDepthChroma(), 10);
    EXPECT_EQ(sps.max_num_ref_frames, 3);
    EXPECT_EQ(sps.log2_max_pic_order_cnt_lsb_minus4, 4);

    // 1088 行里裁掉底部 4 个单元（CropUnitY = 2 * (2-1) = 2）→ 1080
    ASSERT_TRUE(sps.frame_cropping_flag);
    EXPECT_EQ(sps.frame_crop_bottom_offset, 4);
    EXPECT_EQ(sps.width(), 1920);
    EXPECT_EQ(sps.height(), 1080);

    EXPECT_FALSE(sps.vui.present);
}

TEST(H264SpfParsingTest, ParseMainProfileWithoutChromaExtension) {
    const auto sps = H264BitstreamParser::ParseFromNalUnit(MakeSps(kSpsMain320x240));

    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.profile_idc, 77);
    EXPECT_EQ(sps.level_idc, 30);
    EXPECT_EQ(sps.ProfileName(), "Main");
    EXPECT_EQ(sps.LevelVersion(), "3.0");
    // Main profile 不带扩展字段，规范隐含 4:2:0 / 8bit
    EXPECT_EQ(sps.chroma_format_idc, 1);
    EXPECT_EQ(sps.bit_depth_luma_minus8, 0);
    EXPECT_EQ(sps.BitDepthLuma(), 8);
    EXPECT_EQ(sps.pic_order_cnt_type, 2);
    EXPECT_EQ(sps.max_num_ref_frames, 2);
    EXPECT_EQ(sps.width(), 320);
    EXPECT_EQ(sps.height(), 240);

    ASSERT_TRUE(sps.vui.present);
    EXPECT_TRUE(sps.vui.aspect_ratio_info_present);
    EXPECT_EQ(sps.vui.aspect_ratio_idc, 1);
    EXPECT_FALSE(sps.vui.timing_info_present);
    EXPECT_FALSE(sps.vui.colour_description_present);
}

TEST(H264SpfParsingTest, UnescapeEmulationPreventionBytes) {
    // 原始 RBSP 里 timing 字段全是 0，编码时必然插入 0x03
    ASSERT_NE(kSpsQcifEscaped.end(),
              std::find(kSpsQcifEscaped.begin(), kSpsQcifEscaped.end(), 0x03));

    const auto sps = H264BitstreamParser::ParseFromNalUnit(MakeSps(kSpsQcifEscaped));

    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.profile_idc, 100);
    EXPECT_EQ(sps.level_idc, 40);
    EXPECT_EQ(sps.width(), 176);
    EXPECT_EQ(sps.height(), 144);
    EXPECT_EQ(sps.max_num_ref_frames, 1);
    ASSERT_TRUE(sps.vui.timing_info_present);
    EXPECT_EQ(sps.vui.num_units_in_tick, 0u);
    EXPECT_EQ(sps.vui.time_scale, 0u);
    EXPECT_FALSE(sps.vui.fixed_frame_rate);
}

// --------------------------------------------------------------------------
// PPS
// --------------------------------------------------------------------------
TEST(H264PpsParsingTest, ParseBasicPps) {
    const auto pps = H264BitstreamParser::ParsePpsFromNalUnit(MakePps(kPpsBasic));

    ASSERT_TRUE(pps.present);
    EXPECT_EQ(pps.pic_parameter_set_id, 0);
    EXPECT_EQ(pps.seq_parameter_set_id, 0);
    EXPECT_EQ(pps.num_ref_idx_l0_default_active_minus1, 2);
    EXPECT_EQ(pps.num_ref_idx_l1_default_active_minus1, 1);
    EXPECT_FALSE(pps.weighted_pred_flag);
    EXPECT_EQ(pps.weighted_bipred_idc, 2);
    EXPECT_EQ(pps.pic_init_qp_minus26, 0);
    EXPECT_TRUE(pps.deblocking_filter_control_present_flag);
    EXPECT_FALSE(pps.constrained_intra_pred_flag);
    EXPECT_EQ(pps.redundant_pic_cnt_present_flag, 0);
}

// --------------------------------------------------------------------------
// 异常输入
// --------------------------------------------------------------------------
TEST(H264BitstreamParserTest, RejectInvalidNalUnits) {
    const NalUnit wrong_type{5, kSpsHigh720p.size(), kSpsHigh720p, true, true};
    EXPECT_FALSE(H264BitstreamParser::ParseFromNalUnit(wrong_type).present);

    const NalUnit empty{7, 0, {}, false, true};
    EXPECT_FALSE(H264BitstreamParser::ParseFromNalUnit(empty).present);

    // 只有 1 字节 payload 的 SPS 必然被截断
    const std::vector<uint8_t> truncated = {0x64};
    EXPECT_FALSE(H264BitstreamParser::ParseFromNalUnit(MakeSps(truncated)).present);

    // PPS 解析不能吃 SPS
    EXPECT_FALSE(H264BitstreamParser::ParsePpsFromNalUnit(MakeSps(kSpsHigh720p)).present);
}

TEST(H264BitstreamParserTest, NalUnitTypeIdentification) {
    EXPECT_TRUE(H264BitstreamParser::IsSpsNalUnit(MakeSps(kSpsHigh720p)));
    EXPECT_FALSE(H264BitstreamParser::IsPpsNalUnit(MakeSps(kSpsHigh720p)));
    EXPECT_TRUE(H264BitstreamParser::IsPpsNalUnit(MakePps(kPpsBasic)));
    EXPECT_FALSE(H264BitstreamParser::IsSpsNalUnit(MakePps(kPpsBasic)));

    const NalUnit idr{5, 0, {}, true, true};
    EXPECT_TRUE(idr.is_idr);
    EXPECT_TRUE(idr.is_keyframe);
    EXPECT_FALSE(H264BitstreamParser::IsSpsNalUnit(idr));
    EXPECT_FALSE(H264BitstreamParser::IsPpsNalUnit(idr));
}

} // namespace
