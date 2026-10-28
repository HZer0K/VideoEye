// 帧级 POC 追踪（"帧级码流结构"里"显示顺序"那一半）的正反例回归测试。
//
// 被测的是 H264PocTracker：容器只给解码顺序（sample 顺序 + DTS），显示顺序必须从
// slice_header 的 POC 推。没有它，工具只能说"这段有 B 帧"（看 slice_type），
// 说不出重排深度、GOP 边界、POC 有没有重复或缺失。
//
// 期望值的来源（**不**是把实现的输出抄一遍）：
//   * A 组：本文件里的 BitWriter 按 H.264 7.3.2.1.1（SPS）/ 7.3.3（slice_header）
//     与 8.2.1（POC 推导）逐位拼出样本，期望值由规范语义直接给出
//     —— 例如"显示顺序 I B B P 时，解码顺序里 POC 必然回退一次、回退 4 个槽位"。
//   * B 组：用带 libx264 的 ffmpeg 命令行预先编好并入库的语料
//     （tests/corpus/codec/real_x264_bframes_gop.mp4，脚本
//     generate_real_corpus.py），拿**编码器写进容器的 PTS**（显示时间戳）当
//     参考输出：按 POC 排出来的显示顺序必须与按 PTS 排出来的逐项一致，且排序
//     后的时间间隔必须处处等于 1/fps。这两条对得上，说明 POC 重建出的显示
//     顺序与真实编码器的显示顺序一致，而不是自洽的巧合。
//
//     ❗ B 组的 B 帧样本**不能**用 TestMediaFactory 现场生成：仓库锁定的预编译
//     FFmpeg 没有 libx264（GPL 组件），会降级到 libopenh264，而它不出 B 帧
//     —— 那样"要 B 帧"的用例会静默退化成空跑。
//
// 明确不在本测试范围内的能力（代码里也不实现，见 H264PocTracker.h 文件头）：
//   * pic_order_cnt_type == 1 —— 有反例断言它给出"不可用 + 原因"，不给 0；
//   * 参考帧列表 / DPB / MMCO —— 不重建，因此没有对应的断言；
//   * HEVC / AV1 的 POC —— 未实现，本文件不碰。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/analysis/codec/H264PocTracker.h"
#include "core/analysis/orchestration/AnalysisEngine.h"
#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"

#include "tests/support/FrameOrderTestSupport.h"
#include "tests/support/TestMediaFactory.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
}

namespace {

using videoeye::AnalysisEngine;
using videoeye::AnalysisOptions;
using videoeye::H264PocTracker;
using videoeye::model::AnalysisResult;
using videoeye::model::FrameOrderEntry;
using videoeye::model::FrameOrderTimeline;
// 共用装置（容器参考顺序 / 只开帧顺序维度的引擎调用）在 tests/support 里，
// 与 HEVC 的用例共用同一份 —— 两边的对账口径必须逐字相同。
using videoeye_test::CodecSample;
using videoeye_test::DemuxedOrderFacts;
using videoeye_test::DemuxForDisplayOrder;
using videoeye_test::DisplayOrderPerGop;
using videoeye_test::MaxBackwardStep;
using videoeye_test::RunFrameOrderAnalysis;
using videoeye_test::Sequential;
using videoeye_test::SkipIfMissing;

// ---------------------------------------------------------------------------
// 极简位写入器（MSB-first，与 H.264 的 RBSP 位序一致）
// ---------------------------------------------------------------------------
class BitWriter {
public:
    void U(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i)
            Bit(((value >> i) & 1u) != 0);
    }
    // ue(v)：code_num = value + 1，先写 (位宽-1) 个前导 0，再写 1，再写剩余位
    void Ue(uint32_t value) {
        const uint64_t code_num = static_cast<uint64_t>(value) + 1;
        int bits = 0;
        while ((code_num >> bits) > 1)
            ++bits;
        for (int i = 0; i < bits; ++i)
            Bit(false);
        Bit(true);
        for (int i = bits - 1; i >= 0; --i)
            Bit(((code_num >> i) & 1) != 0);
    }
    void Se(int32_t value) {
        // se(v) 的映射：k = 2|v|-1（v<0 时为偶数），符号由奇偶给出
        const uint32_t k = (value <= 0) ? static_cast<uint32_t>(-2 * value) : static_cast<uint32_t>(2 * value - 1);
        Ue(k);
    }
    void Trailing() {
        Bit(true);
        while (bit_count_ % 8 != 0)
            Bit(false);
    }
    std::vector<uint8_t> Bytes() const {
        return data_;
    }

private:
    void Bit(bool value) {
        if (bit_count_ % 8 == 0)
            data_.push_back(0);
        if (value)
            data_.back() |= static_cast<uint8_t>(1u << (7 - (bit_count_ % 8)));
        ++bit_count_;
    }
    std::vector<uint8_t> data_;
    int bit_count_ = 0;
};

// ---------------------------------------------------------------------------
// 参数集与 slice header 生成
// ---------------------------------------------------------------------------
struct SpsParams {
    int id = 0;
    int poc_type = 0;
    // log2(max_frame_num)：规范里写作 log2_max_frame_num_minus4 + 4
    int log2_frame_num = 8;
    // log2(max_pic_order_cnt_lsb)：只在 poc_type == 0 时写进码流
    int log2_poc_lsb = 8;
    // 176x144（QCIF）是 16 的整数倍，不需要 frame_cropping，省掉一类无关差异
    int width = 176;
    int height = 144;
};

std::vector<uint8_t> MakeSps(const SpsParams& p) {
    const int mb_w = p.width / 16;
    const int mb_h = p.height / 16;
    BitWriter w;
    w.U(66, 8); // profile_idc = Baseline（无 chroma_format_idc 等 High 扩展字段）
    w.U(0, 8);  // constraint_set_flags + reserved
    w.U(30, 8); // level_idc = 3.0
    w.Ue(static_cast<uint32_t>(p.id));
    w.Ue(static_cast<uint32_t>(p.log2_frame_num - 4));
    w.Ue(static_cast<uint32_t>(p.poc_type));
    if (p.poc_type == 0) {
        w.Ue(static_cast<uint32_t>(p.log2_poc_lsb - 4));
    } else if (p.poc_type == 1) {
        // delta_pic_order_always_zero_flag=1 → slice_header 里不出现 delta_pic_order_cnt
        w.U(1, 1);
        w.Se(0); // offset_for_non_ref_pic
        w.Se(0); // offset_for_top_to_bottom_field
        w.Ue(0); // num_ref_frames_in_pic_order_cnt_cycle = 0
    }
    w.Ue(1);   // max_num_ref_frames
    w.U(0, 1); // gaps_in_frame_num_value_allowed_flag
    w.Ue(static_cast<uint32_t>(mb_w - 1));
    w.Ue(static_cast<uint32_t>(mb_h - 1));
    w.U(1, 1); // frame_mbs_only_flag = 1（逐行，无场图像）
    w.U(1, 1); // direct_8x8_inference_flag
    w.U(0, 1); // frame_cropping_flag
    w.U(0, 1); // vui_parameters_present_flag
    w.Trailing();
    return w.Bytes();
}

std::vector<uint8_t> MakePps(int pps_id, int sps_id) {
    BitWriter w;
    w.Ue(static_cast<uint32_t>(pps_id));
    w.Ue(static_cast<uint32_t>(sps_id));
    w.U(0, 1); // entropy_coding_mode_flag
    w.U(0, 1); // bottom_field_pic_order_in_frame_present_flag
    w.Ue(0);   // num_slice_groups_minus1
    w.Ue(0);   // num_ref_idx_l0_default_active_minus1
    w.Ue(0);   // num_ref_idx_l1_default_active_minus1
    w.U(0, 1); // weighted_pred_flag
    w.U(0, 2); // weighted_bipred_idc
    w.Se(0);   // pic_init_qp_minus26
    w.Se(0);   // pic_init_qs_minus26
    w.Se(0);   // chroma_qp_index_offset
    w.U(0, 1); // deblocking_filter_control_present_flag
    w.U(0, 1); // constrained_intra_pred_flag
    w.U(0, 1); // redundant_pic_cnt_present_flag
    w.Trailing();
    return w.Bytes();
}

struct SliceParams {
    int first_mb = 0;
    int slice_type = 2; // 归一化：0=P 1=B 2=I
    int pps_id = 0;
    int frame_num = 0;
    bool idr = false;
    int poc_lsb = 0; // poc_type 0 下写进码流的 pic_order_cnt_lsb
    int log2_frame_num = 8;
    int log2_poc_lsb = 8;
    int poc_type = 0;
};

std::vector<uint8_t> MakeSlice(const SliceParams& p) {
    BitWriter w;
    w.Ue(static_cast<uint32_t>(p.first_mb));
    w.Ue(static_cast<uint32_t>(p.slice_type));
    w.Ue(static_cast<uint32_t>(p.pps_id));
    w.U(static_cast<uint32_t>(p.frame_num), p.log2_frame_num);
    // frame_mbs_only_flag = 1 → 没有 field_pic_flag / bottom_field_flag
    if (p.idr)
        w.Ue(0); // idr_pic_id
    if (p.poc_type == 0)
        w.U(static_cast<uint32_t>(p.poc_lsb), p.log2_poc_lsb);
    w.Trailing();
    return w.Bytes();
}

// Annex B 封装：00 00 00 01 + NAL header(1B) + RBSP（带 emulation prevention 转义）
std::vector<uint8_t> WrapAnnexB(int nal_type, int nal_ref_idc, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out = {0x00, 0x00, 0x00, 0x01};
    out.push_back(static_cast<uint8_t>((nal_ref_idc << 5) | (nal_type & 0x1F)));
    // emulation_prevention_three_byte：连续两个 0x00 之后出现 <=0x03 的字节要插 0x03。
    // 真实编码器都这么做，测试样本也照做 —— 否则测的是"没有防冲突字节"的简化世界。
    int zeros = 0;
    for (uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 0x03) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = (byte == 0x00) ? zeros + 1 : 0;
    }
    return out;
}

// avcC 配置记录（ISO/IEC 14496-15 5.2.4.1）。MP4/MOV 的 extradata 就是它，
// SPS/PPS **带 1 字节 NAL header** 存在里面（与媒体包里"只有 payload"不同）。
std::vector<uint8_t> MakeAvcC(int profile, int level, const std::vector<uint8_t>& sps_rbsp,
                              const std::vector<uint8_t>& pps_rbsp) {
    std::vector<uint8_t> out;
    out.push_back(0x01); // configurationVersion
    out.push_back(static_cast<uint8_t>(profile));
    out.push_back(0x00); // profile_compatibility
    out.push_back(static_cast<uint8_t>(level));
    out.push_back(0xFF); // 6 bit 保留 + lengthSizeMinusOne = 3（4 字节）
    out.push_back(0xE1); // 3 bit 保留 + numOfSPS = 1
    std::vector<uint8_t> sps_nal = {0x67};
    sps_nal.insert(sps_nal.end(), sps_rbsp.begin(), sps_rbsp.end());
    out.push_back(static_cast<uint8_t>((sps_nal.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(sps_nal.size() & 0xFF));
    out.insert(out.end(), sps_nal.begin(), sps_nal.end());
    out.push_back(0x01); // numOfPPS = 1
    std::vector<uint8_t> pps_nal = {0x68};
    pps_nal.insert(pps_nal.end(), pps_rbsp.begin(), pps_rbsp.end());
    out.push_back(static_cast<uint8_t>((pps_nal.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(pps_nal.size() & 0xFF));
    out.insert(out.end(), pps_nal.begin(), pps_nal.end());
    return out;
}

// 长度前缀封装（MP4/MOV/fMP4 的媒体包形态）
std::vector<uint8_t> WrapLengthPrefixed(int nal_type, int nal_ref_idc, const std::vector<uint8_t>& rbsp,
                                        int prefix_bytes) {
    const std::vector<uint8_t> nal = WrapAnnexB(nal_type, nal_ref_idc, rbsp);
    // WrapAnnexB 的 payload 部分从第 5 字节开始（去掉 4 字节起始码）
    const std::vector<uint8_t> payload(nal.begin() + 4, nal.end());
    std::vector<uint8_t> out;
    const uint32_t size = static_cast<uint32_t>(payload.size());
    for (int i = prefix_bytes - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((size >> (8 * i)) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> Concat(const std::vector<std::vector<uint8_t>>& parts) {
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// ---------------------------------------------------------------------------
// 测试脚手架：建流 → 逐包喂 → 收尾
// ---------------------------------------------------------------------------
class PocFixture {
public:
    // 只喂参数集（不带时间戳：参数集不是帧）
    void FeedParameterSets(const SpsParams& sps_params, int pps_id = 0, int sps_id = 0) {
        sps_params_ = sps_params;
        const std::vector<uint8_t> sps = WrapAnnexB(7, 3, MakeSps(sps_params));
        const std::vector<uint8_t> pps = WrapAnnexB(8, 3, MakePps(pps_id, sps_id));
        tracker_.Feed(sps.data(), sps.size(), false, 0.0, 0);
        tracker_.Feed(pps.data(), pps.size(), false, 0.0, 1);
        packet_index_ = 2;
    }

    // 喂一帧（一个包一帧）。poc_lsb 用"显示序号 × 2"，与逐行流的 POC 步长一致。
    void FeedFrame(int slice_type, int frame_num, int poc_lsb, bool idr = false, int first_mb = 0) {
        SliceParams p;
        p.slice_type = slice_type;
        p.frame_num = frame_num;
        p.poc_lsb = poc_lsb;
        p.idr = idr;
        p.first_mb = first_mb;
        p.log2_frame_num = sps_params_.log2_frame_num;
        p.log2_poc_lsb = sps_params_.log2_poc_lsb;
        p.poc_type = sps_params_.poc_type;
        const std::vector<uint8_t> packet = WrapAnnexB(idr ? 5 : 1, idr ? 3 : 2, MakeSlice(p));
        // 时间戳按"显示序号"给：这是编码器自己会写进容器的显示时间戳，
        // B 组的真实样本对账用的就是这种关系。
        const double time_seconds = static_cast<double>(poc_lsb) / 2.0 * frame_duration_;
        tracker_.Feed(packet.data(), packet.size(), true, time_seconds, packet_index_++);
    }

    const FrameOrderTimeline& Finish() {
        // 幂等性顺手在这里盯：连调两次结果必须一致
        tracker_.Finish();
        return tracker_.Finish();
    }

    H264PocTracker& tracker() {
        return tracker_;
    }

private:
    H264PocTracker tracker_;
    SpsParams sps_params_;
    int64_t packet_index_ = 0;
    double frame_duration_ = 1.0 / 25.0;
};

// ---------------------------------------------------------------------------
// 1) 正例：IBBP 的显示顺序重排
// ---------------------------------------------------------------------------
//
// 显示顺序 I0 B1 B2 P3（POC = 显示序号 × 2 → 0 2 4 6），
// 解码顺序 = I0(POC 0) → P3(POC 6) → B1(POC 2) → B2(POC 4)。
// 于是在解码顺序里 POC 序列是 0,6,2,4 —— 必然且只在 6→2 处回退一次，回退 4。
// 这个 4 就是"重排深度"（POC 步长为 2 ，即 2 帧）。
TEST(H264PocTrackerTest, IbbpGopReorderingIsDetectedWithDepth) {
    PocFixture f;
    f.FeedParameterSets(SpsParams{});
    f.FeedFrame(2, 0, 0, /*idr=*/true); // I
    f.FeedFrame(0, 1, 6);               // P（显示第 3 帧）
    f.FeedFrame(1, 2, 2);               // B（显示第 1 帧）
    f.FeedFrame(1, 3, 4);               // B（显示第 2 帧）
    const FrameOrderTimeline& t = f.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 4);
    EXPECT_EQ(t.i_count, 1);
    EXPECT_EQ(t.p_count, 1);
    EXPECT_EQ(t.b_count, 2);
    EXPECT_EQ(t.idr_count, 1);
    EXPECT_EQ(t.poc_type, 0);

    // 只有一处回退，幅度 4 —— 期望值来自"显示顺序 I B B P"这个结构，不是实现给的
    EXPECT_EQ(t.reorder_events, 1);
    EXPECT_EQ(t.reorder_depth, 4);
    EXPECT_TRUE(t.HasReordering());

    // 没有重复、没有缺口、单 GOP
    EXPECT_EQ(t.duplicate_poc_count, 0);
    EXPECT_EQ(t.poc_gap_slots, 0);
    EXPECT_EQ(t.gop_count, 1);
    EXPECT_EQ(t.max_gop_frames, 4);

    // 逐帧明细：POC 必须与上面喂进去的值一一对应
    ASSERT_EQ(t.frames.size(), 4u);
    const std::vector<int64_t> pocs = {t.frames[0].poc, t.frames[1].poc, t.frames[2].poc, t.frames[3].poc};
    EXPECT_EQ(pocs, (std::vector<int64_t>{0, 6, 2, 4}));
    EXPECT_EQ(t.frames[0].TypeChar(), 'I');
    EXPECT_EQ(t.frames[1].TypeChar(), 'P');
    EXPECT_EQ(t.frames[2].TypeChar(), 'B');
    EXPECT_TRUE(t.frames[0].idr);
    EXPECT_FALSE(t.frames[1].idr);
}

// ---------------------------------------------------------------------------
// 2) 正例（反"假信号"）：IDR 重置 POC 不能算重排，跨 GOP 的 POC 不能算重复
// ---------------------------------------------------------------------------
//
// 两个结构相同的 GOP：POC 序列 0,2,4 | 0,2,4。
// 这段流里一个 B 帧都没有，所以重排必须是 0；POC 值虽然重复出现，
// 但那是两个 GOP 各自从 0 开始计，不是"同一幅图像被写了两次" —— 重复也必须是 0。
// 这一条同时盯住两个最容易造假的口径（多 GOP 文件里它们会恒 > 0）。
TEST(H264PocTrackerTest, IdrResetIsNotReorderingAndCrossGopPocIsNotDuplicate) {
    PocFixture f;
    f.FeedParameterSets(SpsParams{});
    for (int gop = 0; gop < 2; ++gop) {
        f.FeedFrame(2, 0, 0, /*idr=*/true);
        f.FeedFrame(0, 1, 2);
        f.FeedFrame(0, 2, 4);
    }
    const FrameOrderTimeline& t = f.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 6);
    EXPECT_EQ(t.idr_count, 2);
    // 关键：这两项在"跨 GOP 混着统计"的实现下分别是 1 与 3
    EXPECT_EQ(t.reorder_events, 0);
    EXPECT_EQ(t.duplicate_poc_count, 0);
    EXPECT_EQ(t.poc_gap_slots, 0);
    EXPECT_FALSE(t.HasReordering());

    EXPECT_EQ(t.gop_count, 2);
    EXPECT_EQ(t.min_gop_frames, 3);
    EXPECT_EQ(t.max_gop_frames, 3);
    EXPECT_DOUBLE_EQ(t.avg_gop_frames, 3.0);
}

// ---------------------------------------------------------------------------
// 3) 正例：一帧多个 slice 只能算一帧
// ---------------------------------------------------------------------------
//
// 规范里 prevPicOrderCnt 记的是"上一幅图像"，不是"上一个 slice"。
// 不按 first_mb_in_slice == 0 收敛，多 slice 的帧会被数成多帧，
// 帧数 / I:P:B 比例 / GOP 长度全线失真。
TEST(H264PocTrackerTest, MultiSlicePictureIsCountedOnce) {
    PocFixture f;
    f.FeedParameterSets(SpsParams{});
    f.FeedFrame(2, 0, 0, /*idr=*/true, /*first_mb=*/0);
    f.FeedFrame(2, 0, 0, /*idr=*/true, /*first_mb=*/40); // 同帧第二个 slice
    f.FeedFrame(2, 0, 0, /*idr=*/true, /*first_mb=*/80); // 同帧第三个 slice
    f.FeedFrame(0, 1, 2, /*idr=*/false, /*first_mb=*/0);
    f.FeedFrame(0, 1, 2, /*idr=*/false, /*first_mb=*/40); // 同帧第二个 slice
    const FrameOrderTimeline& t = f.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 2) << "5 个 slice 属于 2 幅图像";
    ASSERT_EQ(t.frames.size(), 2u);
    EXPECT_EQ(t.frames[0].poc, 0);
    EXPECT_EQ(t.frames[1].poc, 2);
    EXPECT_EQ(t.i_count, 1);
    EXPECT_EQ(t.p_count, 1);
    EXPECT_EQ(t.reorder_events, 0);
}

// ---------------------------------------------------------------------------
// 4) 正例：pic_order_cnt_type == 2 的 frame_num 路径
// ---------------------------------------------------------------------------
//
// type 2 没有 pic_order_cnt_lsb，POC 由 frame_num 推（8.2.1.3）：
// 逐行时每帧占 2 个 POC 槽位 → POC = abs_frame_num × 2。
TEST(H264PocTrackerTest, PocType2DerivesPocFromFrameNum) {
    PocFixture f;
    SpsParams sps;
    sps.poc_type = 2;
    sps.log2_frame_num = 8;
    f.FeedParameterSets(sps);
    f.FeedFrame(2, 0, 0, /*idr=*/true);
    f.FeedFrame(0, 1, 0);
    f.FeedFrame(0, 2, 0);
    const FrameOrderTimeline& t = f.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.poc_type, 2);
    ASSERT_EQ(t.frames.size(), 3u);
    // poc_lsb 这一路我们全喂了 0：type 2 下它不该参与计算
    EXPECT_EQ(t.frames[0].poc, 0);
    EXPECT_EQ(t.frames[1].poc, 2);
    EXPECT_EQ(t.frames[2].poc, 4);
    EXPECT_EQ(t.reorder_events, 0);
    EXPECT_EQ(t.idr_count, 1);
    EXPECT_EQ(t.gop_count, 1);
}

// ---------------------------------------------------------------------------
// 5) 正例：type 2 的 frame_num 回绕（不回绕就会把后面的帧算成负 POC）
// ---------------------------------------------------------------------------
//
// log2_frame_num = 4 → max_frame_num = 16。frame_num 从 15 回到 0 时必须进位
// （frame_num_offset += 16），否则 abs_frame_num 会从 15 掉到 0，
// 表现为"POC 大幅回退" —— 一个纯属实现错误的假重排。
TEST(H264PocTrackerTest, PocType2FrameNumWrapDoesNotLookLikeReordering) {
    PocFixture f;
    SpsParams sps;
    sps.poc_type = 2;
    sps.log2_frame_num = 4; // max_frame_num = 16
    f.FeedParameterSets(sps);
    f.FeedFrame(2, 0, 0, /*idr=*/true);
    f.FeedFrame(0, 15, 0);
    f.FeedFrame(0, 0, 0); // 回绕：abs_frame_num = 16
    f.FeedFrame(0, 1, 0); // abs_frame_num = 17
    const FrameOrderTimeline& t = f.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 4u);
    EXPECT_EQ(t.frames[1].poc, 30);
    EXPECT_EQ(t.frames[2].poc, 32);
    EXPECT_EQ(t.frames[3].poc, 34);
    EXPECT_EQ(t.reorder_events, 0) << "frame_num 回绕不是显示顺序重排";
}

// ---------------------------------------------------------------------------
// 6) 反例：POC 缺帧（拼接丢帧 / 封装漏帧）
// ---------------------------------------------------------------------------
//
// POC 0,2,4,10,12：步长 2，理论应有 (12-0)/2+1 = 7 个槽位，实际只有 5 个 → 缺 2。
// 这里是"推断总量"，实现不声称知道具体缺了哪两帧。
TEST(H264PocTrackerTest, MissingFramesAreInferredFromPocStep) {
    PocFixture f;
    f.FeedParameterSets(SpsParams{});
    f.FeedFrame(2, 0, 0, /*idr=*/true);
    f.FeedFrame(0, 1, 2);
    f.FeedFrame(0, 2, 4);
    f.FeedFrame(0, 3, 10); // 跳过了 6、8
    f.FeedFrame(0, 4, 12);
    const FrameOrderTimeline& t = f.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 5);
    EXPECT_EQ(t.poc_gap_slots, 2);
    EXPECT_EQ(t.duplicate_poc_count, 0);
    EXPECT_EQ(t.reorder_events, 0);
}

// ---------------------------------------------------------------------------
// 7) 反例：POC 重复（同一幅图像被封装两次）
// ---------------------------------------------------------------------------
TEST(H264PocTrackerTest, DuplicatePocIsDetectedWithinGop) {
    PocFixture f;
    f.FeedParameterSets(SpsParams{});
    f.FeedFrame(2, 0, 0, /*idr=*/true);
    f.FeedFrame(0, 1, 2);
    f.FeedFrame(0, 2, 2); // POC 与上一帧相同
    f.FeedFrame(0, 3, 4);
    const FrameOrderTimeline& t = f.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.duplicate_poc_count, 1);
    EXPECT_EQ(t.frames_observed, 4);
}

// ---------------------------------------------------------------------------
// 8) 正例：extradata 路径（MP4 的 avcC）—— 参数集不在码流里也要能算
// ---------------------------------------------------------------------------
//
// 这是主力封装的真实形态：编码器写完 avcC 之后不再重复 SPS/PPS，
// 只扫包的 tracker 会看到"零个 SPS"，于是把原因记成"扫到 slice 之前没有 SPS"，
// 指向完全错误的方向。
TEST(H264PocTrackerTest, ExtradataParameterSetsEnablePocForLengthPrefixedPackets) {
    H264PocTracker tracker;
    tracker.Reset(0);
    tracker.SetFraming(H264PocTracker::Framing::LengthPrefixed);
    tracker.SetLengthPrefixBytes(4);

    // 标准 avcC 配置记录（与真实 MP4 的 extradata 同构）
    const std::vector<uint8_t> avcc = MakeAvcC(66, 30, MakeSps(SpsParams{}), MakePps(0, 0));
    EXPECT_TRUE(tracker.FeedExtradata(avcc.data(), avcc.size()));

    // 之后进来的媒体包里只有 slice，一个参数集都没有
    SliceParams p;
    p.log2_frame_num = 8;
    p.log2_poc_lsb = 8;
    for (int i = 0; i < 3; ++i) {
        p.frame_num = i;
        p.poc_lsb = i * 2;
        p.slice_type = (i == 0) ? 2 : 0;
        p.idr = (i == 0);
        const std::vector<uint8_t> packet = WrapLengthPrefixed(p.idr ? 5 : 1, p.idr ? 3 : 2, MakeSlice(p), 4);
        tracker.Feed(packet.data(), packet.size(), true, i * 0.04, i + 1);
    }
    const FrameOrderTimeline& t = tracker.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 3);
    ASSERT_EQ(t.frames.size(), 3u);
    EXPECT_EQ(t.frames[2].poc, 4);
}

// ---------------------------------------------------------------------------
// 8b) 反例（回归）：长度前缀落在 256~511 的包不能被当成 Annex B 起始码
// ---------------------------------------------------------------------------
//
// 这是本文件里唯一一条"修过真 bug 之后补的"用例，值得写清楚它防的是什么。
//
// 长度前缀封装的包，前 4 字节是 NAL 长度。当长度落在 [0x100, 0x1FF]
// （256~511 字节 —— 恰好是 P 帧最常见的量级）时，这四字节就是
// `00 00 01 XX`，与 Annex B 的 3 字节起始码**逐字节相同**。
// 于是"扫前 64 字节找起始码"这条自动判定必然误判，把 258 字节的 slice 载荷
// 当成一条 `nal_type = XX` 的 NAL 丢掉。
//
// 实测代价（libopenh264 + MP4，40 帧）：样本大小落在 256~511 的有 33 个，
// 于是 40 帧只扫到 7 帧，而且丢的帧毫无规律 —— 从日志上看只像"某些包没帧"。
//
// 修法是"容器给了前缀宽度就照它切，不猜"（见 H264PocTracker::Feed）。
// 这条用例把前缀长度**凑到** 256~511 区间，让误判必然发生，
// 从而把"有人把自动判定改回无条件优先"这件事钉死。
TEST(H264PocTrackerTest, LengthPrefixInStartCodeByteRangeIsNotMistakenForAnnexB) {
    H264PocTracker tracker;
    tracker.Reset(0);
    // Auto + 已知前缀宽度 = 管线里 MP4/MKV 的真实配置
    tracker.SetFraming(H264PocTracker::Framing::Auto);
    tracker.SetLengthPrefixBytes(4);
    const std::vector<uint8_t> avcc = MakeAvcC(66, 30, MakeSps(SpsParams{}), MakePps(0, 0));
    EXPECT_TRUE(tracker.FeedExtradata(avcc.data(), avcc.size()));

    // 造一个总长度（含 4 字节前缀）落在 256~511 的包：
    // slice 的 RBSP 只有几字节，用尾部填充（cabac_zero_words / 对齐填充，
    // 真实编码器在 slice 尾部就是这么补齐的）把长度顶到 300。
    SliceParams p;
    p.log2_frame_num = 8;
    p.log2_poc_lsb = 8;

    int frames = 0;
    for (int i = 0; i < 3; ++i) {
        p.frame_num = i;
        p.poc_lsb = i * 2;
        p.slice_type = (i == 0) ? 2 : 0;
        p.idr = (i == 0);
        std::vector<uint8_t> packet = WrapLengthPrefixed(p.idr ? 5 : 1, p.idr ? 3 : 2, MakeSlice(p), 4);
        // 4 字节前缀 + payload 顶到 300 字节：前缀变成 00 00 01 2C
        ASSERT_LT(packet.size(), 300u) << "样本本身太长了，凑不出 256~511 这个区间";
        packet.resize(300, 0x00);
        const uint32_t nal_size = 300 - 4;
        packet[0] = static_cast<uint8_t>((nal_size >> 24) & 0xFF);
        packet[1] = static_cast<uint8_t>((nal_size >> 16) & 0xFF);
        packet[2] = static_cast<uint8_t>((nal_size >> 8) & 0xFF);
        packet[3] = static_cast<uint8_t>(nal_size & 0xFF);
        // 自检：前缀必须真的长得像起始码，否则这条用例就是空跑
        ASSERT_EQ(packet[0], 0x00);
        ASSERT_EQ(packet[1], 0x00);
        ASSERT_EQ(packet[2], 0x01);
        frames += tracker.Feed(packet.data(), packet.size(), true, i * 0.04, i + 1);
    }
    EXPECT_EQ(frames, 3) << "每个包都是一帧，一个都不能被当成起始码丢掉";

    const FrameOrderTimeline& t = tracker.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 3);
    ASSERT_EQ(t.frames.size(), 3u);
    EXPECT_EQ(t.frames[2].poc, 4);
}

// ---------------------------------------------------------------------------
// 9) 反例：slice 之前没有 SPS —— 必须给出原因，不能静默给 0 帧
// ---------------------------------------------------------------------------
TEST(H264PocTrackerTest, MissingSpsReportsReasonInsteadOfEmptyResult) {
    H264PocTracker tracker;
    tracker.Reset(0);
    SliceParams p;
    const std::vector<uint8_t> packet = WrapAnnexB(1, 2, MakeSlice(p));
    tracker.Feed(packet.data(), packet.size(), true, 0.0, 0);
    const FrameOrderTimeline& t = tracker.Finish();

    EXPECT_FALSE(t.available);
    EXPECT_FALSE(t.unavailable_reason.empty()) << "不可用时必须给出原因，别让人猜";
    EXPECT_NE(t.unavailable_reason.find("SPS"), std::string::npos);
    EXPECT_EQ(t.frames_observed, 0);
    // 摘要给的是原因而不是"共 0 帧"，避免上层把它当成"扫过但没帧"
    EXPECT_EQ(t.Summary(), t.unavailable_reason);
}

// ---------------------------------------------------------------------------
// 10) 反例：pic_order_cnt_type == 1 —— 明确不支持，不给 0 冒充
// ---------------------------------------------------------------------------
//
// 这是本实现的能力边界。测试的价值在于确认"边界被如实报告"：
// 一旦有人把这条路径补成"先给个 0"，这条测试会红。
TEST(H264PocTrackerTest, PocType1IsExplicitlyUnsupported) {
    PocFixture f;
    SpsParams sps;
    sps.poc_type = 1;
    f.FeedParameterSets(sps);
    f.FeedFrame(2, 0, 0, /*idr=*/true);
    f.FeedFrame(0, 1, 0);
    const FrameOrderTimeline& t = f.Finish();

    EXPECT_FALSE(t.available);
    EXPECT_NE(t.unavailable_reason.find("pic_order_cnt_type=1"), std::string::npos)
        << "原因必须写明是哪一种 POC 类型不支持，实际原因：" << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 0);
}

// ---------------------------------------------------------------------------
// 11) 健壮性：截断 / 垃圾 slice 不能崩，也不能污染统计
// ---------------------------------------------------------------------------
TEST(H264PocTrackerTest, TruncatedSliceIsIgnored) {
    PocFixture f;
    f.FeedParameterSets(SpsParams{});
    f.FeedFrame(2, 0, 0, /*idr=*/true);

    // 一个只有 2 字节的 slice NAL：解析必然读不到 frame_num / poc_lsb
    const std::vector<uint8_t> junk = {0x00, 0x00, 0x00, 0x01, 0x21, 0x00};
    f.tracker().Feed(junk.data(), junk.size(), true, 0.04, 10);
    f.FeedFrame(0, 1, 2);

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 2) << "截断的 slice 不应计入帧数";
}

// ---------------------------------------------------------------------------
// 12) 正例：明细上限只截明细，不截统计
// ---------------------------------------------------------------------------
//
// 上限的语义是"逐帧明细别占太多内存"，统计量（I/P/B、重排、GOP）必须仍是全程的。
// 把两者一起截了，报告里就会出现"共 2 帧、GOP 平均 2 帧"这种自相矛盾的数字。
TEST(H264PocTrackerTest, MaxFramesTruncatesDetailButNotStatistics) {
    H264PocTracker tracker;
    H264PocTracker::Options options;
    options.max_frames = 2;
    tracker.Reset(0, options);
    const std::vector<uint8_t> sps = WrapAnnexB(7, 3, MakeSps(SpsParams{}));
    const std::vector<uint8_t> pps = WrapAnnexB(8, 3, MakePps(0, 0));
    tracker.Feed(sps.data(), sps.size(), false, 0.0, 0);
    tracker.Feed(pps.data(), pps.size(), false, 0.0, 1);

    for (int i = 0; i < 5; ++i) {
        SliceParams p;
        p.frame_num = i;
        p.poc_lsb = i * 2;
        p.slice_type = (i == 0) ? 2 : 0;
        p.idr = (i == 0);
        const std::vector<uint8_t> packet = WrapAnnexB(p.idr ? 5 : 1, 2, MakeSlice(p));
        tracker.Feed(packet.data(), packet.size(), true, i * 0.04, i + 2);
    }
    const FrameOrderTimeline& t = tracker.Finish();

    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_TRUE(t.truncated);
    EXPECT_EQ(t.frames.size(), 2u);
    EXPECT_EQ(t.frames_observed, 5) << "统计量必须仍是全程的";
    EXPECT_EQ(t.i_count, 1);
    EXPECT_EQ(t.p_count, 4);
    // 摘要里要写明"只统计了前面一段"，否则读者会拿 2 当总帧数
    EXPECT_NE(t.Summary().find("仅统计前"), std::string::npos);
}

// ---------------------------------------------------------------------------
// B 组：真实编码器样本
//
// 合成样本能保证"实现与规范一致"，但保证不了"规范理解和真实编码器一致"。
// 这一组拿真实码流做交叉验证，参考输出全部来自 FFmpeg 自己（容器里的时间戳、
// 解码器的输出顺序），与被测代码零共享。
// ---------------------------------------------------------------------------

std::string MakeMedia(const std::string& dir_name, int max_b_frames) {
    const std::string dir = (std::filesystem::temp_directory_path() / dir_name).string();
    std::filesystem::create_directories(dir);
    videoeye_test::TestMediaSpec spec;
    spec.frame_count = 40;
    spec.fps = 25;
    spec.max_b_frames = max_b_frames;
    spec.with_audio = false;
    const videoeye_test::TestMedia media = videoeye_test::CreateTestMedia(dir, "clip", spec);
    EXPECT_TRUE(media.ok) << media.error;
    EXPECT_GT(media.frame_count, 0);
    return media.path;
}

// ---------------------------------------------------------------------------
// B1) 反例（真实样本）：无 B 帧的真实 x264 流不能报重排
// ---------------------------------------------------------------------------
//
// 这一条是 A 组的镜像：那边用合成样本证明"有 B 帧要报出来"，
// 这里用真实编码器证明"没有 B 帧就不能报" —— 两者缺一，
// "重排检测"就只是个恒真的计数器。
//
// 顺带对账 IDR 个数与容器关键帧包数：两者来自完全不同的通道
// （一个是逐 slice 的 NAL 类型，一个是容器的 sync sample 标记），
// 对不上说明其中一边的判据错了。
TEST(H264PocTrackerRealEncoderTest, WithoutBFramesThereIsNoReordering) {
    const std::string path = MakeMedia("videoeye_poc_nobf", 0);
    const AnalysisResult result = RunFrameOrderAnalysis(path);
    ASSERT_EQ(result.scan_status, videoeye::model::AnalysisStatus::Complete) << result.error_message;
    ASSERT_EQ(result.frame_order_timelines.size(), 1u);

    const FrameOrderTimeline& t = result.frame_order_timelines[0];
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 40) << "编码器喂了 40 帧，逐帧扫出来的也必须是 40";
    EXPECT_EQ(t.b_count, 0) << "样本显式关掉了 B 帧";
    EXPECT_EQ(t.reorder_events, 0) << "没有 B 帧时解码顺序 = 显示顺序";
    EXPECT_FALSE(t.HasReordering());
    // gop_size = fps = 25 → 40 帧里两个 IDR（第 0 与第 25 帧）
    EXPECT_EQ(t.idr_count, 2);
    EXPECT_EQ(t.gop_count, 2);
    // 40 帧切成 25 + 15 两段
    EXPECT_EQ(t.min_gop_frames, 15);
    EXPECT_EQ(t.max_gop_frames, 25);
    EXPECT_EQ(t.duplicate_poc_count, 0);
    EXPECT_EQ(t.poc_gap_slots, 0);
}

// ---------------------------------------------------------------------------
// B2) 正例（真实样本）：按 POC 排出来的显示顺序 == 编码器写进容器的显示顺序
// ---------------------------------------------------------------------------
//
// 这一条是整组测试里最硬的：它验证的不是"实现自洽"，而是逐 slice 解出来的 POC
// 排出的显示顺序，与**真实编码器自己记录的显示顺序**逐项一致。
//
// 参考输出只取容器里的 PTS（libx264 经 muxer 写进 stts/ctts），被测代码全程
// 不参与它的产生；两侧唯一的共同输入是同一份码流文件。
//
// 有 B 帧时"包顺序 → 显示顺序"这张映射表是乱序的（例：0,3,2,4,1,...），
// 没有 B 帧时它是 0,1,2,3,...。所以样本必须真的有 B 帧，否则这条测试会退化成
// 恒真的空跑 —— 语料（generate_real_corpus.py 里 -bf 3）与下面的 ASSERT
// 一起把这件事钉住。
TEST(H264PocTrackerRealEncoderTest, PocOrderMatchesEncoderDisplayOrder) {
    const std::string path = CodecSample("real_x264_bframes_gop.mp4");
    SkipIfMissing(path, "real_x264_bframes_gop.mp4");

    const DemuxedOrderFacts facts = DemuxForDisplayOrder(path);
    ASSERT_TRUE(facts.ok) << "容器里取不到足够的时间戳，无法对账";

    // 被测侧走完整管线：这样连"extradata 里的 SPS/PPS 有没有被喂给 POC tracker"
    // 一起验了 —— MP4 的码流里一个 SPS 都没有（libx264 的 b_repeat_headers=0），
    // 只有 extradata 路径能让它可用。
    const AnalysisResult result = RunFrameOrderAnalysis(path);
    ASSERT_EQ(result.scan_status, videoeye::model::AnalysisStatus::Complete) << result.error_message;
    ASSERT_EQ(result.frame_order_timelines.size(), 1u);
    const FrameOrderTimeline& t = result.frame_order_timelines[0];
    ASSERT_TRUE(t.available) << t.unavailable_reason;

    ASSERT_EQ(t.frames.size(), facts.pts_in_decode_order.size())
        << "逐 slice 数出来的帧数必须与容器里的视频包数一致"
           "（MP4 里一个包就是一帧；对不上说明多 slice/空包被重复计数了）";

    // 样本自检：没有 B 帧的话下面整段对账是恒真的空跑，必须在这里挡住。
    ASSERT_GT(t.b_count, 0) << "语料里没有 B 帧，无法验证显示顺序重排";
    ASSERT_GT(t.reorder_events, 0) << "有 B 帧就必然存在解码顺序里的 POC 回退";

    // ---- 交叉对账一：IDR 个数 ----
    // 左侧来自逐 slice 的 NAL type 5，右侧来自容器的同步样本表（stss）。
    // 两条通道毫无共享，对不上说明其中一边的判据错了。
    const std::size_t container_keys = static_cast<std::size_t>(
        std::count(facts.is_key_in_decode_order.begin(), facts.is_key_in_decode_order.end(), true));
    ASSERT_GT(container_keys, 0u) << "容器没标任何关键帧，无法分段对账";
    EXPECT_EQ(static_cast<std::size_t>(t.idr_count), container_keys)
        << "逐 slice 数出的 IDR 与容器同步样本表标记的关键帧个数不一致";

    const std::vector<std::vector<int64_t>> ours = DisplayOrderPerGop(t);
    const std::vector<std::vector<int64_t>> reference = facts.DecodeIndexInDisplayOrderPerGop();
    ASSERT_EQ(ours.size(), reference.size())
        << "两边的 GOP 分段数不一致（左边：按 NAL type 5；右边：按容器关键帧标记）";
    ASSERT_GT(ours.size(), 1u) << "只有一个 GOP 时 POC 重置这条语义测不到";

    for (std::size_t g = 0; g < ours.size(); ++g) {
        EXPECT_EQ(ours[g], reference[g]) << "第 " << g
                                         << " 个 GOP 内，按 POC 排出来的显示顺序与容器 PTS 顺序不一致 —— "
                                            "POC 重建有误（左边：本实现；右边：编码器写入的显示时间戳）";
        // 参考侧自检：这一段里包顺序 ≠ 显示顺序，这份参考才有区分度
        EXPECT_NE(reference[g], Sequential(reference[g].size()))
            << "第 " << g << " 个 GOP 的 PTS 顺序就是文件顺序，说明这段没有重排，参考无区分度";
        // 段内 POC 必须严格递增（段间重置是允许的，段内并列才是错）
        std::vector<int64_t> pocs;
        for (int64_t decode_index : ours[g])
            pocs.push_back(t.frames[static_cast<std::size_t>(decode_index)].poc);
        for (std::size_t i = 1; i < pocs.size(); ++i)
            EXPECT_LT(pocs[i - 1], pocs[i]) << "第 " << g << " 个 GOP 内 POC 出现并列/倒退";
    }

    // ---- 交叉对账二：重排深度 ----
    // 两侧都能量出"这一段被重排了多深"，口径不同（最大 POC 回退槽位 vs
    // 最大 PTS 回退帧数）所以不要求相等，但都不该是 0。
    int64_t reference_back = 0;
    for (const std::vector<int64_t>& segment : reference)
        reference_back = std::max(reference_back, MaxBackwardStep(segment));
    EXPECT_GT(reference_back, 0) << "参考侧没有向后的跳步，说明 PTS 没有重排";
    EXPECT_GT(t.reorder_depth, 0) << "被测侧没有测到 POC 回退";
    EXPECT_LE(t.reorder_depth, static_cast<int64_t>(t.frames.size())) << "重排深度不可能超过总帧数";
}

// ---------------------------------------------------------------------------
// B3) 真实样本：按 POC 排序后的时间戳必须是等差的（步长 = 1/fps）
// ---------------------------------------------------------------------------
//
// B2 逐项比了"顺序"，这一条比"时间间隔"。两条合起来才说明"显示顺序 + 显示
// 时刻都对"：只比顺序抓不到"两帧被排到了同一个位置"，只比间隔抓不到
// "顺序整体错位一格"。
TEST(H264PocTrackerRealEncoderTest, PocOrderYieldsUniformFrameIntervals) {
    const std::string path = CodecSample("real_x264_bframes_gop.mp4");
    SkipIfMissing(path, "real_x264_bframes_gop.mp4");

    const DemuxedOrderFacts facts = DemuxForDisplayOrder(path);
    ASSERT_TRUE(facts.ok);

    const AnalysisResult result = RunFrameOrderAnalysis(path);
    ASSERT_EQ(result.scan_status, videoeye::model::AnalysisStatus::Complete) << result.error_message;
    ASSERT_EQ(result.frame_order_timelines.size(), 1u);
    const FrameOrderTimeline& t = result.frame_order_timelines[0];
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), facts.pts_in_decode_order.size());

    // 段内按 POC 排好之后，相邻两帧的显示时间差必须恰好是一帧。
    // 只在段内比 —— 跨段时 POC 与时间戳双双重置，差值没有意义。
    std::size_t checked = 0;
    for (const std::vector<int64_t>& segment : DisplayOrderPerGop(t)) {
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const FrameOrderEntry& entry = t.frames[static_cast<std::size_t>(segment[i])];
            ASSERT_TRUE(entry.has_time) << "MP4 的每个包都带时间戳，缺一个说明喂包时丢了";
            if (i == 0)
                continue;
            const FrameOrderEntry& previous = t.frames[static_cast<std::size_t>(segment[i - 1])];
            EXPECT_NEAR(entry.time_seconds - previous.time_seconds, 1.0 / 25.0, 1e-6)
                << "按 POC 排到第 " << i
                << " 位的帧，与前一帧的显示时间差不是 40 ms"
                   "（语料按 25 fps 编码）";
            ++checked;
        }
    }
    EXPECT_GT(checked, 20u) << "只对账了 " << checked << " 个间隔，样本太短，这条用例没有区分度";
}

} // namespace
