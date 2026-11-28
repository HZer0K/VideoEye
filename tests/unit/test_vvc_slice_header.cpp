// H.266 (VVC) 帧级 slice_header 解析的回归测试。
//
// 被测的是 VvcSliceHeaderParser：它只回答两个问题 ——
//   1. 这个 slice 的 sh_slice_type 是什么（以及它是不是一幅新图像的开头）
//   2. 这个 slice 的 SliceQpY 是多少（**只在整条位宽路径可证固定时**才回答）
//
// 期望值全部按 H.266 7.3.2.9（slice_header）与 7.4.3.3（QP 范围）独立给出，
// 参数集直接以结构体注入，**不**经过 SPS/PPS 解析器 —— 这样变量只有一个：
// slice_header 的字段顺序与"哪些位真的存在"。
//
// 本文件逐个钉住的坑：
//   1. slice_type 之前有地址字段，宽度 = ceil(log2(NumTilesInPic)) 或
//      ceil(log2(NumSlicesInSubpic))。宽度算错不会报错，只会把 I 读成 P。
//   2. sh_extra_bit 是按 SPS 的 present 标志逐位出现的，不是按字节数。
//   3. sh_slice_type 之后、sh_qp_delta 之前还有 sh_no_output_of_prior_pics_flag
//      （只有 IDR/CRA/GDR 有）与 ALF 段（定长，但分支不少）。
//   4. 非 I slice 的 QP **不该**给：前面有 ref_pic_lists()/pred_weight_table()
//      可变段，猜一个数会"看着完全合理"。
//   5. 拒绝时必须带具体原因（qp_unavailable_reason 靠它显示）。

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/VvcSliceHeader.h"
#include "core/domain/model/VvcBitstreamInfo.h"
#include "core/media/codec/BitReader.h"

namespace {

using videoeye::BitReader;
using videoeye::VvcPhPrefix;
using videoeye::VvcSliceHeaderInfo;
using videoeye::VvcSliceHeaderParser;
using videoeye::model::VvcPpsInfo;
using videoeye::model::VvcSpsInfo;

// H.266 7.4.1 Table 5
constexpr int kIdrWRadl = 7;
constexpr int kIdrNLp = 8;
constexpr int kCra = 9;
constexpr int kGdr = 10;
constexpr int kTrail = 0;

// MSB-first 位写入器（与 test_vvc_frame_order_tracker.cpp 同形）。
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
    void Se(int32_t value) {
        // se(v) → ue(v)：正数 2v-1，负数 -2v
        Ue(value <= 0 ? static_cast<uint32_t>(-2 * value) : static_cast<uint32_t>(2 * value - 1));
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

// 默认 SPS：8bit / 4:2:0 / 无 ALF / 无 LMCS / 无显式缩放矩阵 / 不携带 IDR 的 RPL /
// 无 subpic / 无 extra_sh_bit。凡是要触发某条拒绝分支，就在副本上改一个开关。
VvcSpsInfo MakeSps() {
    VvcSpsInfo sps;
    sps.present = true;
    sps.sps_seq_parameter_set_id = 0;
    sps.sps_chroma_format_idc = 1;
    sps.sps_bitdepth_minus8 = 0;
    sps.sps_subpic_info_present_flag = false;
    sps.sps_num_subpics_minus1 = 0;
    sps.sps_num_extra_sh_bytes = 0;
    sps.sps_alf_enabled_flag = false;
    sps.sps_ccalf_enabled_flag = false;
    sps.sps_lmcs_enabled_flag = false;
    sps.sps_explicit_scaling_list_enabled_flag = false;
    sps.sps_idr_rpl_present_flag = false;
    return sps;
}

// 默认 PPS：no_pic_partition（整幅一个 tile / 一个 slice ⇒ 没有地址字段）。
VvcPpsInfo MakePps() {
    VvcPpsInfo pps;
    pps.present = true;
    pps.pps_pic_parameter_set_id = 0;
    pps.pps_seq_parameter_set_id = 0;
    pps.pps_no_pic_partition_flag = true;
    pps.pps_rect_slice_flag = true;
    pps.pps_single_slice_per_subpic_flag = true;
    pps.pps_num_slices_in_pic_minus1 = 0;
    pps.pps_num_tiles_in_pic = 1;
    pps.pps_pic_init_qp_minus26 = 4; // SliceQpY 基准 = 30
    pps.pps_rpl_info_in_ph_flag = false;
    pps.pps_alf_info_in_ph_flag = false;
    return pps;
}

VvcPhPrefix MakePh(bool inter_allowed = true, int pps_id = 0) {
    VvcPhPrefix ph;
    ph.valid = true;
    ph.pps_id = pps_id;
    ph.inter_slice_allowed = inter_allowed;
    ph.intra_slice_allowed = true;
    return ph;
}

VvcSliceHeaderInfo Parse(const std::vector<uint8_t>& rbsp, int nal_type, const VvcPhPrefix& ph, const VvcSpsInfo& sps,
                         const VvcPpsInfo& pps) {
    return VvcSliceHeaderParser::Parse(rbsp.data(), rbsp.size(), nal_type, ph, sps, pps);
}

// --------------------------------------------------------------------------
// sh_slice_type
// --------------------------------------------------------------------------

// VVC 码流里 0=B / 1=P / 2=I，项目内部统一成 0=P / 1=B / 2=I（与 H.264 的
// slice_type % 5 同口径）。三个值都要过一遍，否则"B 与 P 互换"这种错看不出来。
TEST(VvcSliceHeaderTests, SliceTypeMappingFollowsStreamOrder) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    struct Case {
        uint32_t stream_value;
        int expected;
    };
    const Case cases[] = {{0, 1}, {1, 0}, {2, 2}};
    for (const Case& c : cases) {
        BitWriter w;
        w.Bit(0); // sh_picture_header_in_slice_header_flag
        w.Ue(c.stream_value);
        const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
        EXPECT_TRUE(info.parsed) << "stream slice_type=" << c.stream_value;
        EXPECT_EQ(info.slice_type, c.expected) << "stream slice_type=" << c.stream_value;
    }
}

// ph_inter_slice_allowed_flag == 0 ⇒ 规范推断 sh_slice_type = I，位流里没有这个字段。
// 若实现仍然去读一位，就会把后面的 sh_no_output_of_prior_pics_flag 当类型。
TEST(VvcSliceHeaderTests, InterSliceDisabledInfersISliceWithoutBits) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0); // 外部 PH
    w.Bit(1); // 这一位是 sh_no_output_of_prior_pics_flag（IDR），不是 slice_type
    w.Se(0);  // sh_qp_delta
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(/*inter_allowed=*/false), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_EQ(info.slice_type, 2);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 30);
}

// sh_slice_type 的 ue(v) 超过 2 是非法值：必须判为没解出来，不能夹断成 I。
TEST(VvcSliceHeaderTests, SliceTypeAboveTwoIsRejected) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Ue(3);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    EXPECT_FALSE(info.parsed);
    EXPECT_FALSE(info.stop_reason.empty());
}

// --------------------------------------------------------------------------
// 地址字段宽度
// --------------------------------------------------------------------------

// 非矩形 slice + 多 tile：地址宽度 = ceil(log2(NumTilesInPic)) = 2（4 个 tile）。
// 宽度算成 1 位的话，类型会从剩下的位里读出一个"看着合理"的错值。
TEST(VvcSliceHeaderTests, TileCountDrivesSliceAddressWidth) {
    VvcSpsInfo sps = MakeSps();
    VvcPpsInfo pps = MakePps();
    pps.pps_no_pic_partition_flag = false;
    pps.pps_rect_slice_flag = false;
    pps.pps_single_slice_per_subpic_flag = true;
    pps.pps_num_tiles_in_pic = 4;

    BitWriter w;
    w.Bit(0);     // 外部 PH
    w.Bits(2, 2); // sh_slice_address = 2（2 位）
    w.Ue(0);      // sh_num_tiles_in_slice_minus1（地址 2 之后还剩 2 个 tile）
    w.Ue(2);      // sh_slice_type = I
    w.Bit(1);     // sh_no_output_of_prior_pics_flag (IDR)
    w.Se(-3);     // sh_qp_delta
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    EXPECT_EQ(info.slice_type, 2);
    EXPECT_FALSE(info.first_slice_of_picture); // 地址 2 ≠ 0
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 27);
}

// 地址 0 才是"这一 slice 开启一幅新图像"。巡检器的帧数全靠它。
TEST(VvcSliceHeaderTests, TileAddressZeroMarksFirstSliceOfPicture) {
    VvcSpsInfo sps = MakeSps();
    VvcPpsInfo pps = MakePps();
    pps.pps_no_pic_partition_flag = false;
    pps.pps_rect_slice_flag = false;
    pps.pps_single_slice_per_subpic_flag = true;
    pps.pps_num_tiles_in_pic = 4;

    BitWriter w;
    w.Bit(0);
    w.Bits(0, 2); // sh_slice_address = 0
    w.Ue(0);      // sh_num_tiles_in_slice_minus1
    w.Ue(2);      // I
    w.Bit(1);
    w.Se(0);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    EXPECT_TRUE(info.first_slice_of_picture);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 30);
}

// 单 tile ⇒ 地址宽度 0，字段根本不存在。多读一位就会把类型读错。
TEST(VvcSliceHeaderTests, SingleTileHasNoAddressField) {
    VvcSpsInfo sps = MakeSps();
    VvcPpsInfo pps = MakePps();
    pps.pps_no_pic_partition_flag = false;
    pps.pps_rect_slice_flag = false;
    pps.pps_single_slice_per_subpic_flag = true;
    pps.pps_num_tiles_in_pic = 1;

    BitWriter w;
    w.Bit(0);
    w.Ue(1); // 直接就是 sh_slice_type = P
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    EXPECT_EQ(info.slice_type, 0);
    EXPECT_TRUE(info.first_slice_of_picture);
}

// 矩形 slice + 每 subpic 一片 ⇒ 没有地址字段，但一幅图像可能有多片，
// 因此**不能**断言"这一片就是图像开头"（CurrSubpicIdx 需要 subpic id 映射表）。
TEST(VvcSliceHeaderTests, MultipleSubpicsDoNotClaimFirstSlice) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_subpic_info_present_flag = true;
    sps.sps_num_subpics_minus1 = 3; // 4 个 subpic
    sps.sps_subpic_id_len_minus1 = 7;
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Bits(0, 8); // sh_subpic_id
    w.Ue(2);      // I
    w.Bit(1);     // no_output（IDR）
    w.Se(0);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    EXPECT_EQ(info.slice_type, 2);
    EXPECT_FALSE(info.first_slice_of_picture) << "多 subpic 时没有映射表，不能断言这是图像第一片";
}

// 多 subpic 且不是"每 subpic 一片"：NumSlicesInSubpic 未知 ⇒ 地址宽度未知 ⇒ 拒绝。
TEST(VvcSliceHeaderTests, MultiSubpicWithoutSingleSliceIsRejected) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_subpic_info_present_flag = true;
    sps.sps_num_subpics_minus1 = 1;
    sps.sps_subpic_id_len_minus1 = 3;
    VvcPpsInfo pps = MakePps();
    pps.pps_single_slice_per_subpic_flag = false;

    BitWriter w;
    w.Bit(0);
    w.Bits(0, 4);
    w.Ue(2);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    EXPECT_FALSE(info.parsed);
    EXPECT_NE(info.stop_reason.find("subpic"), std::string::npos) << info.stop_reason;
}

// sh_extra_bit 按 SPS 的 present 标志逐位出现。少读一位，sh_slice_type 就错。
TEST(VvcSliceHeaderTests, ExtraSliceHeaderBitsAreConsumed) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_num_extra_sh_bytes = 1;
    sps.sps_extra_sh_bit_present_flag[0] = true;
    sps.sps_extra_sh_bit_present_flag[1] = true;
    sps.sps_extra_sh_bit_present_flag[3] = true; // 第 2 位不存在
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Bit(1); // extra bit[0]
    w.Bit(0); // extra bit[1]
    w.Bit(0); // extra bit[3]
    w.Ue(2);  // I
    w.Bit(1);
    w.Se(0);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    EXPECT_EQ(info.slice_type, 2);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 30);
}

// --------------------------------------------------------------------------
// QP：可证固定宽度的路径
// --------------------------------------------------------------------------

TEST(VvcSliceHeaderTests, IdrISliceYieldsQpFromPpsBase) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0); // 外部 PH
    w.Ue(2);  // I
    w.Bit(0); // sh_no_output_of_prior_pics_flag (IDR)
    w.Se(-4); // sh_qp_delta
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrNLp, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 26); // 26 + 4 - 4
    EXPECT_TRUE(info.stop_reason.empty());
}

// TRAIL 的 I slice：RPL 在 PH 里时长度字段都不存在，QP 仍可算。
// 注意规范的条件**不看** slice_type —— 少判一个 in_ph 就会去读 ref_pic_lists()。
TEST(VvcSliceHeaderTests, TrailISliceWithRplInPhYieldsQp) {
    const VvcSpsInfo sps = MakeSps();
    VvcPpsInfo pps = MakePps();
    pps.pps_rpl_info_in_ph_flag = true;

    BitWriter w;
    w.Bit(0);
    w.Ue(2); // I（TRAIL 的 I slice）
    w.Se(6); // 没有 no_output 位（非 IRAP）
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 36);
}

// ALF 段是**定长**的（u(3) 的 id 个数 + 每项 u(3)），所以有 ALF 也能拿到 QP。
// 少跳这一段，QP 会从一个 APS id 的位里读出来 —— 数值往往仍然"在范围内"。
TEST(VvcSliceHeaderTests, AlfBlockIsSkippedExactly) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_alf_enabled_flag = true;
    sps.sps_ccalf_enabled_flag = true; // 需要 chroma_format != 0 才存在，默认 4:2:0
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);     // 外部 PH
    w.Ue(2);      // I
    w.Bit(1);     // sh_no_output_of_prior_pics_flag
    w.Bit(1);     // sh_alf_enabled_flag
    w.Bits(2, 3); // sh_num_alf_aps_ids_luma
    w.Bits(1, 3); // sh_alf_aps_id_luma[0]
    w.Bits(3, 3); // sh_alf_aps_id_luma[1]
    w.Bit(1);     // sh_alf_cb_enabled_flag
    w.Bit(1);     // sh_alf_cr_enabled_flag
    w.Bits(5, 3); // sh_alf_aps_id_chroma
    w.Bit(1);     // sh_alf_cc_cb_enabled_flag
    w.Bits(2, 3); // sh_alf_cc_cb_aps_id
    w.Bit(1);     // sh_alf_cc_cr_enabled_flag
    w.Bits(6, 3); // sh_alf_cc_cr_aps_id
    w.Se(1);      // sh_qp_delta
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 31);
}

// 10bit 的下界是 -6*2 = -12：QP=14 之类"低于 0"的值在 10bit 上是合法的。
TEST(VvcSliceHeaderTests, TenBitAllowsNegativeQpDownToOffset) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_bitdepth_minus8 = 2;
    VvcPpsInfo pps = MakePps();
    pps.pps_pic_init_qp_minus26 = -20; // 基准 = 6

    BitWriter w;
    w.Bit(0);
    w.Ue(2);
    w.Bit(1);
    w.Se(-10); // 6 - 10 = -4，10bit 下界 -12，合法
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed) << info.stop_reason;
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, -4);
}

// 越界的 QP 一律丢弃：错位不报错，取值范围是最后一道筛。
TEST(VvcSliceHeaderTests, OutOfRangeQpIsRejected) {
    const VvcSpsInfo sps = MakeSps();
    VvcPpsInfo pps = MakePps();
    pps.pps_pic_init_qp_minus26 = 37; // 基准 = 63 > 51

    BitWriter w;
    w.Bit(0);
    w.Ue(2);
    w.Bit(1);
    w.Se(0);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_FALSE(info.has_qp);
    EXPECT_NE(info.stop_reason.find("越界"), std::string::npos) << info.stop_reason;
}

// --------------------------------------------------------------------------
// QP：应当拒绝（并给出具体原因）的四种情形
// --------------------------------------------------------------------------

TEST(VvcSliceHeaderTests, NonIntraSliceWithoutRplDataReportsWhy) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    // 只写 inline_ph + 类型就结束：P slice 后面必须跟两条 RPL，缺了就是截断。
    // 要点不是"拒绝"这件事，而是拒绝的理由必须指向 ref_pic_lists()，
    // 别说成"非 I slice 一律不支持" —— 那是旧实现的能力缺失，不是规范结论。
    BitWriter w;
    w.Bit(0);
    w.Ue(1); // P
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_EQ(info.slice_type, 0);
    EXPECT_FALSE(info.has_qp);
    EXPECT_NE(info.stop_reason.find("ref_pic_lists()"), std::string::npos) << info.stop_reason;
}

// --------------------------------------------------------------------------
// P slice 走完整 RPL 通道后应当真的给出 QP
//
// 这是本次改动的核心：以前"非 I slice 一律不给 QP"，理由是 RPL 与
// pred_weight_table 是可变长段。现在按规范把这两段逐位跳过，P slice 也能给。
// --------------------------------------------------------------------------

TEST(VvcSliceHeaderTests, PSliceWithSpsRplGetsQp) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_num_ref_pic_lists[0] = 1;
    sps.sps_num_ref_pic_lists[1] = 0;
    sps.rpl_recorded = true;
    auto& list = sps.sps_ref_pic_lists[0][0];
    list.recorded = true;
    list.num_ref_entries = 2;
    list.ltrp_in_header_flag = false;
    list.inter_layer_ref_pic_flag = {0, 0};
    list.st_ref_pic_flag = {1, 1}; // 两项都是短期参考 ⇒ num_ltrp_entries = 0

    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0); // sh_picture_header_in_slice_header_flag
    w.Ue(1);  // P
    // ref_pic_lists()：
    //   list 0 —— sps_num_ref_pic_lists[0] == 1 ⇒ 读 rpl_sps_flag，rpl_idx 推断为 0
    w.Bit(1); // rpl_sps_flag[0] = 1（用 SPS 里那一条）
    //   list 1 —— sps_num_ref_pic_lists[1] == 0 ⇒ rpl_sps_flag 推断 0，内联一条
    w.Ue(0); // num_ref_entries = 0
    // sh_num_ref_idx_active_override_flag：num_entries[0] == 2 > 1 ⇒ 存在
    w.Bit(0); // 不覆盖 ⇒ num_ref_idx_active[0] = min(2, 默认 1) = 1
    w.Se(2);  // sh_qp_delta

    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_EQ(info.slice_type, 0);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 26 + 4 + 2);
}

TEST(VvcSliceHeaderTests, IntraSliceWithInlineRplGetsQp) {
    // I slice 也照样要读 ref_pic_lists()（条件不看 slice_type）。
    // 两条 list 都内联，各带一个空的 num_ref_entries。
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Ue(2); // I
    w.Ue(0); // list 0 内联：num_ref_entries = 0
    w.Ue(0); // list 1 内联：num_ref_entries = 0
    w.Se(-1);

    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 26 + 4 - 1);
}

// 加权预测：pred_weight_table() 的每一项都带"标志位 + 可选参数"，
// 少读一个 chroma 标志后面就整段错位 —— 但错位后 QP 仍在合法范围内，
// 只有"读出来的 QP 不等于期望值"能抓到。
TEST(VvcSliceHeaderTests, PSliceWithWeightedPredictionGetsQp) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_num_ref_pic_lists[0] = 1;
    sps.sps_num_ref_pic_lists[1] = 0;
    sps.rpl_recorded = true;
    auto& list = sps.sps_ref_pic_lists[0][0];
    list.recorded = true;
    list.num_ref_entries = 2;
    list.ltrp_in_header_flag = false;
    list.inter_layer_ref_pic_flag = {0, 0};
    list.st_ref_pic_flag = {1, 1};

    VvcPpsInfo pps = MakePps();
    pps.pps_weighted_pred_flag = true; // ⇒ P slice 里出现 pred_weight_table()

    BitWriter w;
    w.Bit(0);
    w.Ue(1);  // P
    w.Bit(1); // rpl_sps_flag[0] = 1
    w.Ue(0);  // list 1 内联：num_ref_entries = 0
    w.Bit(0); // sh_num_ref_idx_active_override_flag = 0 ⇒ num_ref_idx_active[0] = 1
    // pred_weight_table()：luma denom + 两条标志 + 各自的差值
    w.Ue(0);  // luma_log2_weight_denom
    w.Se(0);  // delta_chroma_log2_weight_denom（chroma_format_idc != 0 才有）
    w.Bit(1); // luma_weight_l0_flag[0]
    w.Bit(1); // chroma_weight_l0_flag[0]
    w.Se(1);  // delta_luma_weight_l0
    w.Se(0);  // luma_offset_l0
    w.Se(1);  // delta_chroma_weight_l0[0][0]
    w.Se(0);  // delta_chroma_offset_l0[0][0]
    w.Se(1);  // delta_chroma_weight_l0[0][1]
    w.Se(0);  // delta_chroma_offset_l0[0][1]
    w.Se(3);  // sh_qp_delta

    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 26 + 4 + 3);
}

// 内联 RPL + 长期参考：ltrp_in_header_flag 在"内联位置"被规范推断为 1，
// 也就是说 **码流里没有这一位**，长期参考项的 poc_lsb 也不出现在 RPL 里。
// 照 SPS 的分支去读会多吃一位，后面整体错位 —— 而 QP 可能仍在合法范围内。
TEST(VvcSliceHeaderTests, InlineRplWithLongTermRefInfersLtrpInHeader) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_long_term_ref_pics_flag = true;

    const VvcPpsInfo pps = MakePps();
    const int poc_bits = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;

    BitWriter w;
    w.Bit(0);
    w.Ue(2); // I
    // list 0 内联：3 项，长期 / 短期 / 长期**交错**。
    //
    // ❗ 交错是必须的。只有 1 个长期项时这个用例判别不了 ltrp_in_header 的推断：
    //    "项循环少读 7 位 poc_lsb_lt" 与 "ltrp 段多读 7 位" 位数相同、位置相邻，
    //    读者落点一模一样 —— 那时把推断值改成 false 也测不出差异（实测判绿）。
    //    3 项里两个长期项被一个短期项隔开，poc_lsb_lt 的位置就与短期项字段
    //    咬合在一起，推断错一位立即整体错位。
    w.Ue(3);  // num_ref_entries
    w.Bit(0); // [0] st_ref_pic_flag = 0（长期）
    w.Bit(1); // [1] st_ref_pic_flag = 1（短期）
    w.Ue(0);  // [1] abs_delta_poc_minus1 = 0
    w.Bit(0); // [1] strp_entry_sign_flag（abs_delta_poc_st = 1 > 0 才有这一位）
    w.Bit(0); // [2] st_ref_pic_flag = 0（长期）
    // ❗ 内联位置 ltrp_in_header 恒为 1 ⇒ 项循环里**没有** rpls_poc_lsb_lt
    // ltrp 段：num_ltrp_entries = 2
    w.Bits(7, poc_bits); // [0] poc_lsb_lt
    w.Bit(0);            // [0] delta_poc_msb_cycle_present_flag = 0
    w.Bits(5, poc_bits); // [1] poc_lsb_lt
    w.Bit(0);            // [1] delta_poc_msb_cycle_present_flag = 0
    // list 1 内联：空
    w.Ue(0);
    w.Se(1);

    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 26 + 4 + 1);
}

TEST(VvcSliceHeaderTests,
     TemporalMvpBlocksNonIntraQp) { // sh_collocated_* 段是否存在取决于 PH 的 ph_temporal_mvp_enabled_flag，
    // 而 PH 不在这里解析 —— 只能拒绝，并且说清楚是这个原因。
    VvcSpsInfo sps = MakeSps();
    sps.sps_temporal_mvp_enabled_flag = true;
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Ue(1); // P
    w.Ue(0); // list 0 内联
    w.Ue(0); // list 1 内联
    w.Se(0);

    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_FALSE(info.has_qp);
    EXPECT_NE(info.stop_reason.find("temporal MVP"), std::string::npos) << info.stop_reason;
}

TEST(VvcSliceHeaderTests, QpDeltaInPictureHeaderIsRejected) {
    const VvcSpsInfo sps = MakeSps();
    VvcPpsInfo pps = MakePps();
    pps.pps_qp_delta_info_in_ph_flag = true;

    BitWriter w;
    w.Bit(0);
    w.Ue(2);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_FALSE(info.has_qp);
    EXPECT_NE(info.stop_reason.find("picture header"), std::string::npos) << info.stop_reason;
}

TEST(VvcSliceHeaderTests, LmcsEnabledBlocksQp) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_lmcs_enabled_flag = true;
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Ue(2);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_FALSE(info.has_qp);
    EXPECT_NE(info.stop_reason.find("LMCS"), std::string::npos) << info.stop_reason;
}

TEST(VvcSliceHeaderTests, ExplicitScalingListBlocksQp) {
    VvcSpsInfo sps = MakeSps();
    sps.sps_explicit_scaling_list_enabled_flag = true;
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Ue(2);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    EXPECT_FALSE(info.has_qp);
    EXPECT_NE(info.stop_reason.find("缩放矩阵"), std::string::npos) << info.stop_reason;
}

// IDR + SPS 声明携带 RPL ⇒ ref_pic_lists() 就在 slice 里，QP 不给。
TEST(VvcSliceHeaderTests, IdrWithRplPresentInSpsYieldsQp) {
    // IDR 一般不带 RPL，但 sps_idr_rpl_present_flag=1 时带 —— 这条路径以前是
    // "一律拒绝"，现在能精确跳过两条内联 RPL 并给出 QP。
    VvcSpsInfo sps = MakeSps();
    sps.sps_idr_rpl_present_flag = true;
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Ue(2);  // I
    w.Bit(0); // sh_no_output_of_prior_pics_flag（IDR 才有）
    w.Ue(0);  // list 0 内联
    w.Ue(0);  // list 1 内联
    w.Se(3);

    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kIdrWRadl, MakePh(), sps, pps);
    ASSERT_TRUE(info.parsed);
    ASSERT_TRUE(info.has_qp) << info.stop_reason;
    EXPECT_EQ(info.qp, 26 + 4 + 3);
}

// --------------------------------------------------------------------------
// 拒绝的形态：内联 PH / 缺 PH / 空载荷 / 截断
// --------------------------------------------------------------------------

TEST(VvcSliceHeaderTests, InlinePictureHeaderIsRejected) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(1); // sh_picture_header_in_slice_header_flag
    w.Ue(2);
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, MakePh(), sps, pps);
    EXPECT_FALSE(info.parsed);
    EXPECT_NE(info.stop_reason.find("内联"), std::string::npos) << info.stop_reason;
}

TEST(VvcSliceHeaderTests, MissingPictureHeaderIsRejected) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    BitWriter w;
    w.Bit(0);
    w.Ue(2);
    VvcPhPrefix ph; // valid = false
    const VvcSliceHeaderInfo info = Parse(w.Bytes(), kTrail, ph, sps, pps);
    EXPECT_FALSE(info.parsed);
    EXPECT_NE(info.stop_reason.find("picture header"), std::string::npos) << info.stop_reason;
}

TEST(VvcSliceHeaderTests, EmptyPayloadIsRejected) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();
    const VvcSliceHeaderInfo info = VvcSliceHeaderParser::Parse(nullptr, 0, kTrail, MakePh(), sps, pps);
    EXPECT_FALSE(info.parsed);
    EXPECT_FALSE(info.stop_reason.empty());
}

// 位流在字段中间就断了：不能拿"补 0 读出的值"当结果。
TEST(VvcSliceHeaderTests, TruncatedPayloadIsRejected) {
    const VvcSpsInfo sps = MakeSps();
    const VvcPpsInfo pps = MakePps();

    // 只给 1 字节：flag=0 + 一个 5 位前缀的 ue 开头，必然读不完。
    const std::vector<uint8_t> rbsp = {0x00};
    const VvcSliceHeaderInfo info = Parse(rbsp, kTrail, MakePh(), sps, pps);
    EXPECT_FALSE(info.parsed);
    EXPECT_FALSE(info.stop_reason.empty());
}

// --------------------------------------------------------------------------
// PH 前缀
// --------------------------------------------------------------------------

TEST(VvcSliceHeaderTests, PhPrefixReadsFlagsAndPpsId) {
    BitWriter w;
    w.Bit(1); // ph_gdr_or_irap_pic_flag
    w.Bit(1); // ph_non_ref_pic_flag
    w.Bit(0); // ph_gdr_pic_flag
    w.Bit(1); // ph_inter_slice_allowed_flag
    w.Bit(0); // ph_intra_slice_allowed_flag
    w.Ue(5);  // ph_pic_parameter_set_id
    const std::vector<uint8_t> payload = w.Bytes();

    BitReader reader;
    reader.Reset(payload.data(), payload.size());
    VvcPhPrefix ph;
    ASSERT_TRUE(VvcSliceHeaderParser::ReadPhPrefix(reader, ph));
    EXPECT_TRUE(ph.valid);
    EXPECT_EQ(ph.pps_id, 5);
    EXPECT_TRUE(ph.inter_slice_allowed);
    EXPECT_FALSE(ph.intra_slice_allowed);
}

// inter_slice_allowed == 0 时 ph_intra_slice_allowed_flag 不存在，不能多读一位。
TEST(VvcSliceHeaderTests, PhPrefixSkipsIntraFlagWhenInterDisabled) {
    BitWriter w;
    w.Bit(0); // ph_gdr_or_irap_pic_flag
    w.Bit(0); // ph_non_ref_pic_flag
    w.Bit(0); // ph_inter_slice_allowed_flag
    w.Ue(1);  // ph_pic_parameter_set_id
    const std::vector<uint8_t> payload = w.Bytes();

    BitReader reader;
    reader.Reset(payload.data(), payload.size());
    VvcPhPrefix ph;
    ASSERT_TRUE(VvcSliceHeaderParser::ReadPhPrefix(reader, ph));
    EXPECT_EQ(ph.pps_id, 1);
    EXPECT_FALSE(ph.inter_slice_allowed);
    EXPECT_TRUE(ph.intra_slice_allowed); // 推导为 1
}

TEST(VvcSliceHeaderTests, PhPrefixRejectsEmptyPayload) {
    BitReader reader;
    VvcPhPrefix ph;
    EXPECT_FALSE(VvcSliceHeaderParser::ReadPhPrefix(reader, ph));
    EXPECT_FALSE(ph.valid);
}

} // namespace
