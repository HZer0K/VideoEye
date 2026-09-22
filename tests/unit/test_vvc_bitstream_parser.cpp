#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/analyzer/VvcBitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace {

using videoeye::analyzer::VvcBitstreamParser;
using videoeye::model::VvcPpsInfo;
using videoeye::model::VvcSpsInfo;
using videoeye::model::VvcVpsInfo;
using videoeye::utils::ExtradataFormat;
using videoeye::utils::ExtradataParser;
using videoeye::utils::ExtradataResult;
using videoeye::utils::NalSyntax;
using videoeye::utils::NalUnit;

// 测试数据由 _smoke/gen_vvc_ps.py 按 H.266 7.3.2.x 逐位生成。
// 语法顺序以 FFmpeg libavcodec/cbs_h266_syntax_template.c 为准。
// NalUnit::data 只含 RBSP payload（2 字节 NAL header 已由 ExtradataParser 剥离）。

// VPS: id=1 / Main 10 / Level 3.1(idc=51) / 单层
const std::vector<uint8_t> kVpsMain10 = {
    0x10, 0x00, 0x00, 0x03, 0x02, 0x33, 0x80, 0x00, 0x40,
};

// SPS A: 1920x1080 4:2:0 10bit Main10 L3.1 VUI(BT.2020 + PQ + 16:9)
const std::vector<uint8_t> kSpsMain10_1080p = {
    0x01, 0x0B, 0x02, 0x33, 0x80, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x43, 0x91,
    0x88, 0x04, 0xBA, 0xD5, 0x7B, 0xFF, 0xFC, 0x6F, 0x6B, 0xFB, 0xFF, 0xB9,
    0x89, 0x80, 0x88, 0x05, 0x09, 0x10, 0x09, 0x00, 0x40,
};

// SPS B: 1920x1088，conformance window bottom=4 → 显示高度 1080；8bit；无 VUI
const std::vector<uint8_t> kSpsMain10_1080pCrop = {
    0x01, 0x0B, 0x02, 0x33, 0x80, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x44, 0x1F,
    0x2A, 0x20, 0x12, 0xEB, 0x55, 0xEF, 0xFF, 0xF1, 0xBD, 0xAF, 0xEF, 0xFE,
    0xE6, 0x08,
};

// SPS C: 3840x2160 4:2:2 12bit，profile_idc=33 (Main 10 4:4:4)，Level 6.2(idc=102)
const std::vector<uint8_t> kSpsMain444_4K = {
    0x01, 0x13, 0x42, 0x66, 0x80, 0x00, 0x00, 0x07, 0x80, 0x80, 0x08, 0x71,
    0x0A, 0x20, 0x12, 0xEB, 0x55, 0xEF, 0xFF, 0xF1, 0xBD, 0xAF, 0xEF, 0xFB,
    0x98, 0x94, 0x82, 0x12, 0x20, 0x12, 0x00, 0x40,
};

// PPS: id=0 / sps_id=0 / 1920x1080（与 SPS A 一致）
const std::vector<uint8_t> kPpsMain10 = {
    0x00, 0x00, 0x07, 0x81, 0x00, 0x21, 0xC8, 0x1F, 0x24, 0x12, 0x60, 0x20,
};

// vvcC 配置记录：Main 10 / L3.1 / 4:2:0 10bit / 1920x1080 / 无 NAL 数组
const std::vector<uint8_t> kVvcCRecord = {
    0xFF,                    // reserved(5)=11111 | lengthSizeMinusOne(2)=3 | ptl_present(1)
    0x00, 0x01,              // ols_idx(9)=0 | num_sublayers(3)=0 | cfr(2)=0 | chroma(2)=1
    0x40,                    // bit_depth_minus8(3)=2 | reserved(5)
    0x01,                    // reserved(2) | num_bytes_constraint_info(6)=1
    0x02,                    // general_profile_idc(7)=1 | general_tier_flag(1)=0
    0x33,                    // general_level_idc = 51 (3.1)
    0x80,                    // frame_only(1) | multilayer(1) | constraint info(6)
    0x00,                    // num_sub_profiles = 0
    0x07, 0x80,              // max_picture_width  = 1920
    0x04, 0x38,              // max_picture_height = 1080
    0x00, 0x00,              // avg_frame_rate
    0x00,                    // num_of_arrays = 0
};

NalUnit MakeNal(uint8_t type, const std::vector<uint8_t>& payload) {
    // VVC 的随机访问点：IDR_W_RADL(7) / IDR_N_LP(8) / CRA_NUT(9) / GDR_NUT(10)
    const bool keyframe = (type >= 7 && type <= 10);
    return NalUnit{type, payload.size(), payload, (type == 7 || type == 8), keyframe};
}

// 手工拼一个带 NAL header 的 VVC NAL（type 在第 2 字节高 5 位）
std::vector<uint8_t> WithVvcHeader(uint8_t type, uint8_t temporal_id_plus1,
                                   const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.push_back(0x00);                                  // forbidden=0 reserved=0 layer_id=0
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
    const VvcSpsInfo sps =
        VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, kSpsMain10_1080pCrop));
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
    // VPS id 越界 / 语法跑飞时允许解析失败，但绝不能崩、也不能把垃圾当结果
    if (sps.present) {
        EXPECT_TRUE(sps.width() >= 0);
        EXPECT_TRUE(sps.height() >= 0);
    }
    const VvcSpsInfo empty = VvcBitstreamParser::ParseSpsFromNalUnit(MakeNal(15, {}));
    EXPECT_FALSE(empty.present);
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

    pps.chroma_format_idc = 1;   // 实际由 BitstreamAnalyzer 从 SPS 拷入
    EXPECT_EQ(pps.SubWidthC(), 2);
    EXPECT_EQ(pps.SubHeightC(), 2);
    EXPECT_EQ(pps.width(), 1920);
    EXPECT_EQ(pps.height(), 1080);

    EXPECT_FALSE(pps.pps_mixed_nalu_types_in_pic_flag);
    EXPECT_FALSE(pps.pps_no_pic_partition_flag);
    EXPECT_FALSE(pps.pps_output_flag_present_flag);
}

// --------------------------------------------------------------------------
// vvcC 配置记录
// --------------------------------------------------------------------------
class VvcConfigRecordTest : public testing::Test {};

TEST_F(VvcConfigRecordTest, ParseVvcC) {
    const ExtradataResult result =
        ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC,
                                         kVvcCRecord.data(), kVvcCRecord.size());
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
    EXPECT_TRUE(result.nal_units.empty());   // num_of_arrays = 0
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
    0xFF, 0x00, 0x65, 0x5F, 0x01, 0x02, 0x23, 0x80, 0x00, 0x00, 0x02, 0x80,
    0x01, 0x68, 0x00, 0x00, 0x02, 0x8F, 0x00, 0x01, 0x00, 0xF3, 0x00, 0x79,
    0x00, 0xAB, 0x02, 0x23, 0x80, 0x00, 0x00, 0x80, 0x14, 0x08, 0x05, 0xA4,
    0x6A, 0x00, 0x73, 0x7A, 0x21, 0x34, 0x52, 0x7B, 0xCE, 0x13, 0x65, 0x63,
    0x04, 0x08, 0x27, 0x00, 0x13, 0x10, 0x10, 0x41, 0x04, 0x20, 0x61, 0x08,
    0x42, 0xC4, 0x42, 0x16, 0x48, 0x42, 0xD4, 0x21, 0x7A, 0x3D, 0x5A, 0x92,
    0xF2, 0x49, 0xA9, 0x2C, 0x91, 0x16, 0xA2, 0x2F, 0x11, 0x26, 0xA2, 0x24,
    0x52, 0x44, 0x49, 0x92, 0x22, 0x4D, 0x46, 0x58, 0x88, 0x42, 0xC9, 0x08,
    0x5A, 0x84, 0x2F, 0x08, 0x49, 0xA8, 0x42, 0x45, 0x24, 0x21, 0x26, 0x48,
    0x42, 0x5D, 0x49, 0x08, 0x48, 0x48, 0x88, 0x42, 0x45, 0x11, 0x08, 0x49,
    0x88, 0x84, 0x25, 0xD4, 0x44, 0x21, 0x22, 0x8C, 0x84, 0x24, 0xC6, 0x42,
    0x12, 0xEA, 0x32, 0x10, 0xA0, 0x41, 0x61, 0x08, 0x20, 0x31, 0x10, 0x81,
    0x92, 0x20, 0xD4, 0x80, 0x4C, 0xC1, 0x04, 0x10, 0x58, 0x40, 0x08, 0x2C,
    0x40, 0x20, 0x21, 0x02, 0x01, 0x01, 0x50, 0x81, 0x00, 0x80, 0xD2, 0x10,
    0x20, 0x10, 0x16, 0x20, 0x40, 0x20, 0x22, 0x08, 0x04, 0x05, 0x90, 0x40,
    0x20, 0x24, 0x20, 0x10, 0x32, 0x02, 0x02, 0x21, 0x01, 0x01, 0x64, 0x20,
    0x20, 0x24, 0x40, 0x40, 0xD0, 0x20, 0x24, 0x81, 0x03, 0x82, 0x06, 0x20,
    0x10, 0xB2, 0x04, 0x08, 0x84, 0x08, 0x16, 0x42, 0x04, 0x09, 0x10, 0x20,
    0xD0, 0x40, 0x92, 0x08, 0x38, 0x41, 0x90, 0x22, 0xD0, 0x82, 0x48, 0x43,
    0x88, 0x68, 0x4B, 0x91, 0xCA, 0x81, 0x05, 0x84, 0x00, 0x82, 0xC4, 0x02,
    0x02, 0x10, 0x20, 0x10, 0x15, 0x08, 0x10, 0x08, 0x1F, 0x7A, 0x5D, 0x8A,
    0xCE, 0x48, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xF0,
    0xC4, 0x90, 0x00, 0x01, 0x00, 0x0C, 0x00, 0x81, 0x00, 0x00, 0x0A, 0x04,
    0x02, 0xD2, 0x22, 0x41, 0xFB, 0x82,
};

class VvcRealEncoderTest : public testing::Test {};

TEST_F(VvcRealEncoderTest, ParseRealVvcC) {
    const ExtradataResult result =
        ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC,
                                         kVvcCRealLibvvenc.data(),
                                         kVvcCRealLibvvenc.size());
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
    EXPECT_EQ(result.nal_units[0].type, 15);   // SPS
    EXPECT_EQ(result.nal_units[1].type, 16);   // PPS
}

TEST_F(VvcRealEncoderTest, ParseRealSpsAndPps) {
    const ExtradataResult result =
        ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC,
                                         kVvcCRealLibvvenc.data(),
                                         kVvcCRealLibvvenc.size());
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

}  // namespace
