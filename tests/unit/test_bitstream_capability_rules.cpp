#include <gtest/gtest.h>

#include <cstdint>

#include "core/analysis/codec/BitstreamCapabilityRules.h"

namespace {

using videoeye::codec::CapabilityGap;
using videoeye::codec::H264ProfileGap;
using videoeye::codec::H264SpsGap;
using videoeye::codec::HevcSpsGap;
using videoeye::codec::IsValidatedBitDepth;
using videoeye::codec::IsValidatedChromaFormat;
using videoeye::codec::IsValidatedH264Profile;
using videoeye::codec::IsValidatedH264Sps;
using videoeye::codec::IsValidatedHevcProfile;
using videoeye::codec::IsValidatedHevcSps;

videoeye::model::H264SpsInfo MakeH264Sps(int profile_idc, int chroma, int luma_minus8) {
    videoeye::model::H264SpsInfo sps;
    sps.present = true;
    sps.profile_idc = profile_idc;
    sps.chroma_format_idc = chroma;
    sps.bit_depth_luma_minus8 = luma_minus8;
    return sps;
}

videoeye::model::HevcSpsInfo MakeHevcSps(int profile, int chroma, int luma_minus8) {
    videoeye::model::HevcSpsInfo sps;
    sps.present = true;
    sps.general_profile_idc = profile;
    sps.chroma_format_idc = chroma;
    sps.bit_depth_luma_minus8 = luma_minus8;
    return sps;
}

}  // namespace

// ==========================================================================
// 单维度
// ==========================================================================

TEST(BitstreamCapabilityRulesTests, ChromaFormatAllows422And444) {
    // C2 的核心：4:2:0 / 4:2:2 / 4:4:4 全部放行。
    // 以前只认 chroma_format_idc == 1，4:2:2 10bit 的贡献流因此永远判 Partial、
    // 也不参与容器一致性比对。
    EXPECT_TRUE(IsValidatedChromaFormat(1));  // 4:2:0
    EXPECT_TRUE(IsValidatedChromaFormat(2));  // 4:2:2
    EXPECT_TRUE(IsValidatedChromaFormat(3));  // 4:4:4

    // 4:0:0 与越界值继续拦：面板没有它的比对通路
    EXPECT_FALSE(IsValidatedChromaFormat(0));
    EXPECT_FALSE(IsValidatedChromaFormat(4));
    EXPECT_FALSE(IsValidatedChromaFormat(-1));
}

TEST(BitstreamCapabilityRulesTests, BitDepthAllows12) {
    EXPECT_TRUE(IsValidatedBitDepth(8));
    EXPECT_TRUE(IsValidatedBitDepth(10));
    EXPECT_TRUE(IsValidatedBitDepth(12));  // RExt 的 4:2:2 12bit 是广电常见组合
    EXPECT_FALSE(IsValidatedBitDepth(0));
    EXPECT_FALSE(IsValidatedBitDepth(9));
    EXPECT_FALSE(IsValidatedBitDepth(14));
}

TEST(BitstreamCapabilityRulesTests, H264ProfileWhitelist) {
    EXPECT_TRUE(IsValidatedH264Profile(66));   // Baseline
    EXPECT_TRUE(IsValidatedH264Profile(77));   // Main
    EXPECT_TRUE(IsValidatedH264Profile(100));  // High
    EXPECT_TRUE(IsValidatedH264Profile(110));  // High 10
    EXPECT_TRUE(IsValidatedH264Profile(122));  // High 4:2:2 —— 4:2:2 流的归处
    EXPECT_TRUE(IsValidatedH264Profile(244));  // High 4:4:4 Predictive
    EXPECT_FALSE(IsValidatedH264Profile(0));
    EXPECT_FALSE(IsValidatedH264Profile(999));
}

TEST(BitstreamCapabilityRulesTests, HevcProfileWhitelist) {
    EXPECT_TRUE(IsValidatedHevcProfile(1));   // Main
    EXPECT_TRUE(IsValidatedHevcProfile(2));   // Main 10
    EXPECT_TRUE(IsValidatedHevcProfile(3));   // Main Still Picture
    EXPECT_TRUE(IsValidatedHevcProfile(4));   // Range Extensions（4:2:2/4:4:4 的归处）
    EXPECT_TRUE(IsValidatedHevcProfile(9));   // Screen-Extended Main
    EXPECT_FALSE(IsValidatedHevcProfile(6));  // Multiview Main：无样本矩阵
    EXPECT_FALSE(IsValidatedHevcProfile(7));  // Scalable Main
    EXPECT_FALSE(IsValidatedHevcProfile(0));
}

// ==========================================================================
// 组合判据：C2 的关键回归
// ==========================================================================

TEST(BitstreamCapabilityRulesTests, H264_422HighProfileIsAccepted) {
    // 4:2:2 8bit High 4:2:2 —— C2 之前被 chroma==1 拦死
    const auto sps = MakeH264Sps(122, 2, 0);
    EXPECT_EQ(H264SpsGap(sps), CapabilityGap::None);
    EXPECT_TRUE(IsValidatedH264Sps(sps));
}

TEST(BitstreamCapabilityRulesTests, H264_444HighProfileIsAccepted) {
    const auto sps = MakeH264Sps(244, 3, 0);
    EXPECT_EQ(H264SpsGap(sps), CapabilityGap::None);
}

TEST(BitstreamCapabilityRulesTests, Hevc_422TenBitIsAccepted) {
    // 贡献流主力组合：general_profile_idc=4 (RExt) + 4:2:2 + 10bit。
    // 这是 C2 里被点名的那一条 —— 以前永远判 Partial。
    const auto sps = MakeHevcSps(4, 2, 2);
    EXPECT_EQ(HevcSpsGap(sps), CapabilityGap::None);
    EXPECT_TRUE(IsValidatedHevcSps(sps));
}

TEST(BitstreamCapabilityRulesTests, Hevc_422TwelveBitIsAccepted) {
    const auto sps = MakeHevcSps(4, 2, 4);
    EXPECT_EQ(HevcSpsGap(sps), CapabilityGap::None);
}

TEST(BitstreamCapabilityRulesTests, Hevc_444Main10IsAccepted) {
    const auto sps = MakeHevcSps(2, 3, 2);
    EXPECT_EQ(HevcSpsGap(sps), CapabilityGap::None);
}

TEST(BitstreamCapabilityRulesTests, GapIsReportedByDimension) {
    // 放开色度不等于什么都放行：不认识的 Profile / 位深仍要给出具体的 gap
    EXPECT_EQ(H264SpsGap(MakeH264Sps(9, 1, 0)), CapabilityGap::Profile);
    EXPECT_EQ(H264SpsGap(MakeH264Sps(100, 0, 0)), CapabilityGap::ChromaFormat);
    EXPECT_EQ(H264SpsGap(MakeH264Sps(100, 1, 6)), CapabilityGap::BitDepth);  // 14bit
    EXPECT_EQ(H264ProfileGap(9), CapabilityGap::Profile);

    EXPECT_EQ(HevcSpsGap(MakeHevcSps(6, 1, 0)), CapabilityGap::Profile);
    EXPECT_EQ(HevcSpsGap(MakeHevcSps(1, 0, 0)), CapabilityGap::ChromaFormat);
    EXPECT_EQ(HevcSpsGap(MakeHevcSps(1, 1, 8)), CapabilityGap::BitDepth);  // 16bit
}

TEST(BitstreamCapabilityRulesTests, GapNoteNamesTheOffendingField) {
    const std::string chroma_note = videoeye::codec::GapNote(
        "HEVC", CapabilityGap::ChromaFormat, "general_profile_idc=4",
        "chroma_format_idc=0", "10bit");
    EXPECT_NE(chroma_note.find("色度采样"), std::string::npos);
    EXPECT_NE(chroma_note.find("不参与容器一致性比对"), std::string::npos);

    const std::string bit_note = videoeye::codec::GapNote(
        "H.264", CapabilityGap::BitDepth, "profile_idc=100", "chroma_format_idc=1", "14bit");
    EXPECT_NE(bit_note.find("位深"), std::string::npos);

    // 没有 gap 就不该给文案（避免 UI 在 Supported 态下还挂着一句"仅供参考"）
    EXPECT_TRUE(videoeye::codec::GapNote(
                    "HEVC", CapabilityGap::None, "1", "1", "8bit")
                    .empty());
}
