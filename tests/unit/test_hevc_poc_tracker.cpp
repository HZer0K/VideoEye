// H.265 (HEVC) 帧级 POC 追踪的回归测试。
//
// 被测的是 HevcPocTracker：与 H.264 同一个问题（容器只给解码顺序，显示顺序
// 只能从 POC 推），但三处语义完全不同 —— NAL 切分、slice_segment_header 布局、
// POC 推导与复位点。所以这里的期望值**不能**照抄 H.264 那套的推理，全部按
// H.265 7.3.6.1（slice_segment_header）、8.3.1（POC）、8.1.3（NoRaslOutputFlag）
// 独立给出。
//
// HEVC 上最容易踩错、本文件逐个钉住的四件事：
//   1. slice_type 的编码顺序是 0=B / 1=P / 2=I（H.264 是 0=P / 1=B / 2=I）。
//      抄错的后果是 I/P/B 计数全反，而"B 帧数 = P 帧数"这种结果看着还挺合理。
//   2. IDR_W_RADL(19) / IDR_N_LP(20) **没有** slice_pic_order_cnt_lsb 字段。
//      去读它就会把后面的位吃掉，整条时间线的 POC 全错。
//   3. 复位点只有"IRAP 且 NoRaslOutputFlag == 1"，即 IDR/BLA 恒复位、
//      插在中间的 CRA **不复位**。见 ErrorOnIrapResets 的反例。
//   4. CRA 之后的 RASL（leading picture）POC 天生更小，那不是 B 帧重排。
//
// 期望值来源（**不**是把实现的输出抄一遍）：
//   * A 组：本文件里的 BitWriter 按上面那几节逐位拼出 SPS / PPS / slice，
//     期望值由规范语义直接给出（例：显示顺序 I B B P 时解码顺序里 POC 必然
//     回退一次、幅度 4）。
//   * B 组：带 libx265 的 ffmpeg 预生成的入库语料
//     （tests/corpus/codec/real_x265_bframes_gop.mp4），拿**编码器写进容器的
//     PTS** 当参考输出，对账"按 POC 排出的显示顺序 == 按 PTS 排出的显示顺序"。
//
// 明确不在本测试范围内的能力（代码里也不实现，见 HevcPocTracker.h 文件头）：
//   * 参考帧列表 / DPB / RPS —— 不重建，因此没有对应断言；
//   * temporal sub-layer 的 prevTid0Pic 细节 —— 逐幅图像累进；
//   * AV1 / VP9 / VVC —— 由 FrameOrderTracker 明确报"未实现"，见最后一条用例。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/FrameOrderTracker.h"
#include "core/analysis/codec/HevcPocTracker.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/FrameOrderInfo.h"
#include "tests/support/FrameOrderTestSupport.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::FrameOrderTracker;
using videoeye::HevcPocTracker;
using videoeye::model::AnalysisResult;
using videoeye::model::FrameOrderEntry;
using videoeye::model::FrameOrderTimeline;

using videoeye_test::CodecSample;
using videoeye_test::DemuxedOrderFacts;
using videoeye_test::DemuxForDisplayOrder;
using videoeye_test::DisplayOrderPerGop;
using videoeye_test::RunFrameOrderAnalysis;
using videoeye_test::SkipIfMissing;

// HEVC NAL 单元类型（Table 7-1），只列本文件用到的。
constexpr uint8_t kNalTrailR = 1;
constexpr uint8_t kNalRaslR = 9;
constexpr uint8_t kNalBlaWLp = 16;
constexpr uint8_t kNalIdrWRadl = 19;
constexpr uint8_t kNalCra = 21;
constexpr uint8_t kNalSps = 33;
constexpr uint8_t kNalPps = 34;

// HEVC 的 slice_type 取值（**不是** H.264 那套）。
constexpr int kSliceB = 0;
constexpr int kSliceP = 1;
constexpr int kSliceI = 2;

// ---------------------------------------------------------------------------
// 位写入器（MSB-first），与 H.264 那份同构但独立：两边的位流布局不同，
// 共用一份会诱导"照抄字段顺序"，而这正是最容易出错的地方。
// ---------------------------------------------------------------------------
class BitWriter {
public:
    void WriteBit(bool value) {
        if (bit_count_ % 8 == 0)
            data_.push_back(0);
        if (value)
            data_.back() |= static_cast<uint8_t>(1u << (7 - (bit_count_ % 8)));
        ++bit_count_;
    }

    void WriteBits(uint64_t value, int count) {
        for (int bit = count - 1; bit >= 0; --bit)
            WriteBit(((value >> bit) & 1u) != 0);
    }

    void WriteUE(uint32_t value) {
        const uint64_t code_num = static_cast<uint64_t>(value) + 1;
        int leading_zero_bits = 0;
        for (uint64_t n = code_num; n > 1; n >>= 1)
            ++leading_zero_bits;
        for (int i = 0; i < leading_zero_bits; ++i)
            WriteBit(false);
        WriteBits(code_num, leading_zero_bits + 1);
    }

    std::vector<uint8_t> FinishRbsp() {
        WriteBit(true); // rbsp_stop_one_bit
        while (bit_count_ % 8 != 0)
            WriteBit(false);
        return data_;
    }

private:
    std::vector<uint8_t> data_;
    int bit_count_ = 0;
};

std::vector<uint8_t> EscapeRbsp(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> ebsp;
    int consecutive_zeros = 0;
    for (uint8_t byte : rbsp) {
        if (consecutive_zeros >= 2 && byte <= 0x03) {
            ebsp.push_back(0x03);
            consecutive_zeros = 0;
        }
        ebsp.push_back(byte);
        consecutive_zeros = (byte == 0) ? consecutive_zeros + 1 : 0;
    }
    return ebsp;
}

// 一个完整的 HEVC NAL：4 字节起始码 + 2 字节 header + RBSP(已转义)。
std::vector<uint8_t> MakeNal(uint8_t type, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out = {0x00, 0x00, 0x00, 0x01};
    out.push_back(static_cast<uint8_t>(type << 1)); // forbidden(1) | type(6) | layer 高 1 位
    out.push_back(0x01);                            // layer 低 5 位 | temporal_id_plus1 = 1
    const std::vector<uint8_t> ebsp = EscapeRbsp(rbsp);
    out.insert(out.end(), ebsp.begin(), ebsp.end());
    return out;
}

std::vector<uint8_t> Concat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// 最小可用 SPS：1 个 CTU 尺寸的 192x108、4:2:0、8bit、无 VUI。
// 只有 log2_max_pic_order_cnt_lsb_minus4 与 sps_id 是可变的 —— 其余字段
// 只要能让 HevcBitstreamParser 解析出 present=true 就够了。
std::vector<uint8_t> BuildSps(int log2_poc_lsb_minus4 = 4, uint32_t sps_id = 0) {
    BitWriter w;
    w.WriteBits(0, 4); // sps_video_parameter_set_id
    w.WriteBits(0, 3); // sps_max_sub_layers_minus1 = 0
    w.WriteBit(true);  // sps_temporal_id_nesting_flag

    // profile_tier_level(1, 0)：Main / Level 4.0，96 位固定长度
    w.WriteBits(0, 2);   // general_profile_space
    w.WriteBit(false);   // general_tier_flag
    w.WriteBits(1, 5);   // general_profile_idc = Main
    w.WriteBits(0, 32);  // general_profile_compatibility_flag
    w.WriteBit(true);    // general_progressive_source_flag
    w.WriteBit(false);   // general_interlaced_source_flag
    w.WriteBit(false);   // general_non_packed_constraint_flag
    w.WriteBit(true);    // general_frame_only_constraint_flag
    w.WriteBits(0, 43);  // general_reserved_zero_43bits
    w.WriteBit(false);   // general_inbld_flag
    w.WriteBits(120, 8); // general_level_idc = 4.0

    w.WriteUE(sps_id); // sps_seq_parameter_set_id
    w.WriteUE(1);      // chroma_format_idc = 4:2:0
    // ❗ 4:2:0 没有 separate_colour_plane_flag；只有 chroma_format_idc == 3 才有。
    w.WriteUE(192);    // pic_width_in_luma_samples
    w.WriteUE(108);    // pic_height_in_luma_samples
    w.WriteBit(false); // conformance_window_flag
    w.WriteUE(0);      // bit_depth_luma_minus8
    w.WriteUE(0);      // bit_depth_chroma_minus8
    w.WriteUE(static_cast<uint32_t>(log2_poc_lsb_minus4));
    w.WriteBit(false); // sps_sub_layer_ordering_info_present_flag
    w.WriteUE(0);      // sps_max_dec_pic_buffering_minus1[0]
    w.WriteUE(0);      // sps_max_num_reorder_pics[0]
    w.WriteUE(0);      // sps_max_latency_increase_plus1[0]
    w.WriteUE(0);      // log2_min_luma_coding_block_size_minus3
    w.WriteUE(3);      // log2_diff_max_min_luma_coding_block_size
    w.WriteUE(0);      // log2_min_luma_transform_block_size_minus2
    w.WriteUE(3);      // log2_diff_max_min_luma_transform_block_size
    w.WriteUE(0);      // max_transform_hierarchy_depth_inter
    w.WriteUE(0);      // max_transform_hierarchy_depth_intra
    w.WriteBit(false); // scaling_list_enable_flag
    w.WriteBit(true);  // amp_enabled_flag
    w.WriteBit(true);  // sample_adaptive_offset_enabled_flag
    w.WriteBit(false); // pcm_enabled_flag
    w.WriteUE(0);      // num_short_term_ref_pic_sets
    w.WriteBit(false); // long_term_ref_pics_present_flag
    w.WriteBit(true);  // sps_temporal_mvp_enabled_flag
    w.WriteBit(true);  // strong_intra_smoothing_enabled_flag
    w.WriteBit(false); // vui_parameters_present_flag
    return w.FinishRbsp();
}

// 最小可用 PPS。num_extra_slice_header_bits / output_flag_present_flag 会改变
// slice_segment_header 的位布局，所以它们必须是可调的 —— 这两个字段读错位，
// 后面的 slice_type 与 poc_lsb 整体平移，症状是"POC 全都差一点点"。
std::vector<uint8_t> BuildPps(uint32_t pps_id = 0, uint32_t sps_id = 0, int extra_slice_header_bits = 0,
                              bool output_flag_present = false) {
    BitWriter w;
    w.WriteUE(pps_id); // pps_pic_parameter_set_id
    w.WriteUE(sps_id); // pps_seq_parameter_set_id
    w.WriteBit(false); // dependent_slice_segments_enabled_flag
    w.WriteBit(output_flag_present);
    w.WriteBits(static_cast<uint64_t>(extra_slice_header_bits), 3);
    w.WriteBit(false); // sign_data_hiding_enabled_flag
    w.WriteBit(false); // cabac_init_present_flag
    w.WriteUE(0);      // num_ref_idx_l0_default_active_minus1
    w.WriteUE(0);      // num_ref_idx_l1_default_active_minus1
    w.WriteUE(0);      // init_qp_minus26
    w.WriteBit(false); // constrained_intra_pred_flag
    w.WriteBit(false); // transform_skip_enabled_flag
    w.WriteBit(false); // cu_qp_delta_enabled_flag
    w.WriteUE(0);      // pps_cb_qp_offset
    w.WriteUE(0);      // pps_cr_qp_offset
    w.WriteBit(false); // pps_slice_chroma_qp_offsets_present_flag
    w.WriteBit(false); // weighted_pred_flag
    w.WriteBit(false); // weighted_bipred_flag
    w.WriteBit(false); // transquant_bypass_enabled_flag
    w.WriteBit(false); // tiles_enabled_flag
    w.WriteBit(false); // entropy_coding_sync_enabled_flag
    w.WriteBit(true);  // pps_loop_filter_across_slices_enabled_flag
    w.WriteBit(false); // deblocking_filter_control_present_flag
    w.WriteBit(false); // pps_scaling_list_data_present_flag
    w.WriteBit(false); // lists_modification_present_flag
    w.WriteUE(0);      // log2_parallel_merge_level_minus2
    w.WriteBit(false); // slice_segment_header_extension_present_flag
    w.WriteBit(false); // pps_extension_present_flag
    return w.FinishRbsp();
}

// 一幅图像的 slice_segment_header 前缀。真正的码流后面还有很多字段，但
// 追踪器只读到 slice_pic_order_cnt_lsb 为止，所以这里到那儿就收尾。
struct SliceSpec {
    uint8_t nal_type = kNalTrailR;
    int slice_type = kSliceP; // HEVC 编码：0=B 1=P 2=I
    int poc_lsb = 0;
    bool first_segment = true;
    uint32_t pps_id = 0;
    int extra_slice_header_bits = 0;
    bool output_flag_present = false;
    // 追加几个非零位模拟 short_term_ref_pic_set。IDR 用它来验"没有 poc_lsb 字段"
    // —— 若实现误读，读到的就是这几位。
    bool trailing_bits = true;
};

std::vector<uint8_t> BuildSlice(const SliceSpec& spec, int log2_poc_lsb_minus4) {
    BitWriter w;
    w.WriteBit(spec.first_segment);
    if (spec.nal_type >= 16 && spec.nal_type <= 23)
        w.WriteBit(false); // no_output_of_prior_pics_flag
    w.WriteUE(spec.pps_id);
    // 非首段还有 dependent_slice_segment_flag / slice_segment_address，
    // 但追踪器在首段标志处就返回了，这里不必构造。
    if (spec.first_segment) {
        for (int i = 0; i < spec.extra_slice_header_bits; ++i)
            w.WriteBit(false); // slice_reserved_flag[i]
        w.WriteUE(static_cast<uint32_t>(spec.slice_type));
        if (spec.output_flag_present)
            w.WriteBit(true); // pic_output_flag
        if (spec.nal_type != kNalIdrWRadl && spec.nal_type != 20) {
            w.WriteBits(static_cast<uint64_t>(spec.poc_lsb), log2_poc_lsb_minus4 + 4);
            if (spec.trailing_bits) {
                w.WriteBit(true); // short_term_ref_pic_set_sps_flag
                w.WriteBits(0x5, 3);
            }
        } else if (spec.trailing_bits) {
            // IDR 没有 poc_lsb：紧随其后就是 ref_pic 的字段，写下非零位，
            // 好让"误读 poc_lsb"这种错在断言 POC == 0 时暴露出来。
            w.WriteBit(true);
            w.WriteBits(0xD, 4);
        }
    }
    return MakeNal(spec.nal_type, w.FinishRbsp());
}

// ---------------------------------------------------------------------------
// 装置：喂参数集 + 逐幅图像，时间戳只用来对齐，不参与 POC 计算。
// ---------------------------------------------------------------------------
struct PocFixture {
    explicit PocFixture(int log2_poc_lsb_minus4 = 4) : log2_minus4_(log2_poc_lsb_minus4) {
        tracker_.Reset(0);
        tracker_.SetFraming(HevcPocTracker::Framing::AnnexB);
    }

    // 参数集走 extradata 通道（等价于 MP4 的 hvcC / MKV 的 CodecPrivate）
    void FeedParameterSets(int extra_slice_header_bits = 0, bool output_flag_present = false, uint32_t sps_id = 0) {
        const std::vector<uint8_t> extradata =
            Concat({MakeNal(kNalSps, BuildSps(log2_minus4_, sps_id)),
                    MakeNal(kNalPps, BuildPps(0, sps_id, extra_slice_header_bits, output_flag_present))});
        ASSERT_TRUE(tracker_.FeedExtradata(extradata.data(), extradata.size()))
            << "自造的 SPS/PPS 没能被 HevcBitstreamParser 认出来，后面的用例都会空跑";
    }

    // 参数集走带内通道（等价于 TS / 裸流的 VPS/SPS/PPS 重复下发）
    void FeedParameterSetsInBand() {
        const std::vector<uint8_t> packet =
            Concat({MakeNal(kNalSps, BuildSps(log2_minus4_)), MakeNal(kNalPps, BuildPps())});
        tracker_.Feed(packet.data(), packet.size(), false, 0.0, packet_index_++);
    }

    void FeedPicture(uint8_t nal_type, int slice_type, int poc_lsb, bool first_segment = true) {
        const SliceSpec spec{nal_type, slice_type, poc_lsb, first_segment, 0, 0, false, true};
        const std::vector<uint8_t> packet = BuildSlice(spec, log2_minus4_);
        const double time_seconds = static_cast<double>(poc_lsb) * (1.0 / 25.0);
        tracker_.Feed(packet.data(), packet.size(), true, time_seconds, packet_index_++);
    }

    void FeedPicture(const SliceSpec& spec) {
        const std::vector<uint8_t> packet = BuildSlice(spec, log2_minus4_);
        tracker_.Feed(packet.data(), packet.size(), true, 0.0, packet_index_++);
    }

    const FrameOrderTimeline& Finish() {
        tracker_.Finish();
        return tracker_.Finish(); // 幂等性顺手盯一下
    }

    HevcPocTracker& tracker() {
        return tracker_;
    }

private:
    HevcPocTracker tracker_;
    int log2_minus4_ = 4;
    int64_t packet_index_ = 0;
};

// ---------------------------------------------------------------------------
// 1) 正例：IBBP 的显示顺序重排
// ---------------------------------------------------------------------------
// 显示顺序 I0 B1 B2 P3（POC = 显示序号），
// 解码顺序 = I0(POC 0) → P3(POC 3) → B1(POC 1) → B2(POC 2)。
// 解码顺序里的 POC 序列是 0,3,1,2 —— 必然且只在 3→1 处回退一次，幅度 2。
TEST(HevcPocTrackerTest, IbbpGopReorderingIsDetectedWithDepth) {
    PocFixture f;
    f.FeedParameterSets();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0);
    f.FeedPicture(kNalTrailR, kSliceP, 3);
    f.FeedPicture(kNalTrailR, kSliceB, 1);
    f.FeedPicture(kNalTrailR, kSliceB, 2);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 4u);
    EXPECT_EQ(t.frames[0].poc, 0);
    EXPECT_EQ(t.frames[1].poc, 3);
    EXPECT_EQ(t.frames[2].poc, 1);
    EXPECT_EQ(t.frames[3].poc, 2);

    EXPECT_EQ(t.i_count, 1);
    EXPECT_EQ(t.p_count, 1);
    EXPECT_EQ(t.b_count, 2);
    EXPECT_EQ(t.idr_count, 1);
    EXPECT_EQ(t.reorder_events, 1) << "只有 3→1 这一处 POC 回退";
    EXPECT_EQ(t.reorder_depth, 2);
    EXPECT_EQ(t.duplicate_poc_count, 0);
    EXPECT_EQ(t.poc_gap_slots, 0);
    EXPECT_EQ(t.poc_type, 0) << "HEVC 只有一套 POC 公式，对外统一记 0";
    EXPECT_FALSE(t.field_coded) << "HEVC 只有帧编码，永远不置 field_coded";
}

// ---------------------------------------------------------------------------
// 2) slice_type 的取值顺序与 H.264 相反
// ---------------------------------------------------------------------------
// HEVC：0=B / 1=P / 2=I。把 0 当成 P 会让 B/P 计数互换 —— 数量看着还挺合理，
// 只有拿 POC 回退去对账才会发现不对。这一条直接钉住映射本身。
TEST(HevcPocTrackerTest, SliceTypeMappingFollowsHevcOrder) {
    PocFixture f;
    f.FeedParameterSets();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0);
    f.FeedPicture(kNalTrailR, kSliceP, 2);
    f.FeedPicture(kNalTrailR, kSliceB, 1);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 3u);
    EXPECT_EQ(t.frames[0].TypeChar(), 'I');
    EXPECT_EQ(t.frames[1].TypeChar(), 'P');
    EXPECT_EQ(t.frames[2].TypeChar(), 'B');
    EXPECT_EQ(t.i_count, 1);
    EXPECT_EQ(t.p_count, 1);
    EXPECT_EQ(t.b_count, 1);
}

// ---------------------------------------------------------------------------
// 3) IDR 的 slice_segment_header 里没有 slice_pic_order_cnt_lsb
// ---------------------------------------------------------------------------
// 规范：该字段只在 nal_unit_type != IDR_W_RADL(19) && != IDR_N_LP(20) 时存在。
// 误读的后果不是"IDR 的 POC 略偏"，而是把后面 ref_pic 的位当成 lsb，
// 于是 IDR 的 POC 变成一个非零乱数 —— 而 IDR 的 POC 按规范必须是 0
// （复位后 prev 全零、且没有 lsb 可加）。后面紧跟的 P 也因此必须精确。
TEST(HevcPocTrackerTest, IdrDoesNotReadPocLsbAndResetsToZero) {
    PocFixture f;
    f.FeedParameterSets();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0); // 若误读，会从 trailing bits 得到非 0
    f.FeedPicture(kNalTrailR, kSliceP, 7);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 2u);
    EXPECT_EQ(t.frames[0].poc, 0) << "IDR 的 POC 必须是 0（没有 lsb 字段，不许去位流里取）";
    EXPECT_TRUE(t.frames[0].idr);
    EXPECT_EQ(t.frames[1].poc, 7) << "复位后的第一帧从自己的 lsb 起算";
    EXPECT_EQ(t.reorder_events, 0);
}

// ---------------------------------------------------------------------------
// 4) BLA 复位、中间的 CRA 不复位
// ---------------------------------------------------------------------------
// NoRaslOutputFlag 为 1 的只有 IDR / BLA，以及"码流第一幅图"上的 CRA。
// 插在码流中间的 CRA 是随机接入点但**不**复位 —— 漏了这条，一份 open-GOP
// 文件会在每个 CRA 处凭空把 POC 清零，紧接着的 RASL 就变成一次假的 POC 回退。
TEST(HevcPocTrackerTest, BlaResetsPocButMidStreamCraDoesNot) {
    PocFixture f;
    f.FeedParameterSets();
    f.FeedPicture(kNalBlaWLp, kSliceI, 0); // BLA：复位
    f.FeedPicture(kNalTrailR, kSliceP, 5);
    f.FeedPicture(kNalCra, kSliceI, 9); // CRA 在中间：**不**复位
    f.FeedPicture(kNalTrailR, kSliceP, 13);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 4u);
    EXPECT_TRUE(t.frames[0].idr) << "BLA 的 NoRaslOutputFlag 恒为 1";
    EXPECT_TRUE(t.frames[0].irap);
    EXPECT_EQ(t.frames[0].poc, 0);

    EXPECT_FALSE(t.frames[2].idr) << "中间的 CRA 不复位 POC";
    EXPECT_TRUE(t.frames[2].irap) << "但它仍然是随机接入点";
    EXPECT_EQ(t.frames[2].poc, 9) << "没有复位，POC 必须接着往下算";
    EXPECT_EQ(t.frames[3].poc, 13);
    EXPECT_EQ(t.reorder_events, 0);
}

// 码流（本例即扫描起点）第一幅图若是 CRA，NoRaslOutputFlag 必为 1：
// 状态复位，但因为 CRA 仍有 slice_pic_order_cnt_lsb，POC = 自己的 lsb，不是 0。
// 这两个结论一起看才能区分"复位"与"强制清零"。
TEST(HevcPocTrackerTest, FirstCraResetsStateButKeepsItsOwnLsb) {
    PocFixture f;
    f.FeedParameterSets();
    f.FeedPicture(kNalCra, kSliceI, 9);
    f.FeedPicture(kNalTrailR, kSliceP, 12);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 2u);
    EXPECT_TRUE(t.frames[0].idr) << "起点上的 CRA 按 NoRaslOutputFlag == 1 处理";
    EXPECT_EQ(t.frames[0].poc, 9) << "CRA 有 lsb 字段，POC 是 9 而不是 0（与 IDR 不同）";
    EXPECT_EQ(t.frames[1].poc, 12);
}

// ---------------------------------------------------------------------------
// 5) CRA 之后的 RASL 不算重排
// ---------------------------------------------------------------------------
// 开放 GOP 的解码顺序：… P(8) → CRA(12) → RASL(10) → P(16) → B(14) …
// CRA(12) → RASL(10) 是一处 POC 回退，但那是 leading picture 的定义，
// 不是 B 帧重排。只有 P(16) → B(14) 才是。
//
// 判据必须看"前一帧是不是 IRAP"而不是"本帧是不是 idr"：RASL 自己既不是
// idr 也不是 irap，只看它就躲不开。漏掉这条会得到 3 次重排 ——
// 数字看着合理，但没有一次对应用户能理解的现象。
TEST(HevcPocTrackerTest, RaslAfterCraIsNotCountedAsReordering) {
    PocFixture f;
    f.FeedParameterSets();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0);
    f.FeedPicture(kNalTrailR, kSliceP, 8);
    f.FeedPicture(kNalTrailR, kSliceB, 4);
    f.FeedPicture(kNalTrailR, kSliceB, 6);
    f.FeedPicture(kNalCra, kSliceI, 12);
    f.FeedPicture(kNalRaslR, kSliceB, 10); // RASL：POC 比 CRA 小，但不算重排
    f.FeedPicture(kNalTrailR, kSliceP, 16);
    f.FeedPicture(kNalTrailR, kSliceB, 14);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 8u);
    // 8→4 与 16→14 是真正的 B 帧重排；12→10 是 RASL 的固有语义。
    EXPECT_EQ(t.reorder_events, 2) << "IRAP 之后的 POC 回退不算重排";
    EXPECT_EQ(t.reorder_depth, 4) << "最大回退仍来自 8→4";
}

// ---------------------------------------------------------------------------
// 6) POC 的 lsb 回绕不能看起来像重排
// ---------------------------------------------------------------------------
// log2_max_pic_order_cnt_lsb_minus4 = 0 → MaxPicOrderCntLsb = 16。
// 连续帧的 lsb 走 12,14,0,2 → POC 必须走 12,14,16,18（进位），
// 而不是 12,14,0,2（那样会报出两次假的"POC 回退"）。
//
// ❗ 步长必须是 2：4 位的 lsb 一步跨过半周期（例如 0 → 12）在规范里就是
// **歧义**的，8.3.1 会按"倒退 4 帧"解释。真实编码器每帧只走 1~2 个 POC 槽，
// 用大步长构造样本只会测到规范的歧义分支，测不到回绕本身。
TEST(HevcPocTrackerTest, PocLsbWrapDoesNotLookLikeReordering) {
    PocFixture f(/*log2_poc_lsb_minus4=*/0);
    f.FeedParameterSets();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0);
    const int lsbs[] = {2, 4, 6, 8, 10, 12, 14, 0, 2, 4};
    for (int lsb : lsbs)
        f.FeedPicture(kNalTrailR, kSliceP, lsb);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 11u);
    for (std::size_t i = 0; i < t.frames.size(); ++i)
        EXPECT_EQ(t.frames[i].poc, static_cast<int64_t>(i) * 2) << "第 " << i << " 帧";
    EXPECT_EQ(t.frames[8].poc, 16) << "lsb 从 14 跳到 0 是进位，不是回退";
    EXPECT_EQ(t.frames[9].poc, 18);
    EXPECT_EQ(t.reorder_events, 0);
    EXPECT_EQ(t.poc_gap_slots, 0);
}

// 半个周期边界：prev 的 lsb 是 6、新 lsb 是 14，差正好 MaxPicOrderCntLsb / 2 = 8。
// 规范里前进方向判据是 **严格大于**（`> Max/2` 才借位），所以这里不借位、
// POC 直接是 14。把不等号抄成 >=，这一帧就会掉到 -2，凭空多出一次"重排"。
TEST(HevcPocTrackerTest, PocLsbHalfPeriodJumpDoesNotBorrow) {
    PocFixture f(/*log2_poc_lsb_minus4=*/0);
    f.FeedParameterSets();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0);
    f.FeedPicture(kNalTrailR, kSliceP, 2); // POC 2
    f.FeedPicture(kNalTrailR, kSliceP, 6); // POC 6
    f.FeedPicture(kNalTrailR, kSliceB, 14);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 4u);
    EXPECT_EQ(t.frames[1].poc, 2);
    EXPECT_EQ(t.frames[2].poc, 6);
    EXPECT_EQ(t.frames[3].poc, 14) << "差正好半个周期时按规范取『不借位』";
}

// ---------------------------------------------------------------------------
// 7) 多 slice 的图像只记一帧
// ---------------------------------------------------------------------------
// first_slice_segment_in_pic_flag == 0 的 slice（含 dependent slice segment）
// 属于同一幅图像，不推进 POC 状态。按 slice 计数会把 I/P/B 与帧数全部放大，
// 而"帧数 = 包数"在 MP4 上又刚好成立（一个包一幅图像），很难被发现。
TEST(HevcPocTrackerTest, MultiSlicePictureIsCountedOnce) {
    PocFixture f;
    f.FeedParameterSets();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0);
    // 同一幅图像的后续两段
    f.FeedPicture(kNalTrailR, kSliceP, 0, /*first_segment=*/false);
    f.FeedPicture(kNalTrailR, kSliceP, 0, /*first_segment=*/false);
    f.FeedPicture(kNalTrailR, kSliceP, 4);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 2) << "三段落里只有一段是图像首段";
    ASSERT_EQ(t.frames.size(), 2u);
    EXPECT_EQ(t.frames[1].poc, 4);
}

// ---------------------------------------------------------------------------
// 8) 被截断的 slice 不能凭空造出一帧
// ---------------------------------------------------------------------------
// 位读完之前就耗尽的包，读出来的字段是补的 0。把它当一帧存下去会凭空多出
// 一幅"POC = 0 的 P 帧"，而且它还会把 POC 状态推乱，连累后面的帧。
TEST(HevcPocTrackerTest, TruncatedSliceIsIgnored) {
    PocFixture f;
    f.FeedParameterSets();

    // 只有 2 字节 payload：够读 first_slice_segment / pps_id，不够读 slice_type
    const std::vector<uint8_t> truncated = {0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x80, 0x00};
    f.tracker().Feed(truncated.data(), truncated.size(), false, 0.0, 0);

    f.FeedPicture(kNalTrailR, kSliceP, 6);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 1) << "被截断的那一包不能算一帧";
    ASSERT_EQ(t.frames.size(), 1u);
    EXPECT_EQ(t.frames[0].poc, 6);
}

// ---------------------------------------------------------------------------
// 9) 参数集缺失要给出原因，而不是"0 帧、无重排"
// ---------------------------------------------------------------------------
TEST(HevcPocTrackerTest, MissingParameterSetsReportReason) {
    HevcPocTracker tracker;
    tracker.Reset(0);
    tracker.SetFraming(HevcPocTracker::Framing::AnnexB);
    const std::vector<uint8_t> packet = BuildSlice(SliceSpec{kNalIdrWRadl, kSliceI, 0, true}, 4);
    tracker.Feed(packet.data(), packet.size(), false, 0.0, 0);

    const FrameOrderTimeline& t = tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_NE(t.unavailable_reason.find("PPS"), std::string::npos)
        << "原因里必须出现卡在哪一步，实际是：" << t.unavailable_reason;
}

// 带内下发参数集（TS / 裸流的形态）与 extradata 通道必须等价
TEST(HevcPocTrackerTest, InBandParameterSetsEnablePoc) {
    PocFixture f;
    f.FeedParameterSetsInBand();
    f.FeedPicture(kNalIdrWRadl, kSliceI, 0);
    f.FeedPicture(kNalTrailR, kSliceP, 3);
    f.FeedPicture(kNalTrailR, kSliceB, 1);
    f.FeedPicture(kNalTrailR, kSliceB, 2);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 4);
    EXPECT_EQ(t.reorder_events, 1);
    EXPECT_EQ(t.reorder_depth, 2);
}

// ---------------------------------------------------------------------------
// 10) PPS 里那两个"改变位布局"的开关必须被正确消费
// ---------------------------------------------------------------------------
// num_extra_slice_header_bits > 0 时 slice_header 里会多出若干保留位；
// output_flag_present_flag 会多出 pic_output_flag。任何一个少跳一位，
// 后面的 slice_type 与 poc_lsb 就整体平移 —— 症状是"POC 全都差一点点"，
// 而这在只有一位之差时甚至不会报错，只会给出错误但自洽的时间线。
TEST(HevcPocTrackerTest, PpsHeaderLayoutSwitchesAreConsumed) {
    PocFixture f;
    f.FeedParameterSets(/*extra_slice_header_bits=*/2, /*output_flag_present=*/true);
    SliceSpec idr{kNalIdrWRadl, kSliceI, 0, true};
    idr.extra_slice_header_bits = 2;
    idr.output_flag_present = true;
    f.FeedPicture(idr);
    SliceSpec p{kNalTrailR, kSliceP, 5, true};
    p.extra_slice_header_bits = 2;
    p.output_flag_present = true;
    f.FeedPicture(p);
    SliceSpec b{kNalTrailR, kSliceB, 3, true};
    b.extra_slice_header_bits = 2;
    b.output_flag_present = true;
    f.FeedPicture(b);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 3u);
    EXPECT_EQ(t.frames[1].poc, 5);
    EXPECT_EQ(t.frames[2].poc, 3);
    EXPECT_EQ(t.reorder_events, 1);
}

// ---------------------------------------------------------------------------
// 11) 明细上限
// ---------------------------------------------------------------------------
// 上限只截断逐帧明细，不许影响统计量 —— 否则一个长文件报出的"共 N 帧"
// 会等于上限值，而那个数字既不是文件长度也不是分析范围。
TEST(HevcPocTrackerTest, MaxFramesTruncatesDetailButNotStatistics) {
    HevcPocTracker tracker;
    HevcPocTracker::Options options;
    options.max_frames = 4;
    tracker.Reset(0, options);
    tracker.SetFraming(HevcPocTracker::Framing::AnnexB);

    const std::vector<uint8_t> extradata = Concat({MakeNal(kNalSps, BuildSps(4)), MakeNal(kNalPps, BuildPps())});
    ASSERT_TRUE(tracker.FeedExtradata(extradata.data(), extradata.size()));

    for (int i = 0; i < 8; ++i) {
        const std::vector<uint8_t> packet = BuildSlice(SliceSpec{kNalTrailR, kSliceP, i, true}, 4);
        tracker.Feed(packet.data(), packet.size(), false, 0.0, i);
    }

    const FrameOrderTimeline& t = tracker.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 8) << "统计必须覆盖全部帧";
    EXPECT_EQ(static_cast<int>(t.frames.size()), 4) << "明细被上限截断";
    EXPECT_TRUE(t.truncated);
    EXPECT_EQ(t.p_count, 8);
}

// ---------------------------------------------------------------------------
// 12) 外观层按编码分派
// ---------------------------------------------------------------------------
TEST(FrameOrderTrackerTest, DispatchesByCodecId) {
    FrameOrderTracker tracker;

    tracker.Reset(AV_CODEC_ID_H264, 0);
    EXPECT_TRUE(tracker.enabled());
    EXPECT_STREQ(tracker.codec_name(), "h264");

    tracker.Reset(AV_CODEC_ID_HEVC, 1);
    EXPECT_TRUE(tracker.enabled());
    EXPECT_STREQ(tracker.codec_name(), "hevc");
    EXPECT_EQ(tracker.stream_index(), 1);

    // VVC 已落地（POC 在 picture header 里，见 test_vvc_frame_order_tracker.cpp）
    tracker.Reset(AV_CODEC_ID_VVC, 2);
    EXPECT_TRUE(tracker.enabled());
    EXPECT_STREQ(tracker.codec_name(), "vvc");
    EXPECT_STREQ(tracker.order_source_name(), "poc");
}

// 没有实现帧级顺序追踪的编码必须明确说"没实现"，
// 不能给一条"0 帧、无重排"的假结论 —— 那种输出会让用户以为文件真的没有 B 帧。
TEST(FrameOrderTrackerTest, UnsupportedCodecIsExplicitlyDisabled) {
    FrameOrderTracker tracker;
    // ❗ 反例要挑一个**确实没实现**的编码。这里用 MPEG-2（面板支持它作为容器
    // 里的常见编码，但帧级顺序追踪没做）；H.264/HEVC/VVC/VP9/AV1 都已实现，
    // 别再拿它们当反例。
    tracker.Reset(AV_CODEC_ID_MPEG2VIDEO, 0);
    EXPECT_FALSE(tracker.enabled());
    EXPECT_FALSE(tracker.disabled_reason().empty());

    const FrameOrderTimeline& t = tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_FALSE(t.unavailable_reason.empty());
}

// ---------------------------------------------------------------------------
// B 组：真实 x265 样本
// ---------------------------------------------------------------------------
// 合成样本能证明"实现与规范一致"，证明不了"规范理解与真实编码器一致"。
// 这一组用真实码流交叉验证，参考输出全部来自 FFmpeg（容器 PTS / 同步样本表），
// 与被测代码零共享 —— 两侧唯一的共同输入是同一份文件。
//
// 语料：tests/corpus/codec/real_x265_bframes_gop.mp4
//   libx265 / 192x108 / 25fps / 40 帧 / keyint 12 / bframes 3 / b-adapt 0
//   repeat-headers=0 → 参数集**只**在 hvcC 里（与真实 MP4 一致，
//   顺带把"extradata 有没有喂给追踪器"这条静默失效路径一起测了）
TEST(HevcPocTrackerRealEncoderTest, PocOrderMatchesEncoderDisplayOrder) {
    const std::string path = CodecSample("real_x265_bframes_gop.mp4");
    SkipIfMissing(path, "real_x265_bframes_gop.mp4");

    const DemuxedOrderFacts facts = DemuxForDisplayOrder(path);
    ASSERT_TRUE(facts.ok) << "容器里取不到足够的时间戳，无法对账";

    const AnalysisResult result = RunFrameOrderAnalysis(path);
    ASSERT_EQ(result.scan_status, videoeye::model::AnalysisStatus::Complete) << result.error_message;
    ASSERT_EQ(result.frame_order_timelines.size(), 1u);
    const FrameOrderTimeline& t = result.frame_order_timelines[0];
    ASSERT_TRUE(t.available) << t.unavailable_reason;

    ASSERT_EQ(t.frames.size(), facts.pts_in_decode_order.size()) << "逐 slice 数出来的帧数必须与容器里的视频包数一致";

    // 样本自检：没有 B 帧的话下面整段对账是恒真的空跑。
    ASSERT_GT(t.b_count, 0) << "语料里没有 B 帧，无法验证显示顺序重排";
    ASSERT_GT(t.reorder_events, 0) << "有 B 帧就必然存在解码顺序里的 POC 回退";
    EXPECT_EQ(t.poc_type, 0);
    EXPECT_FALSE(t.field_coded);

    // 交叉对账一：GOP 段数 == 容器标记的关键帧数（两条完全独立的通道）。
    // 这份样本是**开放 GOP**（x265 默认写 CRA + RASL），正好把"GOP 段数必须
    // 按随机接入点分段、而不是按 POC 复位点分段"这条判据放到真实码流上验：
    // 只有第一幅是 IDR，后面三个是 CRA，POC 一路递增。
    const std::size_t container_keys = static_cast<std::size_t>(
        std::count(facts.is_key_in_decode_order.begin(), facts.is_key_in_decode_order.end(), true));
    ASSERT_GT(container_keys, 0u) << "容器没标任何关键帧，无法分段对账";
    EXPECT_EQ(static_cast<std::size_t>(t.irap_count), container_keys)
        << "逐 slice 数出的随机接入点数与容器同步样本表不一致";
    EXPECT_EQ(static_cast<std::size_t>(t.gop_count), container_keys)
        << "逐 slice 解出的 GOP 段数与容器同步样本表不一致";
    EXPECT_GE(t.idr_count, 1) << "至少第一幅应当是 IDR";
    EXPECT_LE(t.idr_count, t.irap_count);

    // 交叉对账二：按 POC 排出的显示顺序 == 按容器 PTS 排出的显示顺序
    const std::vector<std::vector<int64_t>> ours = DisplayOrderPerGop(t);
    const std::vector<std::vector<int64_t>> reference = facts.DecodeIndexInDisplayOrderPerGop();
    ASSERT_EQ(ours.size(), reference.size());
    for (std::size_t gop = 0; gop < ours.size(); ++gop) {
        EXPECT_EQ(ours[gop], reference[gop]) << "第 " << gop << " 段的显示顺序与容器 PTS 给出的不一致";
    }
}

// x265 在闭 GOP 下每帧的 POC 步长恒为 1，且不跳号。
// "跳号"这类错误（把一幅图像算成两帧、或漏解析某个 slice）会让 poc_gap_slots
// 变成正数，而 POC 排序本身仍然可能看起来是对的 —— 必须单独断言。
TEST(HevcPocTrackerRealEncoderTest, PocSequenceIsContiguousWithinGop) {
    const std::string path = CodecSample("real_x265_bframes_gop.mp4");
    SkipIfMissing(path, "real_x265_bframes_gop.mp4");

    const AnalysisResult result = RunFrameOrderAnalysis(path);
    ASSERT_EQ(result.scan_status, videoeye::model::AnalysisStatus::Complete) << result.error_message;
    ASSERT_EQ(result.frame_order_timelines.size(), 1u);
    const FrameOrderTimeline& t = result.frame_order_timelines[0];
    ASSERT_TRUE(t.available) << t.unavailable_reason;

    EXPECT_EQ(t.duplicate_poc_count, 0) << "同一 GOP 内不该出现两个相同 POC";
    EXPECT_EQ(t.poc_gap_slots, 0) << "POC 不该跳号";
    EXPECT_EQ(t.frames_observed, 40);
    // 这份样本是开放 GOP：keyint=12，四段按解码顺序分别是 9 / 12 / 12 / 7 帧
    // （首段 = IDR + 8 帧；中间两段 = CRA + 3 个 RASL + 8 帧；末段被 40 帧的总长截断）。
    // 这些数字与生成脚本里的 keyint / 总帧数对得上，不是照着实现的输出抄的。
    EXPECT_EQ(t.gop_count, 4);
    EXPECT_EQ(t.max_gop_frames, 12) << "keyint=12，任何一段都不会超过它";
    EXPECT_EQ(t.min_gop_frames, 7) << "末段被总帧数截断";
}

} // namespace
