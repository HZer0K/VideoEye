// H.266 (VVC) 帧级 POC 追踪的回归测试。
//
// 被测的是 VvcPocTracker：与 H.264/HEVC 同一个问题（容器只给解码顺序），
// 但"POC 写在哪"完全不同 —— VVC 的 POC 在 picture_header（PH，NAL 19）里，
// 不是 slice_header。所以期望值全部按 H.266 7.3.2.8（PH）、7.3.2.9（slice）、
// 8.3.1（POC）、8.1.3（NoRaslOutputFlag）独立给出，**不**照抄实现的输出。
//
// 本文件逐个钉住的坑：
//   1. PH 开头两位（ph_gdr_or_irap_pic_flag / ph_non_ref_pic_flag）无条件存在，
//      按 HEVC 的直觉只读一位，后面整体错位。
//   2. ph_pic_parameter_set_id 是 ue(v)，不是定长。
//   3. ph_pic_order_cnt_lsb 对 IDR 也存在（规范要求写 0）—— 与 HEVC 相反。
//   4. 中间插入的 CRA 不复位 POC（只有首幅/EOS 之后才复位）。
//   5. 前导图像（RADL=2 / RASL=3）的 POC 回退不是 B 帧重排。VVC 的 7/8/9 是
//      IDR/CRA，沿用 HEVC 的 "6..9" 判据会把重排统计整段吞掉。
//   6. 同一幅图像的多个 slice 只记一帧。
//
// 数据来源：SPS 用 tests/unit/test_vvc_bitstream_parser.cpp 里那份由
// _smoke/gen_vvc_ps.py 按 7.3.2.3 逐位生成的 1080p10bit SPS（log2_max_poc_lsb=8）；
// PPS / PH / slice 由本文件的 BitWriter 按规范表现拼，与实现的分支结构无关。
//
// 明确不在本测试范围内（代码里也不实现，见 VvcPocTracker.h 文件头）：
//   * POC MSB cycle：遇到即拒绝，不给"看起来对"的时间线；
//   * 多 tile / 显式 slice 布局下的 slice_type：只给 POC，类型记未知；
//   * 参考帧列表 / DPB / HRD。

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/FrameOrderTracker.h"
#include "core/analysis/codec/VvcPocTracker.h"
#include "core/domain/model/FrameOrderInfo.h"
#include "core/media/codec/ExtradataParser.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::ExtradataFormat;
using videoeye::ExtradataParser;
using videoeye::ExtradataResult;
using videoeye::FrameOrderTracker;
using videoeye::VvcPocTracker;
using videoeye::model::FrameOrderEntry;
using videoeye::model::FrameOrderTimeline;

// ---- 与 test_vvc_bitstream_parser.cpp 同一份生成器产物（1080p / 10bit / 4:2:0）
// 关键字段：log2_max_pic_order_cnt_lsb_minus4 = 4（即 POC lsb 8 位）、
//           sps_poc_msb_cycle_flag = 0、无 subpic、无 extra_ph/sh bits。
const std::vector<uint8_t> kSps1080p10Bit = {
    0x01, 0x0B, 0x02, 0x33, 0x80, 0x00, 0x00, 0x0F, 0x02, 0x00, 0x43, 0x91, 0x88, 0x04, 0xBA, 0xD5, 0x7B,
    0xFF, 0xFC, 0x6F, 0x6B, 0xFB, 0xFF, 0xB9, 0x89, 0x80, 0x88, 0x05, 0x09, 0x10, 0x09, 0x00, 0x40,
};

// 另一份 PPS：pps_no_pic_partition_flag = 0（图像被 tile 划分过）。
// 用途是钉住"地址宽度不可证为 0 时不猜 slice_type"这条边界。
const std::vector<uint8_t> kPpsPartitioned = {
    0x00, 0x00, 0x07, 0x81, 0x00, 0x21, 0xC8, 0x1F, 0x24, 0x12, 0x60, 0x20,
};

// 极简 MSB-first 位写入器（与 test_vvc_bitstream_parser.cpp 的同名工具同形）。
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

// PPS：no_pic_partition_flag = 1（整幅图像一个 tile / 一个 slice），
// 后面按 7.3.2.4 把解析器会读的 QP 段补齐（否则 PPS 走不到 present）。
std::vector<uint8_t> MakePpsNoPartition() {
    BitWriter w;
    w.Bits(0, 6); // pps_pic_parameter_set_id
    w.Bits(0, 4); // pps_seq_parameter_set_id
    w.Bit(0);     // pps_mixed_nalu_types_in_pic_flag
    w.Ue(1920);   // pps_pic_width_in_luma_samples
    w.Ue(1080);   // pps_pic_height_in_luma_samples
    w.Bit(0);     // pps_conformance_window_flag
    w.Bit(0);     // pps_scaling_window_explicit_signalling_flag
    w.Bit(0);     // pps_output_flag_present_flag
    w.Bit(1);     // pps_no_pic_partition_flag = 1
    w.Bit(0);     // pps_subpic_id_mapping_present_flag
    // ---- QP / 权重 / 去块段（顺序即规范顺序）----
    w.Bit(0); // pps_cabac_init_present_flag
    w.Ue(0);  // pps_num_ref_idx_default_active_minus1[0]
    w.Ue(0);  // pps_num_ref_idx_default_active_minus1[1]
    w.Bit(0); // pps_rpl1_idx_present_flag
    w.Bit(0); // pps_weighted_pred_flag
    w.Bit(0); // pps_weighted_bipred_flag
    w.Bit(0); // pps_ref_wraparound_enabled_flag
    w.Ue(8);  // pps_pic_init_qp_minus26 (se(v)：4 -> ue 8)
    w.Bit(0); // pps_cu_qp_delta_enabled_flag
    w.Bit(0); // pps_chroma_tool_offsets_present_flag
    w.Bit(0); // pps_deblocking_filter_control_present_flag
    w.Bit(0); // pps_picture_header_extension_present_flag
    w.Bit(0); // pps_slice_header_extension_present_flag
    w.Bit(0); // pps_extension_flag
    w.Bit(1); // rbsp trailing bits
    return w.Bytes();
}

// 一幅图像的 picture_header 字段（只写与 POC/类型有关的部分）。
struct PhSpec {
    int pps_id = 0;
    int poc_lsb = 0; // 8 位（由 kSps 的 log2_max_poc_lsb 决定）
    bool gdr_or_irap = false;
    bool non_ref = false;
    bool gdr = false;
    bool inter_allowed = true;
    bool intra_allowed = false;
};

void WritePh(BitWriter& w, const PhSpec& ph) {
    w.Bit(ph.gdr_or_irap);
    w.Bit(ph.non_ref); // ❗ 无条件存在：少这一位后面全错
    if (ph.gdr_or_irap)
        w.Bit(ph.gdr);
    w.Bit(ph.inter_allowed);
    if (ph.inter_allowed)
        w.Bit(ph.intra_allowed);
    w.Ue(static_cast<uint32_t>(ph.pps_id));
    w.Bits(static_cast<uint32_t>(ph.poc_lsb), 8);
}

std::vector<uint8_t> PhPayload(const PhSpec& ph) {
    BitWriter w;
    WritePh(w, ph);
    return w.Bytes();
}

// slice：引用外部 PH（sh_picture_header_in_slice_header_flag = 0）。
// inter_allowed 为 false 时规范推断 slice_type = I，位流里没有这个字段。
std::vector<uint8_t> SlicePayload(int slice_type, bool has_slice_type_field = true) {
    BitWriter w;
    w.Bit(0);
    if (has_slice_type_field)
        w.Ue(static_cast<uint32_t>(slice_type));
    return w.Bytes();
}

// slice：PH 内联在 slice header 里（flag = 1，一幅图像一个 slice）。
std::vector<uint8_t> SliceWithInlinePhPayload(const PhSpec& ph, int slice_type) {
    BitWriter w;
    w.Bit(1);
    WritePh(w, ph);
    w.Ue(static_cast<uint32_t>(slice_type));
    return w.Bytes();
}

// Annex B 封装一个 VVC NAL（2 字节 header）。
std::vector<uint8_t> AnnexBNal(uint8_t type, uint8_t temporal_id_plus1, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = {0x00, 0x00, 0x00,
                                0x01, 0x00, static_cast<uint8_t>((type << 3) | (temporal_id_plus1 & 0x07))};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// 测试夹具：默认 Annex B 封装 + 先喂 SPS/PPS（带内路径）。
class VvcStream {
public:
    explicit VvcStream(const std::vector<uint8_t>& pps = MakePpsNoPartition()) {
        tracker_.Reset(AV_CODEC_ID_VVC, 0);
        tracker_.SetFraming(FrameOrderTracker::Framing::AnnexB);
        Feed(15, kSps1080p10Bit);
        Feed(16, pps);
    }

    int Feed(uint8_t type, const std::vector<uint8_t>& payload) {
        const std::vector<uint8_t> bytes = AnnexBNal(type, 1, payload);
        return tracker_.Feed(bytes.data(), bytes.size(), false, 0.0, packet_++);
    }

    // 一幅图像：PH（可选，独立 NAL）+ 若干 slice。
    // forced_nal_type >= 0 时用它当 VCL NAL 类型（RADL=2 / RASL=3 这类前导图像
    // 必须显式指定，否则默认的 TRAIL 不会带 leading 语义）。
    int FeedPicture(const PhSpec& ph, int slice_type, bool ph_in_slice = false, int slice_count = 1,
                    int forced_nal_type = -1) {
        int frames = 0;
        if (!ph_in_slice)
            Feed(19, PhPayload(ph));
        for (int i = 0; i < slice_count; ++i) {
            const std::vector<uint8_t> slice =
                ph_in_slice ? SliceWithInlinePhPayload(ph, slice_type) : SlicePayload(slice_type, ph.inter_allowed);
            // 独立 PH 的 slice 用 TRAIL_NUT(0)；IDR/CRA 必须用对应类型，
            // 否则 IRAP/POC 复位判据不会触发。
            uint8_t nal_type = 0;
            if (forced_nal_type >= 0)
                nal_type = static_cast<uint8_t>(forced_nal_type);
            else if (ph.gdr_or_irap)
                nal_type = ph.gdr ? 10 : irap_nal_type_; // GDR_NUT / IDR / CRA
            frames += Feed(nal_type, slice);
        }
        return frames;
    }

    void UseIrapNalType(uint8_t type) {
        irap_nal_type_ = type;
    }

    const FrameOrderTimeline& Finish() {
        return tracker_.Finish();
    }
    const FrameOrderTimeline& Timeline() const {
        return tracker_.timeline();
    }

private:
    FrameOrderTracker tracker_;
    int64_t packet_ = 0;
    uint8_t irap_nal_type_ = 7; // 默认 IDR_W_RADL
};

std::vector<int64_t> Pocs(const FrameOrderTimeline& t) {
    std::vector<int64_t> out;
    out.reserve(t.frames.size());
    for (const FrameOrderEntry& e : t.frames)
        out.push_back(e.poc);
    return out;
}

// --------------------------------------------------------------------------
// POC 与 I/P/B 归类
// --------------------------------------------------------------------------
TEST(VvcFrameOrderTest, PocFromPictureHeaderAndSliceTypes) {
    VvcStream stream;
    stream.FeedPicture(PhSpec{0, 0, true, false, false, false, false}, 2); // IDR，全 I 图像
    stream.FeedPicture(PhSpec{0, 4, false, false, false, true, false}, 1); // P
    stream.FeedPicture(PhSpec{0, 2, false, false, false, true, false}, 0); // B

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    ASSERT_EQ(t.frames_observed, 3);
    EXPECT_EQ(Pocs(t), (std::vector<int64_t>{0, 4, 2}));

    EXPECT_EQ(t.i_count, 1);
    EXPECT_EQ(t.p_count, 1);
    EXPECT_EQ(t.b_count, 1);
    EXPECT_EQ(t.other_count, 0);
    EXPECT_EQ(t.idr_count, 1);
    EXPECT_EQ(t.irap_count, 1);
    // 解码顺序 0 -> 4 -> 2：回退一次、幅度 2（= 重排深度）
    EXPECT_EQ(t.reorder_events, 1);
    EXPECT_EQ(t.reorder_depth, 2);
    // 单 GOP
    EXPECT_EQ(t.gop_count, 1);
}

TEST(VvcFrameOrderTest, IdrResetsPoc) {
    VvcStream stream;
    stream.FeedPicture(PhSpec{0, 0, true, false, false, false, false}, 2); // IDR
    stream.FeedPicture(PhSpec{0, 4, false, false, false, true, false}, 1); // P
    stream.FeedPicture(PhSpec{0, 0, true, false, false, false, false}, 2); // 第二个 IDR
    stream.FeedPicture(PhSpec{0, 2, false, false, false, true, false}, 0); // B

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    // IDR 把 POC 归零：第二段从 0 重新开始，而不是接着 4 往上加
    EXPECT_EQ(Pocs(t), (std::vector<int64_t>{0, 4, 0, 2}));
    EXPECT_EQ(t.idr_count, 2);
    EXPECT_EQ(t.gop_count, 2);
    // IDR 处的"回退"不是重排，只有 4 -> 0 之后的 0 -> 2？没有回退；
    // 4 -> 0 由 idr 判据排除
    EXPECT_EQ(t.reorder_events, 0);
}

TEST(VvcFrameOrderTest, LsbWrapAroundUsesHalfRangeRule) {
    VvcStream stream;
    // 首幅 IDR 直接给 lsb=250：复位路径不参与回绕推算，POC = 250。
    // （若从 lsb=0 直接跳到 250，差值 250 > 半周期，按 8.3.1 会被判成回退 ——
    //  那是规范的语义，不是缺陷，所以测试要用连续递增的序列。）
    stream.FeedPicture(PhSpec{0, 250, true, false, false, false, false}, 2);
    stream.FeedPicture(PhSpec{0, 254, false, false, false, true, false}, 1);
    stream.FeedPicture(PhSpec{0, 2, false, false, false, true, false}, 0); // 回绕到下一周期

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    // 254 -> 2：差值 252 >= 128（MaxPicOrderCntLsb/2），MSB 进一位 => 256 + 2
    EXPECT_EQ(Pocs(t), (std::vector<int64_t>{250, 254, 258}));
    EXPECT_EQ(t.reorder_events, 0);
}

// 反例：见到 IRAP 就复位会让"插在中间的 CRA"整体偏一个回绕周期。
// 8.1.3 里 CRA 的 NoRaslOutputFlag 只在"码流第一幅图 / EOS 之后"为 1。
TEST(VvcFrameOrderTest, MidStreamCraDoesNotResetPoc) {
    VvcStream stream;
    stream.FeedPicture(PhSpec{0, 192, true, false, false, false, false}, 2); // IDR
    stream.UseIrapNalType(9);                                                // CRA_NUT
    stream.FeedPicture(PhSpec{0, 8, true, false, false, false, false}, 2);   // 中间的 CRA

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    // 不复位：8 < 192 且差值 184 >= 128 => MSB +256 => 264。
    // 若错误地按 IRAP 复位，这里会是 8。
    EXPECT_EQ(Pocs(t), (std::vector<int64_t>{192, 264}));
    EXPECT_EQ(t.idr_count, 1);
    EXPECT_EQ(t.irap_count, 2);
    EXPECT_EQ(t.gop_count, 2);
}

// 前导图像（RADL/RASL）的 POC 天生更小，不是 B 帧重排。
// 这里第二个前导图像的**前一帧不是 IRAP**（是另一个前导图像），
// 所以只有 entry.leading 能把它排除；沿用 HEVC 的 6..9 判据会数成 1 次重排。
TEST(VvcFrameOrderTest, LeadingPicturesAreNotCountedAsReorder) {
    VvcStream stream;
    stream.UseIrapNalType(9); // CRA 起始（开放 GOP 的常见形态）
    stream.FeedPicture(PhSpec{0, 16, true, false, false, false, false}, 2);              // CRA   POC 16
    stream.FeedPicture(PhSpec{0, 12, false, false, false, true, false}, 0, false, 1, 3); // RASL  POC 12
    stream.FeedPicture(PhSpec{0, 4, false, false, false, true, false}, 0, false, 1, 2);  // RADL  POC 4
    stream.FeedPicture(PhSpec{0, 20, false, false, false, true, false}, 1);              // TRAIL POC 20

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    ASSERT_EQ(t.frames.size(), 4u);
    EXPECT_TRUE(t.frames[1].leading);
    EXPECT_TRUE(t.frames[2].leading);
    EXPECT_FALSE(t.frames[3].leading);
    // 12 -> 4 是同一幅 IRAP 的前导图像，语义上就该更早，不是重排。
    // 若 leading 判据失效，这里会 >= 1。
    EXPECT_EQ(t.reorder_events, 0);
}

TEST(VvcFrameOrderTest, MultipleSlicesInOnePictureCountOnce) {
    VvcStream stream;
    const PhSpec ph{0, 10, false, false, false, true, false};
    // 一个 PH + 三个 slice（同一幅图像）
    EXPECT_EQ(stream.FeedPicture(ph, 1, false, 3), 1);

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.frames_observed, 1);
    EXPECT_EQ(t.frames.size(), 1u);
    EXPECT_EQ(t.frames[0].poc, 10);
}

TEST(VvcFrameOrderTest, InlinePictureHeaderInSliceWorks) {
    VvcStream stream;
    // 码流里没有 PH NAL：PH 内联在 slice header 开头（单 slice 图像的常见形态）
    EXPECT_EQ(stream.FeedPicture(PhSpec{0, 6, false, false, false, true, false}, 0, true), 1);

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    ASSERT_EQ(t.frames.size(), 1u);
    EXPECT_EQ(t.frames[0].poc, 6);
    EXPECT_EQ(t.frames[0].slice_type, 1); // B
}

// --------------------------------------------------------------------------
// "不猜"的边界
// --------------------------------------------------------------------------
TEST(VvcFrameOrderTest, MissingPpsDoesNotInventFrame) {
    VvcStream stream;
    // 引用一个不存在的 PPS：POC 的位宽无从得知
    stream.FeedPicture(PhSpec{5, 0, false, false, false, true, false}, 1);

    const FrameOrderTimeline& t = stream.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_EQ(t.frames_observed, 0);
    EXPECT_TRUE(t.frames.empty());
    EXPECT_NE(t.unavailable_reason.find("PPS"), std::string::npos);
}

TEST(VvcFrameOrderTest, TruncatedPictureHeaderDoesNotInventFrame) {
    VvcStream stream;
    // 只给 1 字节：位流在 ph_pic_order_cnt_lsb 之前就耗尽
    stream.Feed(19, {0xC0});

    const FrameOrderTimeline& t = stream.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_EQ(t.frames_observed, 0);
    EXPECT_TRUE(t.frames.empty());
}

// 图像被 tile 划分过（PPS 的 no_pic_partition_flag = 0）：sh_slice_address 的
// 宽度无法从现有字段反推。此时 POC 照常给，slice_type 必须记未知（'?'），
// 而不是从错位的位里读一个看起来合理的 I/P/B。
TEST(VvcFrameOrderTest, SliceTypeUnknownWhenAddressWidthNotProvable) {
    VvcStream stream(kPpsPartitioned);
    stream.FeedPicture(PhSpec{0, 8, false, false, false, true, false}, 1);

    const FrameOrderTimeline& t = stream.Finish();
    ASSERT_TRUE(t.available);
    ASSERT_EQ(t.frames.size(), 1u);
    EXPECT_EQ(t.frames[0].poc, 8);         // POC 与 slice_type 无关，照常可信
    EXPECT_EQ(t.frames[0].slice_type, -1); // 不猜
    EXPECT_EQ(t.frames[0].TypeChar(), '?');
    EXPECT_EQ(t.other_count, 1);
    EXPECT_EQ(t.i_count + t.p_count + t.b_count, 0);
}

// --------------------------------------------------------------------------
// 分派层 + extradata 路径
// --------------------------------------------------------------------------
TEST(VvcFrameOrderTest, TrackerDispatchEnablesVvc) {
    FrameOrderTracker tracker;
    tracker.Reset(AV_CODEC_ID_VVC, 0);
    EXPECT_TRUE(tracker.enabled());
    EXPECT_STREQ(tracker.codec_name(), "vvc");
    EXPECT_STREQ(tracker.order_source_name(), "poc");
}

// MP4 的 VVC extradata 是 vvcC：参数集只写在里面，码流里一个 SPS 都没有。
// 少了这条路径，帧顺序在主力封装上会恒为"不可用"，原因还会写成"没有解析到 SPS"。
TEST(VvcFrameOrderTest, ExtradataVvcCProvidesParameterSets) {
    // 最小 vvcC：ptl_present = 0，两组数组（SPS / PPS）
    const auto array = [](uint8_t nal_type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> out = {static_cast<uint8_t>(0x80 | nal_type), 0x00, 0x01};
        const uint16_t len = static_cast<uint16_t>(payload.size() + 2);
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.push_back(0x00); // NAL header byte0
        out.push_back(static_cast<uint8_t>((nal_type << 3) | 0x01));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    };

    std::vector<uint8_t> vvcc = {0xFE}; // reserved(5)=11111 | lengthSizeMinusOne=3 | ptl_present=0
    vvcc.push_back(0x02);               // num_of_arrays
    for (const auto& part : {array(15, kSps1080p10Bit), array(16, MakePpsNoPartition())})
        vvcc.insert(vvcc.end(), part.begin(), part.end());

    const ExtradataResult parsed = ExtradataParser::ParseWithFormat(ExtradataFormat::VvcC, vvcc.data(), vvcc.size());
    ASSERT_TRUE(parsed.valid) << parsed.error_message;
    ASSERT_EQ(parsed.nal_units.size(), 2u);

    FrameOrderTracker tracker;
    tracker.Reset(AV_CODEC_ID_VVC, 0);
    tracker.SetFraming(FrameOrderTracker::Framing::AnnexB);
    ASSERT_TRUE(tracker.FeedExtradata(vvcc.data(), vvcc.size()));

    const std::vector<uint8_t> ph = AnnexBNal(19, 1, PhPayload(PhSpec{0, 12, false, false, false, true, false}));
    const std::vector<uint8_t> slice = AnnexBNal(0, 1, SlicePayload(1));
    tracker.Feed(ph.data(), ph.size(), false, 0.0, 0);
    tracker.Feed(slice.data(), slice.size(), false, 0.0, 1);

    const FrameOrderTimeline& t = tracker.Finish();
    ASSERT_TRUE(t.available);
    ASSERT_EQ(t.frames.size(), 1u);
    EXPECT_EQ(t.frames[0].poc, 12);
    EXPECT_EQ(t.frames[0].slice_type, 0); // P
}

} // namespace
