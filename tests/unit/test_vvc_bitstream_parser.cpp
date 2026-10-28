#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <random>
#include <utility>
#include <vector>

#include "core/analysis/codec/VvcBitstreamParser.h"
#include "core/media/codec/ExtradataParser.h"

namespace {

using videoeye::ExtradataFormat;
using videoeye::ExtradataParser;
using videoeye::ExtradataResult;
using videoeye::NalSyntax;
using videoeye::NalUnit;
using videoeye::VvcBitstreamParser;
using videoeye::model::VvcPpsInfo;
using videoeye::model::VvcSpsInfo;
using videoeye::model::VvcVpsInfo;

// 测试数据由 _smoke/gen_vvc_ps.py 按 H.266 7.3.2.x 逐位生成。
// 语法顺序以 FFmpeg libavcodec/cbs_h266_syntax_template.c 为准。
// NalUnit::data 只含 RBSP payload（2 字节 NAL header 已由 ExtradataParser 剥离）。

// VPS: id=1 / Main 10 / Level 3.1(idc=51) / 单层
const std::vector<uint8_t> kVpsMain10 = {
    0x10, 0x00, 0x00, 0x03, 0x02, 0x33, 0x80, 0x00, 0x40,
};

// SPS A: 1920x1080 4:2:0 10bit Main10 L3.1 VUI(BT.2020 + PQ + 16:9)
const std::vector<uint8_t> kSpsMain10_1080p = {
    0x01, 0x0B, 0x02, 0x33, 0x80, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x43, 0x91, 0x88, 0x04, 0xBA, 0xD5, 0x7B,
    0xFF, 0xFC, 0x6F, 0x6B, 0xFB, 0xFF, 0xB9, 0x89, 0x80, 0x88, 0x05, 0x09, 0x10, 0x09, 0x00, 0x40,
};

// SPS B: 1920x1088，conformance window bottom=4 → 显示高度 1080；8bit；无 VUI
const std::vector<uint8_t> kSpsMain10_1080pCrop = {
    0x01, 0x0B, 0x02, 0x33, 0x80, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x44, 0x1F, 0x2A,
    0x20, 0x12, 0xEB, 0x55, 0xEF, 0xFF, 0xF1, 0xBD, 0xAF, 0xEF, 0xFE, 0xE6, 0x08,
};

// SPS C: 3840x2160 4:2:2 12bit，profile_idc=33 (Main 10 4:4:4)，Level 6.2(idc=102)
const std::vector<uint8_t> kSpsMain444_4K = {
    0x01, 0x13, 0x42, 0x66, 0x80, 0x00, 0x00, 0x07, 0x80, 0x80, 0x08, 0x71, 0x0A, 0x20, 0x12, 0xEB,
    0x55, 0xEF, 0xFF, 0xF1, 0xBD, 0xAF, 0xEF, 0xFB, 0x98, 0x94, 0x82, 0x12, 0x20, 0x12, 0x00, 0x40,
};

// PPS: id=0 / sps_id=0 / 1920x1080（与 SPS A 一致）
const std::vector<uint8_t> kPpsMain10 = {
    0x00, 0x00, 0x07, 0x81, 0x00, 0x21, 0xC8, 0x1F, 0x24, 0x12, 0x60, 0x20,
};

// vvcC 配置记录：Main 10 / L3.1 / 4:2:0 10bit / 1920x1080 / 无 NAL 数组
const std::vector<uint8_t> kVvcCRecord = {
    0xFF,       // reserved(5)=11111 | lengthSizeMinusOne(2)=3 | ptl_present(1)
    0x00, 0x01, // ols_idx(9)=0 | num_sublayers(3)=0 | cfr(2)=0 | chroma(2)=1
    0x40,       // bit_depth_minus8(3)=2 | reserved(5)
    0x01,       // reserved(2) | num_bytes_constraint_info(6)=1
    0x02,       // general_profile_idc(7)=1 | general_tier_flag(1)=0
    0x33,       // general_level_idc = 51 (3.1)
    0x80,       // frame_only(1) | multilayer(1) | constraint info(6)
    0x00,       // num_sub_profiles = 0
    0x07, 0x80, // max_picture_width  = 1920
    0x04, 0x38, // max_picture_height = 1080
    0x00, 0x00, // avg_frame_rate
    0x00,       // num_of_arrays = 0
};

NalUnit MakeNal(uint8_t type, const std::vector<uint8_t>& payload) {
    // VVC 的随机访问点：IDR_W_RADL(7) / IDR_N_LP(8) / CRA_NUT(9) / GDR_NUT(10)
    const bool keyframe = (type >= 7 && type <= 10);
    return NalUnit{type, payload.size(), payload, (type == 7 || type == 8), keyframe};
}

// 手工拼一个带 NAL header 的 VVC NAL（type 在第 2 字节高 5 位）
std::vector<uint8_t> WithVvcHeader(uint8_t type, uint8_t temporal_id_plus1, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.push_back(0x00); // forbidden=0 reserved=0 layer_id=0
    out.push_back(static_cast<uint8_t>((type << 3) | (temporal_id_plus1 & 0x07)));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// --------------------------------------------------------------------------
// Profile / Level 名称转换
// --------------------------------------------------------------------------
class VvcProfileTierLevelTest : public testing::Test {};

TEST_F(VvcProfileTierLevelTest, GetProfileName) {
    // H.266 的 general_profile_idc 是位组合：1=Main 10、+32=4:4:4、+64=Still Picture
    EXPECT_EQ(VvcBitstreamParser::GetProfileName(1), "Main 10");
    EXPECT_EQ(VvcBitstreamParser::GetProfileName(33), "Main 10 4:4:4");
    EXPECT_EQ(VvcBitstreamParser::GetProfileName(65), "Main 10 Still Picture");
    EXPECT_EQ(VvcBitstreamParser::GetProfileName(97), "Main 10 4:4:4 Still Picture");
    // 未列出的取值按位拼名，至少不返回空串
    EXPECT_FALSE(VvcBitstreamParser::GetProfileName(17).empty());
}

TEST_F(VvcProfileTierLevelTest, GetLevelVersion) {
    // H.266 A.4：general_level_idc = major * 16 + minor * 3
    EXPECT_EQ(VvcBitstreamParser::GetLevelVersion(51), "3.1");
    EXPECT_EQ(VvcBitstreamParser::GetLevelVersion(67), "4.1");
    EXPECT_EQ(VvcBitstreamParser::GetLevelVersion(102), "6.2");
    EXPECT_EQ(VvcBitstreamParser::GetLevelVersion(153), "9.3");
}

// --------------------------------------------------------------------------
// NAL 类型判断 + NAL header 位域
// --------------------------------------------------------------------------
class VvcNalUnitTypeTest : public testing::Test {};

TEST_F(VvcNalUnitTypeTest, IsVpsSpsPpsNalUnit) {
    const NalUnit vps = MakeNal(14, kVpsMain10);
    const NalUnit sps = MakeNal(15, kSpsMain10_1080p);
    const NalUnit pps = MakeNal(16, kPpsMain10);

    EXPECT_TRUE(VvcBitstreamParser::IsVpsNalUnit(vps));
    EXPECT_FALSE(VvcBitstreamParser::IsSpsNalUnit(vps));
    EXPECT_FALSE(VvcBitstreamParser::IsPpsNalUnit(vps));

    EXPECT_TRUE(VvcBitstreamParser::IsSpsNalUnit(sps));
    EXPECT_FALSE(VvcBitstreamParser::IsVpsNalUnit(sps));

    EXPECT_TRUE(VvcBitstreamParser::IsPpsNalUnit(pps));
    EXPECT_FALSE(VvcBitstreamParser::IsSpsNalUnit(pps));

    // 图像 NAL 不应被误判成参数集
    EXPECT_FALSE(VvcBitstreamParser::IsSpsNalUnit(MakeNal(7, {})));
}

// ⚠️ 这条是整个 VVC 支持的地基：VVC 的 nal_unit_type 在**第 2 字节**的高 5 位，
//    而 HEVC 在第 1 字节。以前两者共用一套启发式时，VVC 的 VPS/SPS/PPS
//    会被整体认错。改 ExtradataParser 时这条必须保持通过。
TEST_F(VvcNalUnitTypeTest, NalHeaderTypeIsInSecondByte) {
    const std::vector<uint8_t> nal = WithVvcHeader(15, 1, {0xAA, 0xBB, 0xCC});
    const NalUnit parsed = ExtradataParser::ParseVvcNalUnit(nal.data(), nal.size());

    EXPECT_EQ(parsed.type, 15);
    // header（2 字节）必须被剥离，data 只留 RBSP payload
    ASSERT_EQ(parsed.data.size(), 3u);
    EXPECT_EQ(parsed.data[0], 0xAA);
    EXPECT_EQ(parsed.data[2], 0xCC);
    EXPECT_FALSE(parsed.is_keyframe);

    // 首字节的 HEVC 公式在这段字节上给出 0 —— 证明两条公式不可混用
    const uint8_t hevc_type = static_cast<uint8_t>((nal[0] >> 1) & 0x3F);
    EXPECT_EQ(hevc_type, 0);
    EXPECT_NE(hevc_type, parsed.type);
}

TEST_F(VvcNalUnitTypeTest, NalHeaderKeyframeTypes) {
    for (uint8_t type : {7, 8, 9, 10}) {
        const std::vector<uint8_t> nal = WithVvcHeader(type, 1, {0x01});
        const NalUnit parsed = ExtradataParser::ParseVvcNalUnit(nal.data(), nal.size());
        EXPECT_EQ(parsed.type, type);
        EXPECT_TRUE(parsed.is_keyframe);
        EXPECT_EQ(parsed.is_idr, (type == 7 || type == 8));
    }
}

TEST_F(VvcNalUnitTypeTest, NalHeaderTooShort) {
    const std::vector<uint8_t> one_byte = {0x00};
    const NalUnit parsed = ExtradataParser::ParseVvcNalUnit(one_byte.data(), one_byte.size());
    EXPECT_EQ(parsed.type, 0);
    EXPECT_TRUE(parsed.data.empty());
}

// --------------------------------------------------------------------------
// VPS 解析
// --------------------------------------------------------------------------
class VvcVpsTest : public testing::Test {};

TEST_F(VvcVpsTest, SingleLayerMain10) {
    const VvcVpsInfo vps = VvcBitstreamParser::ParseVpsFromNalUnit(MakeNal(14, kVpsMain10));
    ASSERT_TRUE(vps.present);
    EXPECT_EQ(vps.vps_video_parameter_set_id, 1);
    EXPECT_EQ(vps.vps_max_layers_minus1, 0);
    EXPECT_EQ(vps.vps_max_sublayers_minus1, 0);
    EXPECT_EQ(vps.MaxLayers(), 1);
    EXPECT_EQ(vps.MaxSubLayers(), 1);
    EXPECT_EQ(vps.general_profile_idc, 1);
    EXPECT_EQ(vps.general_tier_flag, 0);
    EXPECT_EQ(vps.general_level_idc, 51u);
    EXPECT_EQ(vps.ProfileName(), "Main 10");
    EXPECT_EQ(vps.LevelString(), "3.1");
    EXPECT_TRUE(vps.vps_each_layer_is_an_ols_flag);
}

TEST_F(VvcVpsTest, TruncatedVpsPrefixesAreRejected) {
    // VPS 此前只有成功路径的测试。逐字节截断：任何「还缺 PTL 语义」的前缀都会让
    // BitReader 越界 -> HasError -> 不置 present。
    // 例外与 SPS 同理：kVpsMain10 的最后一个字节是 vps_extension_flag + rbsp
    // trailing bits，无语义 —— 截到 n-1 字节时全部语义字段（含 PTL）已完整，
    // present=true 是正确行为。因此循环上界是 size()-2，再用两条显式断言钉住边界。
    for (size_t len = 1; len + 1 < kVpsMain10.size(); ++len) {
        const std::vector<uint8_t> prefix(kVpsMain10.begin(), kVpsMain10.begin() + len);
        const VvcVpsInfo vps = VvcBitstreamParser::ParseVpsFromNalUnit(MakeNal(14, prefix));
        EXPECT_FALSE(vps.present) << "VPS 截断到 " << len << " 字节仍置了 present";
    }
    {
        // n-2 字节：还缺 PTL 尾部字段，必须失败
        const std::vector<uint8_t> prefix(kVpsMain10.begin(), kVpsMain10.begin() + kVpsMain10.size() - 2);
        EXPECT_FALSE(VvcBitstreamParser::ParseVpsFromNalUnit(MakeNal(14, prefix)).present);
        // n-1 字节：只缺 vps_extension_flag，语义字段完整，允许 present
        const std::vector<uint8_t> full_fields(kVpsMain10.begin(), kVpsMain10.end() - 1);
        EXPECT_TRUE(VvcBitstreamParser::ParseVpsFromNalUnit(MakeNal(14, full_fields)).present);
    }
}

// --------------------------------------------------------------------------
// SPS 解析
// --------------------------------------------------------------------------
class VvcSpsTest : public testing::Test {};

TEST_F(VvcSpsTest, Main10_1080p) {
    const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, kSpsMain10_1080p));
    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.sps_seq_parameter_set_id, 0);
    EXPECT_EQ(sps.sps_video_parameter_set_id, 1);
    EXPECT_EQ(sps.sps_max_sublayers_minus1, 0);
    EXPECT_EQ(sps.sps_chroma_format_idc, 1);
    EXPECT_EQ(sps.sps_log2_ctu_size_minus5, 1);
    EXPECT_EQ(sps.CtuSize(), 64);
    EXPECT_EQ(sps.MinCbSizeY(), 4);
    EXPECT_EQ(sps.SubWidthC(), 2);
    EXPECT_EQ(sps.SubHeightC(), 2);

    // 尺寸：无裁剪 → 显示尺寸 == 编码尺寸
    EXPECT_EQ(sps.sps_pic_width_max_in_luma_samples, 1920);
    EXPECT_EQ(sps.sps_pic_height_max_in_luma_samples, 1080);
    EXPECT_FALSE(sps.sps_conformance_window_flag);
    EXPECT_EQ(sps.width(), 1920);
    EXPECT_EQ(sps.height(), 1080);

    // PTL 在 SPS 里（sps_ptl_dpb_hrd_params_present_flag = 1）
    EXPECT_TRUE(sps.sps_ptl_dpb_hrd_params_present_flag);
    EXPECT_EQ(sps.general_profile_idc, 1);
    EXPECT_EQ(sps.general_level_idc, 51u);
    EXPECT_EQ(sps.ProfileName(), "Main 10");
    EXPECT_EQ(sps.LevelString(), "3.1");

    EXPECT_EQ(sps.sps_bitdepth_minus8, 2);
    EXPECT_EQ(sps.BitDepthLuma(), 10);
    EXPECT_EQ(sps.BitDepthChroma(), 10);
    EXPECT_EQ(sps.MaxPicOrderCntLsb(), 1 << 8);

    // 工具开关（生成器里都置 1）
    EXPECT_TRUE(sps.sps_sao_enabled_flag);
    EXPECT_TRUE(sps.sps_alf_enabled_flag);
    EXPECT_TRUE(sps.sps_ccalf_enabled_flag);
    EXPECT_TRUE(sps.sps_lmcs_enabled_flag);
    EXPECT_TRUE(sps.sps_mts_enabled_flag);
    EXPECT_TRUE(sps.sps_lfnst_enabled_flag);
    EXPECT_TRUE(sps.sps_affine_enabled_flag);
    EXPECT_TRUE(sps.sps_ibc_enabled_flag);
    EXPECT_TRUE(sps.sps_dep_quant_enabled_flag);
    EXPECT_FALSE(sps.sps_palette_enabled_flag);
    EXPECT_FALSE(sps.sps_long_term_ref_pics_flag);
    EXPECT_EQ(sps.sps_num_ref_pic_lists[0], 0);
    EXPECT_EQ(sps.sps_num_ref_pic_lists[1], 0);

    // VUI：BT.2020 NCL + PQ + BT.2020 NCL，limited range
    ASSERT_TRUE(sps.vui.present);
    ASSERT_TRUE(sps.vui.colour_description_present_flag);
    EXPECT_EQ(sps.vui.colour_primaries, 9);
    EXPECT_EQ(sps.vui.transfer_characteristics, 16);
    EXPECT_EQ(sps.vui.matrix_coeffs, 9);
    EXPECT_FALSE(sps.vui.full_range_flag);
    EXPECT_TRUE(sps.vui.aspect_ratio_info_present_flag);
    EXPECT_EQ(sps.vui.aspect_ratio_idc, 1);
}

TEST_F(VvcSpsTest, ConformanceWindowCropsDisplaySize) {
    const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, kSpsMain10_1080pCrop));
    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.sps_pic_width_max_in_luma_samples, 1920);
    EXPECT_EQ(sps.sps_pic_height_max_in_luma_samples, 1088);
    ASSERT_TRUE(sps.sps_conformance_window_flag);
    EXPECT_EQ(sps.sps_conf_win_bottom_offset, 4);

    // SubHeightC(chroma 4:2:0) = 2 → 1088 - 4*2 = 1080
    EXPECT_EQ(sps.width(), 1920);
    EXPECT_EQ(sps.height(), 1080);

    EXPECT_EQ(sps.sps_bitdepth_minus8, 0);
    EXPECT_EQ(sps.BitDepthLuma(), 8);
    EXPECT_FALSE(sps.sps_vui_parameters_present_flag);
    EXPECT_FALSE(sps.vui.present);
}

TEST_F(VvcSpsTest, Chroma422_12bit_4K) {
    const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, kSpsMain444_4K));
    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.sps_chroma_format_idc, 2);
    // 4:2:2 → SubWidthC = 2、SubHeightC = 1（不是 4:2:0 的 2/2）
    EXPECT_EQ(sps.SubWidthC(), 2);
    EXPECT_EQ(sps.SubHeightC(), 1);
    EXPECT_EQ(sps.width(), 3840);
    EXPECT_EQ(sps.height(), 2160);
    EXPECT_EQ(sps.BitDepthLuma(), 12);

    EXPECT_EQ(sps.general_profile_idc, 33);
    EXPECT_EQ(sps.ProfileName(), "Main 10 4:4:4");
    EXPECT_EQ(sps.general_level_idc, 102u);
    EXPECT_EQ(sps.LevelString(), "6.2");
}

TEST_F(VvcSpsTest, GarbageInputDoesNotCrash) {
    const std::vector<uint8_t> junk = {0xFF, 0xFF, 0xFF};
    const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, junk));
    // 全 0xFF 垃圾码流：ue(v) 全读成 0 → 0x0 分辨率，合理性校验必须拦下，
    // 不能把垃圾当"解析成功"交给下游（旧实现会 present=true + 0x0 尺寸）。
    EXPECT_FALSE(sps.present);
    const VvcSpsInfo empty = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, {}));
    EXPECT_FALSE(empty.present);
}

TEST_F(VvcSpsTest, LongGarbageStreamIsRejected) {
    // 足够长的 0xFF 流：位足够把语法"走完"，但宽高 ue(v)=0 —— 校验仍要拒绝
    // 置 present，否则下游会拿到 0x0 分辨率 + 满屏垃圾工具开关。
    const std::vector<uint8_t> junk(64, 0xFF);
    const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, junk));
    EXPECT_FALSE(sps.present);

    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, junk));
    EXPECT_FALSE(pps.present);
}

TEST_F(VvcSpsTest, TruncatedSpsPrefixesAreRejected) {
    // 截断测试：对每个严格前缀，解析必须失败（present=false）。
    // SPS 的 VUI 段落在尾部且解析会一直读到接近末尾，任何截断前缀都会让
    // BitReader 越界 → HasError → 不置 present。半截参数集绝不许进入结果。
    // 例外：SPS A 的最后一个字节（0x40）是 rbsp_trailing_bits，无语义 ——
    // 截到 n-1 字节时全部语义字段已完整，present=true 是正确行为，不算假完成。
    // 因此循环上界是 size()-1，再用两条显式断言钉住这个边界。
    for (size_t len = 1; len + 1 < kSpsMain10_1080p.size(); ++len) {
        const std::vector<uint8_t> prefix(kSpsMain10_1080p.begin(), kSpsMain10_1080p.begin() + len);
        const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, prefix));
        EXPECT_FALSE(sps.present) << "SPS A 截断到 " << len << " 字节仍置了 present";
    }
    {
        // n-2 字节：还缺 VUI 尾部字段，必须失败
        const std::vector<uint8_t> prefix(kSpsMain10_1080p.begin(),
                                          kSpsMain10_1080p.begin() + kSpsMain10_1080p.size() - 2);
        EXPECT_FALSE(VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, prefix)).present);
        // n-1 字节：只缺 trailing bits，语义字段完整，允许 present
        const std::vector<uint8_t> full_fields(kSpsMain10_1080p.begin(), kSpsMain10_1080p.end() - 1);
        EXPECT_TRUE(VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, full_fields)).present);
    }
    // 带 conformance window / 无 VUI 的变体同样不许漏
    for (size_t len = 1; len < kSpsMain10_1080pCrop.size(); ++len) {
        const std::vector<uint8_t> prefix(kSpsMain10_1080pCrop.begin(), kSpsMain10_1080pCrop.begin() + len);
        const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, prefix));
        EXPECT_FALSE(sps.present) << "SPS B 截断到 " << len << " 字节仍置了 present";
    }
}

// --------------------------------------------------------------------------
// PPS 解析
// --------------------------------------------------------------------------
class VvcPpsTest : public testing::Test {};

TEST_F(VvcPpsTest, Main10_1080p) {
    VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, kPpsMain10));
    ASSERT_TRUE(pps.present);
    EXPECT_EQ(pps.pps_pic_parameter_set_id, 0);
    EXPECT_EQ(pps.pps_seq_parameter_set_id, 0);

    // PPS 的分辨率是**无条件**的 ue（不是"可选覆盖"），必须能读出来
    EXPECT_EQ(pps.pps_pic_width_in_luma_samples, 1920);
    EXPECT_EQ(pps.pps_pic_height_in_luma_samples, 1080);

    pps.chroma_format_idc = 1; // 实际由 BitstreamAnalyzer 从 SPS 拷入
    EXPECT_EQ(pps.SubWidthC(), 2);
    EXPECT_EQ(pps.SubHeightC(), 2);
    EXPECT_EQ(pps.width(), 1920);
    EXPECT_EQ(pps.height(), 1080);

    EXPECT_FALSE(pps.pps_mixed_nalu_types_in_pic_flag);
    EXPECT_FALSE(pps.pps_no_pic_partition_flag);
    EXPECT_FALSE(pps.pps_output_flag_present_flag);
}

TEST_F(VvcPpsTest, TruncatedPpsPrefixesAreRejected) {
    // PPS 解析是浅解析（读满 58 bit / 约 7.25 字节即置 present，尾部 tile
    // 划分不读）。截到不足这些位的前缀必须失败；8 字节起的前缀包含全部
    // 已读字段，允许 present —— 此时字段本身是完整的，不算"假完成"。
    for (size_t len = 1; len <= 7; ++len) {
        const std::vector<uint8_t> prefix(kPpsMain10.begin(), kPpsMain10.begin() + len);
        const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, prefix));
        EXPECT_FALSE(pps.present) << "PPS 截断到 " << len << " 字节仍置了 present";
    }
}

namespace {

// 极简 MSB-first 位写入器：只为拼出"头部合法、subpic 计数越界"的 PPS。
class BitWriter {
public:
    void Bit(uint32_t v) {
        cur_ = static_cast<uint8_t>((cur_ << 1) | (v & 1u));
        if (++n_ == 8) {
            out_.push_back(cur_);
            cur_ = 0;
            n_ = 0;
        }
    }
    void Bits(uint32_t v, int count) {
        for (int i = count - 1; i >= 0; --i)
            Bit((v >> i) & 1u);
    }
    void Ue(uint32_t value) {
        const uint32_t code = value + 1;
        int len = 0;
        for (uint32_t t = code; t > 1; t >>= 1)
            ++len;
        for (int i = 0; i < len; ++i)
            Bit(0);
        Bits(code, len + 1);
    }
    void Se(int v) {
        Ue(v <= 0 ? static_cast<uint32_t>(-2 * v) : static_cast<uint32_t>(2 * v - 1));
    }
    std::vector<uint8_t> Bytes() {
        std::vector<uint8_t> out = out_;
        if (n_ > 0)
            out.push_back(static_cast<uint8_t>(cur_ << (8 - n_)));
        return out;
    }

private:
    std::vector<uint8_t> out_;
    uint8_t cur_ = 0;
    int n_ = 0;
};

// PPS RBSP：前 10 个字段都合法（1920x1080），subpic_id_mapping 打开，
// pps_num_subpics_minus1 取传入值。构造出的字节里不含 00 00 03，无需 emulation prevention。
std::vector<uint8_t> MakePpsWithNumSubpics(uint32_t num_subpics_minus1) {
    BitWriter w;
    w.Bits(0, 6); // pps_pic_parameter_set_id
    w.Bits(0, 4); // pps_seq_parameter_set_id
    w.Bit(0);     // pps_mixed_nalu_types_in_pic_flag
    w.Ue(1920);   // pps_pic_width_in_luma_samples
    w.Ue(1080);   // pps_pic_height_in_luma_samples
    w.Bit(0);     // pps_conformance_window_flag
    w.Bit(0);     // pps_scaling_window_explicit_signalling_flag
    w.Bit(0);     // pps_output_flag_present_flag
    w.Bit(0);     // pps_no_pic_partition_flag
    w.Bit(1);     // pps_subpic_id_mapping_present_flag
    w.Ue(num_subpics_minus1);
    w.Ue(0); // subpic_id_len_minus1
    return w.Bytes();
}

} // namespace

TEST_F(VvcPpsTest, SubpicIdMappingOverLimitIsRejected) {
    // pps_num_subpics_minus1 = 1025 越过 kMaxNumSubpics(1024)：语法合法但数值超限，
    // 必须走 ReadUEBounded 的报错分支 -> HasError -> present=false。
    // 旧实现用裸 ReadUE 且循环无上界，这个计数能撑起约 20 亿次 SkipBits。
    const std::vector<uint8_t> over = MakePpsWithNumSubpics(1025);
    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, over));
    EXPECT_FALSE(pps.present) << "越界 subpic 计数必须拒绝置 present";
}

// --------------------------------------------------------------------------
// PPS 后半段（QP / 权重 / 去块）：按 H.266 7.3.2.4 Table 15 逐位构造
//
// ⚠️ 这段以前没有测试，于是"字段顺序读反"这种错误一路活到了面板上：
//    按正确语法构造的 QP=30 输入会被判成 qp_fields_valid=false（字段全被回滚）。
// 生成器独立于实现：只照规范表写，不抄解析器的 if 分支。
// --------------------------------------------------------------------------
namespace {

struct PpsQpOptions {
    bool rpl1_idx_present = false;
    int second_default_active_minus1 = 0; // 仅在 rpl1_idx_present 时有意义
    bool ref_wraparound = false;
    int wraparound_offset = 0;
    int pic_init_qp_minus26 = 4; // 26 + 4 = 30
    // true = 走 tile 划分段（no_pic_partition_flag = 0）：2 列 × 1 行 tile。
    // 这一段是"顺序差一位、QP 全错"的重灾区，单独给它一个用例。
    bool tile_partition = false;
};

// no_pic_partition_flag = 1（跳过 tile 段）→ 直接进 QP 段。
std::vector<uint8_t> MakePpsWithQp(const PpsQpOptions& opt) {
    BitWriter w;
    w.Bits(0, 6);                      // pps_pic_parameter_set_id
    w.Bits(0, 4);                      // pps_seq_parameter_set_id
    w.Bit(0);                          // pps_mixed_nalu_types_in_pic_flag
    w.Ue(1920);                        // pps_pic_width_in_luma_samples
    w.Ue(1080);                        // pps_pic_height_in_luma_samples
    w.Bit(0);                          // pps_conformance_window_flag
    w.Bit(0);                          // pps_scaling_window_explicit_signalling_flag
    w.Bit(0);                          // pps_output_flag_present_flag
    w.Bit(opt.tile_partition ? 0 : 1); // pps_no_pic_partition_flag
    w.Bit(0);                          // pps_subpic_id_mapping_present_flag
    if (opt.tile_partition) {
        // ---- tile 段（H.266 7.3.2.4）----
        w.Bits(1, 2); // pps_log2_ctu_size_minus5（规范要求与 SPS 相同）
        w.Ue(1);      // pps_num_exp_tile_columns_minus1 = 1 → 2 列
        w.Ue(0);      // pps_num_exp_tile_rows_minus1    = 0 → 1 行
        // ❗ 宽/高数组长度是 minus1 + 1：两列都要显式给宽度
        w.Ue(9);  // pps_tile_column_width_minus1[0]
        w.Ue(9);  // pps_tile_column_width_minus1[1]
        w.Ue(16); // pps_tile_row_height_minus1[0]
        w.Bit(0); // pps_loop_filter_across_tiles_enabled_flag
        w.Bit(1); // pps_rect_slice_flag
        w.Bit(1); // pps_single_slice_per_subpic_flag
        // ❗ NumTilesInPic > 1 且 rect_slice/single_slice 都为 1 时这一位照样存在
        w.Bit(0); // pps_loop_filter_across_slices_enabled_flag
    }
    // ---- 后半段：顺序即规范顺序 ----
    w.Bit(0); // pps_cabac_init_present_flag
    w.Ue(0);  // pps_num_ref_idx_default_active_minus1[0]  ← 无条件
    w.Ue(static_cast<uint32_t>(opt.second_default_active_minus1)); // [1]  ← 无条件
    w.Bit(opt.rpl1_idx_present ? 1 : 0);                           // pps_rpl1_idx_present_flag（在两条之后）
    w.Bit(0);                                                      // pps_weighted_pred_flag
    w.Bit(0);                                                      // pps_weighted_bipred_flag
    w.Bit(opt.ref_wraparound ? 1 : 0);                             // pps_ref_wraparound_enabled_flag
    if (opt.ref_wraparound)
        w.Ue(static_cast<uint32_t>(opt.wraparound_offset)); // pps_pic_width_minus_wraparound_offset
    w.Se(opt.pic_init_qp_minus26);                          // pps_pic_init_qp_minus26
    w.Bit(0);                                               // pps_cu_qp_delta_enabled_flag
    w.Bit(0);                                               // pps_chroma_tool_offsets_present_flag
    w.Bit(0);                                               // pps_deblocking_filter_control_present_flag
    w.Bit(0);                                               // pps_picture_header_extension_present_flag
    w.Bit(0);                                               // pps_slice_header_extension_present_flag
    w.Bit(0);                                               // pps_extension_flag
    w.Bit(1);                                               // rbsp trailing bits：stop bit
    return w.Bytes();
}

} // namespace

TEST_F(VvcPpsTest, QpSectionFollowsSpecOrder) {
    const std::vector<uint8_t> rbsp = MakePpsWithQp(PpsQpOptions{});
    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, rbsp));
    ASSERT_TRUE(pps.present);
    EXPECT_TRUE(pps.qp_fields_valid) << "规范顺序的 PPS 必须能解出 QP 段";
    EXPECT_EQ(pps.parse_depth, VvcPpsInfo::ParseDepth::Full);
    EXPECT_EQ(pps.pps_pic_init_qp_minus26, 4);
    EXPECT_EQ(pps.pps_pic_init_qp_minus26 + 26, 30);
    // 两条默认 active 都是无条件的：rpl1_idx_present_flag = 0 时第二条照样存在
    EXPECT_FALSE(pps.pps_rpl1_idx_present_flag);
    EXPECT_EQ(pps.pps_num_ref_idx_default_active_minus1[0], 0);
    EXPECT_EQ(pps.pps_num_ref_idx_default_active_minus1[1], 0);
}

TEST_F(VvcPpsTest, TwoDefaultActiveRefIdxAreAlwaysPresent) {
    // pps_rpl1_idx_present_flag = 1 时第二条取 2：若实现"用 flag 决定读不读"，
    // 这个 2 会被读成后面的 flag 位，QP 段整体错位。
    PpsQpOptions opt;
    opt.rpl1_idx_present = true;
    opt.second_default_active_minus1 = 2;
    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, MakePpsWithQp(opt)));
    ASSERT_TRUE(pps.present);
    ASSERT_TRUE(pps.qp_fields_valid);
    EXPECT_TRUE(pps.pps_rpl1_idx_present_flag);
    EXPECT_EQ(pps.pps_num_ref_idx_default_active_minus1[1], 2);
    EXPECT_EQ(pps.pps_pic_init_qp_minus26, 4);
}

TEST_F(VvcPpsTest, TilePartitionSectionKeepsQpAligned) {
    // tile 段最容易"差一位"的两处：
    //   1. pps_tile_column_width_minus1[] 的长度是 minus1 **+1**（少读一个 → 后面全错）
    //   2. pps_loop_filter_across_slices_enabled_flag 的读取条件与 num_tiles 无关
    //      （num_tiles > 1、rect_slice=1、single_slice=1 时**照样**要读一位）
    PpsQpOptions opt;
    opt.tile_partition = true;
    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, MakePpsWithQp(opt)));
    ASSERT_TRUE(pps.present);
    ASSERT_TRUE(pps.qp_fields_valid) << "tile 段走位错一位，QP 段就整体读废";
    EXPECT_FALSE(pps.pps_no_pic_partition_flag);
    EXPECT_EQ(pps.pps_num_exp_tile_columns_minus1, 1);
    EXPECT_EQ(pps.pps_num_exp_tile_rows_minus1, 0);
    EXPECT_TRUE(pps.pps_rect_slice_flag);
    EXPECT_TRUE(pps.pps_single_slice_per_subpic_flag);
    EXPECT_EQ(pps.pps_pic_init_qp_minus26, 4);
}

TEST_F(VvcPpsTest, WraparoundOffsetIsConsumedBeforeQp) { // pps_ref_wraparound_enabled_flag = 1 时多一个 ue(v)。漏读它，
    // pps_pic_init_qp_minus26 就会按错位读 —— 这里直接钉住 offset 与 QP。
    PpsQpOptions opt;
    opt.ref_wraparound = true;
    opt.wraparound_offset = 64;
    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, MakePpsWithQp(opt)));
    ASSERT_TRUE(pps.present);
    ASSERT_TRUE(pps.qp_fields_valid);
    EXPECT_TRUE(pps.pps_ref_wraparound_enabled_flag);
    EXPECT_EQ(pps.pps_pic_width_minus_wraparound_offset, 64);
    EXPECT_EQ(pps.pps_pic_init_qp_minus26, 4);
}

// --------------------------------------------------------------------------
// vvcC 配置记录
// --------------------------------------------------------------------------
class VvcConfigRecordTest : public testing::Test {};

TEST_F(VvcConfigRecordTest, ParseVvcC) {
    const ExtradataResult result =
        ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC, kVvcCRecord.data(), kVvcCRecord.size());
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.format, ExtradataFormat::VvcC);

    EXPECT_EQ(result.config.general_profile_idc, 1);
    EXPECT_EQ(result.config.general_tier_flag, 0);
    EXPECT_EQ(result.config.general_level_idc, 51u);
    EXPECT_EQ(result.config.chroma_format_idc, 1);
    EXPECT_EQ(result.config.bit_depth_minus_8, 2);
    EXPECT_EQ(result.config.max_picture_width, 1920);
    EXPECT_EQ(result.config.max_picture_height, 1080);
    EXPECT_EQ(result.width, 1920);
    EXPECT_EQ(result.height, 1080);
    EXPECT_TRUE(result.nal_units.empty()); // num_of_arrays = 0
}

// --------------------------------------------------------------------------
// AnnexB 抽取：按 NalSyntax::Vvc 解读 header
// --------------------------------------------------------------------------
class VvcAnnexBTest : public testing::Test {};

TEST_F(VvcAnnexBTest, ExtractVvcParameterSets) {
    // 3 个 VVC NAL：VPS(14) / SPS(15) / PPS(16)，AnnexB 起始码分隔
    std::vector<uint8_t> stream;
    const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

    auto append = [&stream, &kStartCode](uint8_t type, const std::vector<uint8_t>& payload) {
        stream.insert(stream.end(), kStartCode, kStartCode + 4);
        stream.push_back(0x00);
        stream.push_back(static_cast<uint8_t>((type << 3) | 0x01));
        stream.insert(stream.end(), payload.begin(), payload.end());
    };

    append(14, kVpsMain10);
    append(15, kSpsMain10_1080p);
    append(16, kPpsMain10);

    const std::vector<NalUnit> nals =
        ExtradataParser::ExtractAnnBNalUnits(stream.data(), stream.size(), NalSyntax::Vvc);
    ASSERT_EQ(nals.size(), 3u);
    EXPECT_EQ(nals[0].type, 14);
    EXPECT_EQ(nals[1].type, 15);
    EXPECT_EQ(nals[2].type, 16);

    // 抽出来的 payload 直接喂给 parser 应能解析出同样的结果
    const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(nals[1]);
    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.width(), 1920);
    EXPECT_EQ(sps.height(), 1080);

    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(nals[2]);
    ASSERT_TRUE(pps.present);
    EXPECT_EQ(pps.pps_pic_width_in_luma_samples, 1920);
}

// --------------------------------------------------------------------------
// 真实编码器输出回归
//
// 上面的合成数据只能证明「生成器和解析器互相一致」——两边都理解错了也照样通过。
// 这份字节来自 libvvenc 真实编码的 640x360 / 10bit / 4:2:0 MP4，用来兜住
// 规范理解上的偏差。取自 vvcC box：
//   ⚠️ vvcC 是 FullBox，box payload 前 4 字节是 version+flags，
//      demuxer 交给 extradata 时已剥掉，这里存的就是剥完之后的样子。
// --------------------------------------------------------------------------
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

class VvcRealEncoderTest : public testing::Test {};

TEST_F(VvcRealEncoderTest, ParseRealVvcC) {
    const ExtradataResult result =
        ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC, kVvcCRealLibvvenc.data(), kVvcCRealLibvvenc.size());
    ASSERT_TRUE(result.valid);

    // vvcC 自带的 PTL：Main 10 / Level 2.1(idc=35) / 4:2:0 / 10bit / 640x360
    EXPECT_EQ(result.config.general_profile_idc, 1);
    EXPECT_EQ(result.config.general_level_idc, 35u);
    EXPECT_EQ(result.config.chroma_format_idc, 1);
    EXPECT_EQ(result.config.bit_depth_minus_8, 2);
    EXPECT_EQ(result.config.max_picture_width, 640);
    EXPECT_EQ(result.config.max_picture_height, 360);

    // vvcC 里带了 SPS + PPS 两组（真实编码器都会带）
    ASSERT_EQ(result.nal_units.size(), 2u);
    EXPECT_EQ(result.nal_units[0].type, 15); // SPS
    EXPECT_EQ(result.nal_units[1].type, 16); // PPS
}

TEST_F(VvcRealEncoderTest, ParseRealSpsAndPps) {
    const ExtradataResult result =
        ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC, kVvcCRealLibvvenc.data(), kVvcCRealLibvvenc.size());
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.nal_units.size(), 2u);

    const VvcSpsInfo sps = VvcBitstreamParser::ParseSpsFromNalUnit(result.nal_units[0]);
    ASSERT_TRUE(sps.present);
    EXPECT_EQ(sps.sps_chroma_format_idc, 1);
    EXPECT_EQ(sps.CtuSize(), 64);
    EXPECT_EQ(sps.width(), 640);
    EXPECT_EQ(sps.height(), 360);
    EXPECT_EQ(sps.BitDepthLuma(), 10);
    EXPECT_EQ(sps.general_profile_idc, 1);
    EXPECT_EQ(sps.general_level_idc, 35u);
    EXPECT_EQ(sps.ProfileName(), "Main 10");
    EXPECT_EQ(sps.LevelString(), "2.1");

    const VvcPpsInfo pps = VvcBitstreamParser::ParsePpsFromNalUnit(result.nal_units[1]);
    ASSERT_TRUE(pps.present);
    EXPECT_EQ(pps.pps_pic_width_in_luma_samples, 640);
    EXPECT_EQ(pps.pps_pic_height_in_luma_samples, 360);
}

// --------------------------------------------------------------------------
// 随机位翻转：解析器必须"要么拒绝、要么自洽"，绝不卡死
// --------------------------------------------------------------------------
class VvcMalformedTest : public testing::Test {};

TEST_F(VvcMalformedTest, RandomBitFlipsNeverHangOrClaimPresent) {
    // 固定种子 —— 失败可复现。每轮只翻 1~3 个 bit：既覆盖"语法还能走通但字段错位"
    // 的情形，又不会退化成纯噪声。放进 async 限时，回归成死循环会变成 2s 失败，
    // 而不是把 CI 挂住。
    std::mt19937 rng(0xC0FFEEu);
    constexpr int kIterations = 128;
    constexpr int kMaxPictureSize = 65536;

    // SPS：present 时尺寸必须落在 (0, 65536]
    for (int iter = 0; iter < kIterations; ++iter) {
        std::vector<uint8_t> mutated = kSpsMain10_1080p;
        const int flips = 1 + static_cast<int>(rng() % 3);
        for (int f = 0; f < flips; ++f) {
            const size_t byte = rng() % mutated.size();
            mutated[byte] ^= static_cast<uint8_t>(1u << (rng() % 8));
        }
        auto task = std::async(std::launch::async,
                               [&mutated]() { return VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, mutated)); });
        ASSERT_EQ(task.wait_for(std::chrono::seconds(2)), std::future_status::ready) << "位翻转后的 SPS 让解析器卡住了";
        const VvcSpsInfo sps = task.get();
        if (sps.present) {
            EXPECT_GT(sps.width(), 0);
            EXPECT_GT(sps.height(), 0);
            EXPECT_LE(sps.width(), kMaxPictureSize);
            EXPECT_LE(sps.height(), kMaxPictureSize);
        }
    }

    // PPS：present 时尺寸同样必须落在 (0, 65536]
    for (int iter = 0; iter < kIterations; ++iter) {
        std::vector<uint8_t> mutated = kPpsMain10;
        const int flips = 1 + static_cast<int>(rng() % 3);
        for (int f = 0; f < flips; ++f) {
            const size_t byte = rng() % mutated.size();
            mutated[byte] ^= static_cast<uint8_t>(1u << (rng() % 8));
        }
        auto task = std::async(std::launch::async,
                               [&mutated]() { return VvcBitstreamParser::ParsePpsFromNalUnit(MakeNal(16, mutated)); });
        ASSERT_EQ(task.wait_for(std::chrono::seconds(2)), std::future_status::ready) << "位翻转后的 PPS 让解析器卡住了";
        const VvcPpsInfo pps = task.get();
        if (pps.present) {
            EXPECT_GT(pps.pps_pic_width_in_luma_samples, 0);
            EXPECT_GT(pps.pps_pic_height_in_luma_samples, 0);
            EXPECT_LE(pps.pps_pic_width_in_luma_samples, kMaxPictureSize);
            EXPECT_LE(pps.pps_pic_height_in_luma_samples, kMaxPictureSize);
        }
    }

    // vvcC 配置记录：只需要"不崩 / 不挂"，valid 时尺寸由 u16 承载必然有界
    for (int iter = 0; iter < kIterations; ++iter) {
        std::vector<uint8_t> mutated = kVvcCRecord;
        const int flips = 1 + static_cast<int>(rng() % 3);
        for (int f = 0; f < flips; ++f) {
            const size_t byte = rng() % mutated.size();
            mutated[byte] ^= static_cast<uint8_t>(1u << (rng() % 8));
        }
        auto task = std::async(std::launch::async, [&mutated]() {
            return ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC, mutated.data(), mutated.size());
        });
        ASSERT_EQ(task.wait_for(std::chrono::seconds(2)), std::future_status::ready)
            << "位翻转后的 vvcC 让解析器卡住了";
        const ExtradataResult parsed = task.get();
        if (parsed.valid) {
            EXPECT_GE(parsed.config.max_picture_width, 0);
            EXPECT_LE(parsed.config.max_picture_width, 0xFFFF);
            EXPECT_LE(parsed.config.max_picture_height, 0xFFFF);
        }
    }
}

} // namespace
