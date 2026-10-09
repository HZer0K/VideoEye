#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/analysis/codec/HevcBitstreamParser.h"
#include "core/media/codec/ExtradataParser.h"

namespace {

using videoeye::HevcBitstreamParser;
using videoeye::NalUnit;

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

TEST(HevcBitstreamParserTest, GarbageInputIsRejected) {
    // 足够长的 0xFF 垃圾流：ue(v) 全读成 0 → 0x0 尺寸。合理性校验必须拒绝
    // 置 present，否则下游拿到"0x0 的有效 SPS"参与容器比对。
    const std::vector<uint8_t> junk(64, 0xFF);
    EXPECT_FALSE(HevcBitstreamParser::ParseSpfFromNalUnit(MakeNal(33, junk)).present);
    // 超巨 ue(v)（前导 0 后跟长串）→ 截断成负数 / 超界值，同样拒绝
    const std::vector<uint8_t> huge = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // sps_id 起手 8 个 0
    };
    EXPECT_FALSE(HevcBitstreamParser::ParseSpfFromNalUnit(MakeNal(33, huge)).present);
}

// --------------------------------------------------------------------------
// hvcC 配置记录（ISO/IEC 14496-15:2017 真实布局）
//
// 历史教训：旧解析器按自造布局（level 在 byte13 / lengthSize 在 byte15 /
// byte16 当 numOfSPS），真实 MP4 hvcC 的 byte16 是 chromaFormat（0xFD），
// 被 &0x1F 误判成 1 个 SPS 后首个长度读到 0xFAF8 越界 —— 恒 0 个参数集。
// 这组用例按真实布局程序化构造，锁住正确布局不被"测试迎合实现"式改回去。
// --------------------------------------------------------------------------
std::vector<uint8_t> BuildHvcC(const std::vector<uint8_t>& vps,
                               const std::vector<uint8_t>& sps,
                               const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> out;
    out.reserve(23 + vps.size() + sps.size() + pps.size() + 6 * 5);
    out.push_back(0x01);                        // configurationVersion = 1
    out.push_back(0x01);                        // profile_space(0)|tier(0)|profile_idc=1 → Main 10
    out.insert(out.end(), 4, 0x60);            // profile_compatibility_flags（4B）
    out.insert(out.end(), 6, 0x00);            // constraint_indicator_flags（6B）
    out.push_back(153);                         // byte12: general_level_idc = 5.1
    out.push_back(0xF0);                        // byte13: reserved | min_spatial_seg 高 4 位
    out.push_back(0x00);                        // byte14: min_spatial_seg 低 8 位
    out.push_back(0xFC);                        // byte15: reserved | parallelismType=0
    out.push_back(0xFC | 1);                    // byte16: reserved | chromaFormat=1（4:2:0）
    out.push_back(0xF8 | 2);                    // byte17: reserved | bitDepthLumaMinus8=2（10bit）
    out.push_back(0xF8 | 2);                    // byte18: reserved | bitDepthChromaMinus8=2
    out.push_back(0x00);                        // byte19: avgFrameRate 高位
    out.push_back(0x00);                        // byte20: avgFrameRate 低位
    out.push_back(0x0F);                        // byte21: cfr=0|numTemporalLayers=1|nested=1|lengthSize=3
    out.push_back(0x03);                        // byte22: numOfArrays = 3（VPS / SPS / PPS）

    auto push_array = [&out](uint8_t nal_type, const std::vector<uint8_t>& payload) {
        out.push_back(static_cast<uint8_t>(0x80 | nal_type));  // completeness=1|reserved|type
        out.push_back(0x00);
        out.push_back(0x01);                                    // numNalus = 1
        const size_t length = payload.size() + 2;              // 2 字节 NAL header + payload
        out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        // HEVC NAL header：forbidden(0) type(6) → 首字节 type<<1；layer(6)+tid+1(3) → 0x01
        out.push_back(static_cast<uint8_t>(nal_type << 1));
        out.push_back(0x01);
        out.insert(out.end(), payload.begin(), payload.end());
    };

    push_array(32, vps);
    push_array(33, sps);
    push_array(34, pps);
    return out;
}

class HevcConfigRecordTest : public testing::Test {};

TEST_F(HevcConfigRecordTest, ParseRealLayoutHvcC) {
    const std::vector<uint8_t> hvcC = BuildHvcC(kVpsMain10, kSpsMain10_1080p, kPpsMain10);
    const videoeye::ExtradataResult result =
        videoeye::ExtradataParser::ParseWithFormat(videoeye::ExtradataFormat::HvcC,
                                                   hvcC.data(), hvcC.size());
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.format, videoeye::ExtradataFormat::HvcC);

    // 配置记录头：真实布局的字段位置（level=byte12、chroma=byte16、位深=byte17）
    EXPECT_EQ(result.config.general_profile_space, 0);
    EXPECT_EQ(result.config.general_tier_flag, 0);
    EXPECT_EQ(result.config.general_profile_idc, 1);
    EXPECT_EQ(result.config.general_level_idc, 153u);
    EXPECT_EQ(result.config.chroma_format_idc, 1);
    EXPECT_EQ(result.config.bit_depth_minus_8, 2);
    EXPECT_EQ(result.config.length_size_minus_one, 3u);

    // 三个数组 → VPS / SPS / PPS 各一条，type 从 NAL 首字节还原
    ASSERT_EQ(result.nal_units.size(), 3u);
    EXPECT_EQ(result.nal_units[0].type, 32);
    EXPECT_EQ(result.nal_units[1].type, 33);
    EXPECT_EQ(result.nal_units[2].type, 34);
    EXPECT_EQ(result.nal_units[1].data, kSpsMain10_1080p);
    EXPECT_EQ(result.nal_units[2].data, kPpsMain10);

    // 从 hvcC 抽出的 SPS 喂解析器应得到 1920x1080 —— 打通 hvcC → NAL → SPS 链路
    const auto sps = HevcBitstreamParser::ParseSpfFromNalUnit(result.nal_units[1]);
    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.width(), 1920);
    EXPECT_EQ(sps.height(), 1080);
    EXPECT_EQ(sps.BitDepthLuma(), 10);

    const auto vps = HevcBitstreamParser::ParseVpsFromNalUnit(result.nal_units[0]);
    ASSERT_TRUE(vps.present);
    EXPECT_EQ(vps.general_level_idc, 153u);
}

TEST_F(HevcConfigRecordTest, HvcCTooSmallIsRejected) {
    const std::vector<uint8_t> small(22, 0x01);   // 少于 23 字节：连 numOfArrays 都没有
    const videoeye::ExtradataResult result =
        videoeye::ExtradataParser::ParseWithFormat(videoeye::ExtradataFormat::HvcC,
                                                   small.data(), small.size());
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(HevcConfigRecordTest, HvcCWithGarbageArrayBoundsIsSafe) {
    // numOfArrays 声称 255 个数组但记录截断：不能越界、不能崩
    std::vector<uint8_t> hvcC = BuildHvcC(kVpsMain10, kSpsMain10_1080p, kPpsMain10);
    hvcC[22] = 0xFF;
    hvcC.resize(hvcC.size() - 5);   // 再截掉尾部制造"声明多、实际少"
    const videoeye::ExtradataResult result =
        videoeye::ExtradataParser::ParseWithFormat(videoeye::ExtradataFormat::HvcC,
                                                   hvcC.data(), hvcC.size());
    EXPECT_TRUE(result.valid);   // 越界项被安全跳过，解析本身不算失败
}

} // namespace
