// 参数集全程追踪（阶段 1）的正反例回归测试。
//
// 被测的是 ParameterSetTracker：它把"首次配置解析"升级成"配置时间线"。
// 三条产出各自都有正反例：
//   1. parameter_sets —— 参数集登记表（同 id 重复发送不应产生变化事件）
//   2. changes        —— 同 id 内容被替换 / 新增了不同配置的 id
//   3. references     —— slice 引用了从未传输的 PPS（解码必然失败，硬证据）
//
// 测试数据由本文件里的 BitWriter 按 H.264 7.3.2.1.1 逐位生成，
// 期望值来自规范语义（"同 id 不同内容 = 替换"、"引用未传输 = 缺失"），
// 不是把实现的输出抄一遍。

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/ParameterSetTracker.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::ParameterSetTracker;

// ---------------------------------------------------------------------------
// 极简位写入器（只够拼 SPS / slice header）
// ---------------------------------------------------------------------------
class BitWriter {
public:
    void U(int value, int bits) {
        for (int i = bits - 1; i >= 0; --i)
            Bit(((value >> i) & 1) != 0);
    }
    // ue(v)：code_num = value + 1，前导零 = 位宽-1，然后 1，然后剩余位
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
    // rbsp_trailing_bits：一个 1 然后补 0 到字节对齐
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

// 生成一份 Baseline profile SPS（4:2:0 8 bit，无 VUI）。
// id / 宽 / 高可指定 —— 这正是"同 id 换配置"与"换 id 换配置"两种场景要用的。
//
// 注意 H.264 的 pic_height_in_map_units 以 16 像素为单位，1080 不是 16 的倍数：
// 真实码流会先写 1088 再用 frame_cropping 裁到 1080。这里照规范生成 crop，
// 否则解析出来的高度会是 1072/1088 这种"凭空多/少 8 像素"的值。
std::vector<uint8_t> MakeSps(int id, int width, int height) {
    const int mb_w = (width + 15) / 16;
    const int mb_h = (height + 15) / 16;
    // 4:2:0 + frame_mbs_only_flag=1 → 裁剪单位在垂直方向是 2 像素
    const int crop_bottom = (mb_h * 16 - height) / 2;
    const int crop_right = (mb_w * 16 - width) / 2;

    BitWriter w;
    w.U(66, 8); // profile_idc = Baseline（无 chroma_format_idc 等 High 扩展）
    w.U(0, 8);  // constraint_set_flags
    w.U(30, 8); // level_idc = 3.0
    w.Ue(static_cast<uint32_t>(id));
    w.Ue(0);   // log2_max_frame_num_minus4
    w.Ue(0);   // pic_order_cnt_type
    w.Ue(0);   // log2_max_pic_order_cnt_lsb_minus4
    w.Ue(1);   // max_num_ref_frames
    w.U(0, 1); // gaps_in_frame_num_value_allowed_flag
    w.Ue(static_cast<uint32_t>(mb_w - 1));
    w.Ue(static_cast<uint32_t>(mb_h - 1));
    w.U(1, 1); // frame_mbs_only_flag
    w.U(1, 1); // direct_8x8_inference_flag
    if (crop_right > 0 || crop_bottom > 0) {
        w.U(1, 1); // frame_cropping_flag
        w.Ue(0);   // frame_crop_left_offset
        w.Ue(static_cast<uint32_t>(crop_right));
        w.Ue(0); // frame_crop_top_offset
        w.Ue(static_cast<uint32_t>(crop_bottom));
    } else {
        w.U(0, 1); // frame_cropping_flag
    }
    w.U(0, 1); // vui_parameters_present_flag
    w.Trailing();
    return w.Bytes();
}

// 一份只包含 pps_id / sps_id 的 PPS（后面字段填 0，解析器只需前两个 ue(v)）
std::vector<uint8_t> MakePps(int pps_id, int sps_id) {
    BitWriter w;
    w.Ue(static_cast<uint32_t>(pps_id));
    w.Ue(static_cast<uint32_t>(sps_id));
    // 补齐后续最小字段，避免解析器因截断拒绝
    w.Ue(0);   // entropy_coding_mode_flag + bottom_field_pic_order_in_frame_present_flag（打包成两位）
    w.Ue(0);   // num_slice_groups_minus1
    w.Ue(0);   // num_ref_idx_l0_default_active_minus1
    w.Ue(0);   // num_ref_idx_l1_default_active_minus1
    w.U(0, 1); // weighted_pred_flag
    w.U(0, 2); // weighted_bipred_idc
    w.Trailing();
    return w.Bytes();
}

// 一个 slice header：first_mb_in_slice / slice_type(I=2) / pps_id
std::vector<uint8_t> MakeSlice(int first_mb, int pps_id) {
    BitWriter w;
    w.Ue(static_cast<uint32_t>(first_mb));
    w.Ue(2); // slice_type = I
    w.Ue(static_cast<uint32_t>(pps_id));
    w.Trailing();
    return w.Bytes();
}

// Annex B 封装：00 00 00 01 + NAL header + RBSP
std::vector<uint8_t> AnnexB(int nal_type, int nal_ref_idc, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out = {0x00, 0x00, 0x00, 0x01};
    out.push_back(static_cast<uint8_t>((nal_ref_idc << 5) | (nal_type & 0x1F)));
    out.insert(out.end(), rbsp.begin(), rbsp.end());
    return out;
}

constexpr int kH264 = AV_CODEC_ID_H264;

} // namespace

// ---------------------------------------------------------------------------
// 1) 同一份参数集重复发送 —— 绝不能算"变化"
// ---------------------------------------------------------------------------
//
// IDR 之前重复发送同一份 SPS/PPS 是编码器的正常行为。如果这里误报，
// 任何普通文件都会出现成百上千条"配置变化"，功能直接失去意义。
TEST(ParameterSetTrackerTest, RepeatedIdenticalSpsIsNotAChange) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);
    const std::vector<uint8_t> packet = AnnexB(7, 3, MakeSps(0, 1280, 720));

    for (int i = 0; i < 3; ++i)
        tracker.Feed(packet.data(), packet.size(), i * 1.0, i, i * 1000);
    tracker.Finish();

    const auto& timeline = tracker.result();
    EXPECT_TRUE(timeline.analyzed);
    EXPECT_EQ(timeline.changes.size(), 0u) << "重复发送同一份 SPS 不是配置变化";
    ASSERT_EQ(timeline.parameter_sets.size(), 1u);
    EXPECT_EQ(timeline.parameter_sets[0].occurrences, 3);
    EXPECT_EQ(timeline.parameter_sets[0].replacements, 0);
    EXPECT_EQ(timeline.parameter_sets[0].summary, "1280x720 / 8 bit / 4:2:0");
}

// ---------------------------------------------------------------------------
// 2) 同一 id 的内容被替换 —— 最危险的一类，必须报 replaced_same_id
// ---------------------------------------------------------------------------
TEST(ParameterSetTrackerTest, SameIdDifferentContentIsFlaggedAsReplacement) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);

    const std::vector<uint8_t> first = AnnexB(7, 3, MakeSps(0, 1280, 720));
    tracker.Feed(first.data(), first.size(), 5.0, 10, 0x1000);

    const std::vector<uint8_t> second = AnnexB(7, 3, MakeSps(0, 1920, 1080));
    tracker.Feed(second.data(), second.size(), 755.24, 15320, 0x1A2B3C);
    tracker.Finish();

    const auto& timeline = tracker.result();
    ASSERT_EQ(timeline.changes.size(), 1u);
    const auto& change = timeline.changes[0];
    EXPECT_TRUE(change.replaced_same_id);
    EXPECT_EQ(change.parameter_set_id, 0);
    EXPECT_EQ(change.old_config, "1280x720 / 8 bit / 4:2:0");
    EXPECT_EQ(change.new_config, "1920x1080 / 8 bit / 4:2:0");
    EXPECT_NE(change.changes.find("1280x720"), std::string::npos);
    EXPECT_NE(change.changes.find("1920x1080"), std::string::npos);
    // 可定位：时间 / 包号 / 文件偏移三项都要有
    EXPECT_TRUE(change.has_time);
    EXPECT_DOUBLE_EQ(change.time_seconds, 755.24);
    EXPECT_EQ(change.packet_index, 15320);
    EXPECT_EQ(change.file_offset, 0x1A2B3C);
    // 文本形态要能直接贴进报告
    const std::string text = change.ToString();
    EXPECT_NE(text.find("00:12:35.240"), std::string::npos) << text;
    EXPECT_NE(text.find("0x1A2B3C"), std::string::npos) << text;

    ASSERT_EQ(timeline.parameter_sets.size(), 1u);
    EXPECT_EQ(timeline.parameter_sets[0].replacements, 1);
}

// ---------------------------------------------------------------------------
// 3) 新增一份不同 id 的 SPS —— 可能只是码率自适应，不能当成"替换"
// ---------------------------------------------------------------------------
TEST(ParameterSetTrackerTest, NewSpsIdWithDifferentConfigIsNotAReplacement) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);

    const std::vector<uint8_t> a = AnnexB(7, 3, MakeSps(0, 1280, 720));
    tracker.Feed(a.data(), a.size(), 0.0, 0, 0);
    const std::vector<uint8_t> b = AnnexB(7, 3, MakeSps(1, 1920, 1080));
    tracker.Feed(b.data(), b.size(), 10.0, 100, 5000);
    tracker.Finish();

    const auto& timeline = tracker.result();
    ASSERT_EQ(timeline.changes.size(), 1u);
    EXPECT_FALSE(timeline.changes[0].replaced_same_id) << "换 id 是新增配置，不是同 id 替换";
    EXPECT_EQ(timeline.changes[0].parameter_set_id, 1);
    EXPECT_EQ(timeline.parameter_sets.size(), 2u);
}

// 反例：新增同 id、同内容的 SPS（编码器换了 profile 但可见参数一样）
// 摘要不同 → 必须报变化；但 visible 配置相同，changes 文本要说明"字节变了但可见参数未变"
TEST(ParameterSetTrackerTest, SameIdSameVisibleConfigStillReportsByteChange) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);

    const std::vector<uint8_t> a = AnnexB(7, 3, MakeSps(0, 1280, 720));
    tracker.Feed(a.data(), a.size(), 0.0, 0, 0);
    const std::vector<uint8_t> b = AnnexB(7, 3, MakeSps(0, 1280, 720));
    // 同 id 同尺寸，但 payload 不同：改 level_idc（索引 7 = 起始码 4 + NAL 头 1 + profile 1 + constraint 1），
    // 可见配置（宽高/位深/色度格式）不变 —— 这正是"字节变了但参数没变"的场景
    std::vector<uint8_t> mutated = b;
    mutated[7] = 31; // level_idc 3.0 → 3.1
    tracker.Feed(mutated.data(), mutated.size(), 1.0, 1, 100);
    tracker.Finish();

    const auto& timeline = tracker.result();
    ASSERT_EQ(timeline.changes.size(), 1u);
    EXPECT_TRUE(timeline.changes[0].replaced_same_id);
}

// ---------------------------------------------------------------------------
// 4) 引用从未传输的 PPS —— 硬证据
// ---------------------------------------------------------------------------
TEST(ParameterSetTrackerTest, SliceReferencingTransmittedPpsIsNotMissing) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);

    const std::vector<uint8_t> sps = AnnexB(7, 3, MakeSps(0, 1280, 720));
    const std::vector<uint8_t> pps = AnnexB(8, 3, MakePps(1, 0));
    const std::vector<uint8_t> slice = AnnexB(5, 3, MakeSlice(0, 1));
    tracker.Feed(sps.data(), sps.size(), 0.0, 0, 0);
    tracker.Feed(pps.data(), pps.size(), 0.0, 1, 100);
    tracker.Feed(slice.data(), slice.size(), 0.0, 2, 200);
    tracker.Finish();

    const auto& timeline = tracker.result();
    EXPECT_EQ(timeline.missing_reference_count, 0);
    ASSERT_FALSE(timeline.references.empty());
    const auto& ref = timeline.references[0];
    EXPECT_EQ(ref.id, 1);
    EXPECT_TRUE(ref.transmitted);
    EXPECT_FALSE(ref.referenced_before_transmit);
    EXPECT_EQ(ref.frame_count, 1);
}

TEST(ParameterSetTrackerTest, SliceReferencingUntransmittedPpsIsMissing) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);

    // 只给 SPS 与 slice（引用 pps_id=1），PPS 从未出现
    const std::vector<uint8_t> sps = AnnexB(7, 3, MakeSps(0, 1280, 720));
    const std::vector<uint8_t> slice = AnnexB(5, 3, MakeSlice(0, 1));
    tracker.Feed(sps.data(), sps.size(), 0.0, 0, 0);
    tracker.Feed(slice.data(), slice.size(), 0.0, 1, 100);
    tracker.Finish();

    const auto& timeline = tracker.result();
    EXPECT_EQ(timeline.missing_reference_count, 1);
    ASSERT_FALSE(timeline.references.empty());
    EXPECT_FALSE(timeline.references[0].transmitted);
    EXPECT_FALSE(timeline.notes.empty()) << "缺失引用必须在 notes 里说明，不能只留一个计数";
}

// 一帧多 slice：只有 first_mb_in_slice == 0 的那个算"一帧"，否则帧数会被放大
TEST(ParameterSetTrackerTest, MultiSlicePictureCountedAsOneFrame) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);

    const std::vector<uint8_t> pps = AnnexB(8, 3, MakePps(0, 0));
    const std::vector<uint8_t> slice0 = AnnexB(5, 3, MakeSlice(0, 0));
    const std::vector<uint8_t> slice1 = AnnexB(5, 3, MakeSlice(5, 0));
    tracker.Feed(pps.data(), pps.size(), 0.0, 0, 0);
    tracker.Feed(slice0.data(), slice0.size(), 0.0, 1, 100);
    tracker.Feed(slice1.data(), slice1.size(), 0.0, 1, 100);
    tracker.Finish();

    const auto& timeline = tracker.result();
    ASSERT_FALSE(timeline.references.empty());
    EXPECT_EQ(timeline.references[0].slice_count, 2);
    EXPECT_EQ(timeline.references[0].frame_count, 1);
}

// ---------------------------------------------------------------------------
// 5) 封装形态：MP4 的长度前缀（无起始码）也必须能追踪
// ---------------------------------------------------------------------------
TEST(ParameterSetTrackerTest, LengthPrefixedPacketsAreParsed) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);
    tracker.SetFraming(ParameterSetTracker::Framing::LengthPrefixed, 4);

    // [4 字节长度][NAL header][RBSP]
    const std::vector<uint8_t> rbsp = MakeSps(0, 1280, 720);
    std::vector<uint8_t> packet;
    const uint32_t nal_len = static_cast<uint32_t>(rbsp.size() + 1);
    packet.push_back(static_cast<uint8_t>((nal_len >> 24) & 0xFF));
    packet.push_back(static_cast<uint8_t>((nal_len >> 16) & 0xFF));
    packet.push_back(static_cast<uint8_t>((nal_len >> 8) & 0xFF));
    packet.push_back(static_cast<uint8_t>(nal_len & 0xFF));
    packet.push_back(0x67); // nal_ref_idc=3, type=7(SPS)
    packet.insert(packet.end(), rbsp.begin(), rbsp.end());

    tracker.Feed(packet.data(), packet.size(), 0.0, 0, 0);
    tracker.Finish();

    const auto& timeline = tracker.result();
    ASSERT_EQ(timeline.parameter_sets.size(), 1u) << "长度前缀封装（MP4 媒体包）必须能识别";
    EXPECT_EQ(timeline.parameter_sets[0].summary, "1280x720 / 8 bit / 4:2:0");
}

// ---------------------------------------------------------------------------
// 6) 范围与降级：编码不支持 / 上限截断
// ---------------------------------------------------------------------------
TEST(ParameterSetTrackerTest, UnsupportedCodecIsDisabledWithReason) {
    ParameterSetTracker tracker;
    tracker.Reset(AV_CODEC_ID_VP9, 0);
    EXPECT_FALSE(tracker.enabled());
    EXPECT_FALSE(tracker.disabled_reason().empty());
}

TEST(ParameterSetTrackerTest, ChangeLimitMarksTruncated) {
    ParameterSetTracker::Options options;
    options.max_changes = 2;
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0, options);

    // 连续 5 次同 id 换配置 → 只有前 2 条被记录，其余置 truncated
    const int widths[5] = {320, 640, 960, 1280, 1920};
    for (int i = 0; i < 5; ++i) {
        const std::vector<uint8_t> packet = AnnexB(7, 3, MakeSps(0, widths[i], 720));
        tracker.Feed(packet.data(), packet.size(), i, i, i * 100);
    }
    tracker.Finish();

    const auto& timeline = tracker.result();
    EXPECT_EQ(timeline.changes.size(), 2u);
    EXPECT_TRUE(timeline.truncated) << "到上限必须显式置 truncated，否则读报告的人会把"
                                       "「只记录了 2 次」当成「只有 2 次」";
}

// 反例：空包 / 非法指针不得崩溃（扫描器可能喂进畸形数据）
TEST(ParameterSetTrackerTest, EmptyAndNullInputIsSafe) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);
    tracker.Feed(nullptr, 0, 0.0, 0, 0);
    const uint8_t junk[3] = {0x12, 0x34, 0x56};
    tracker.Feed(junk, sizeof(junk), 0.0, 1, 0);
    tracker.Finish();
    EXPECT_TRUE(tracker.result().changes.empty());
    EXPECT_EQ(tracker.result().missing_reference_count, 0);
}

// Finish 必须幂等：Pipeline 里 BeforeClose 与析构都可能调用
TEST(ParameterSetTrackerTest, FinishIsIdempotent) {
    ParameterSetTracker tracker;
    tracker.Reset(kH264, 0);
    const std::vector<uint8_t> slice = AnnexB(5, 3, MakeSlice(0, 0));
    tracker.Feed(slice.data(), slice.size(), 0.0, 0, 0);
    tracker.Finish();
    const int64_t first = tracker.result().missing_reference_count;
    tracker.Finish();
    EXPECT_EQ(tracker.result().missing_reference_count, first) << "重复 Finish 不能把缺失计数翻倍";
}
