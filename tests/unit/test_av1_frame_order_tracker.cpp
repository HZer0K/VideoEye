// AV1 帧级显示顺序追踪的单元测试。
//
// 三组互补：
//   A) 合成位流 —— 按 AV1 规范 5.9.2 / 5.5.1 的字段顺序手工造序列头与帧头，
//      钉住位布局（尤其 showable_frame 只在 show_frame == 0 时存在这一位）。
//   B) order_hint 的环形语义 —— 回到 0 不等于"回退 255 帧"。这是本编码
//      最容易写错、且错了不会报错只会给出荒谬数字的地方。
//   C) 外壳分派 / 复位 / 上限 / 不支持路径。
//
// ❗ AV1 与 VP9 的关键差别：AV1 **有** order_hint（真字段），所以顺序值是从
//   码流里读出来的；只有 enable_order_hint == 0 或 reduced_still_picture_header
//   时才退化回显示事件序（order_source == "display_rank"）。

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/Av1BitstreamParser.h"
#include "core/analysis/codec/Av1FrameOrderTracker.h"
#include "core/analysis/codec/FrameOrderTracker.h"
#include "core/domain/model/FrameOrderInfo.h"
#include "core/media/codec/ExtradataParser.h"
#include "tests/support/FrameOrderTestSupport.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::Av1BitstreamParser;
using videoeye::Av1FrameOrderTracker;
using videoeye::FrameOrderTracker;
using videoeye::model::FrameOrderTimeline;

// MSB-first 位写入器（AV1 的 f(n) 是高位在前）
class BitWriter {
public:
    void Bit(uint32_t value) {
        if (bit_count_ == 0)
            bytes_.push_back(0);
        if (value != 0)
            bytes_.back() |= static_cast<uint8_t>(1u << (7 - bit_count_));
        bit_count_ = (bit_count_ + 1) % 8;
    }
    void Bits(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i)
            Bit((value >> i) & 1u);
    }
    std::vector<uint8_t> Bytes() const {
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    int bit_count_ = 0;
};

// OBU 头 + leb128 长度（容器里的 OBU 都带 obu_has_size_field）
std::vector<uint8_t> MakeObu(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((type << 3) | 0x02)); // forbidden=0, ext=0, has_size=1, reserved=0

    size_t size = payload.size();
    do {
        uint8_t byte = static_cast<uint8_t>(size & 0x7F);
        size >>= 7;
        if (size != 0)
            byte |= 0x80;
        out.push_back(byte);
    } while (size != 0);

    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> Concat(const std::vector<std::vector<uint8_t>>& parts) {
    std::vector<uint8_t> out;
    for (const auto& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// color_config()：profile 0 / 8bit / 4:2:0 / 无色描述（走最普通的那条分支）
void WriteColorConfig(BitWriter& w) {
    w.Bit(0);     // high_bitdepth
    w.Bit(0);     // mono_chrome（profile 0 才有这一位）
    w.Bit(0);     // color_description_present_flag
    w.Bit(0);     // color_range
    w.Bits(0, 2); // chroma_sample_position
    w.Bit(0);     // separate_uv_delta_q
}

struct SeqOptions {
    bool reduced_still_picture_header = false;
    bool enable_order_hint = true;
    int order_hint_bits_minus_1 = 2; // bits = 3
    bool frame_id_numbers = false;
};

std::vector<uint8_t> SequenceHeaderObu(const SeqOptions& opt) {
    BitWriter w;
    w.Bits(0, 3); // seq_profile = 0
    w.Bit(0);     // still_picture
    w.Bit(opt.reduced_still_picture_header ? 1 : 0);

    if (opt.reduced_still_picture_header) {
        w.Bits(8, 5); // seq_level_idx_0
    } else {
        w.Bit(0);      // timing_info_present_flag
        w.Bit(0);      // initial_display_delay_present_flag
        w.Bits(0, 5);  // operating_points_cnt_minus_1 = 0 → 1 个 operating point
        w.Bits(0, 12); // operating_point_idc
        w.Bits(8, 5);  // seq_level_idx_0 = 8（major 4 → 后面必须还有 seq_tier 一位）
        w.Bit(0);      // seq_tier
    }

    w.Bits(7, 4);  // frame_width_bits_minus_1  → 8 位
    w.Bits(7, 4);  // frame_height_bits_minus_1 → 8 位
    w.Bits(63, 8); // max_frame_width_minus_1
    w.Bits(63, 8); // max_frame_height_minus_1

    if (!opt.reduced_still_picture_header) {
        w.Bit(opt.frame_id_numbers ? 1 : 0);
        if (opt.frame_id_numbers) {
            w.Bits(0, 4); // delta_frame_id_length_minus_2
            w.Bits(0, 3); // additional_frame_id_length_minus_1
        }
    }

    w.Bit(0); // use_128x128_superblock
    w.Bit(0); // enable_filter_intra
    w.Bit(0); // enable_intra_edge_filter

    if (!opt.reduced_still_picture_header) {
        w.Bit(0); // enable_interintra_compound
        w.Bit(0); // enable_masked_compound
        w.Bit(0); // enable_warped_motion
        w.Bit(0); // enable_dual_filter
        w.Bit(opt.enable_order_hint ? 1 : 0);
        if (opt.enable_order_hint) {
            w.Bit(0); // enable_jnt_comp
            w.Bit(0); // enable_ref_frame_mvs
        }
        w.Bit(1); // seq_choose_screen_content_tools = 1 → SELECT_SCREEN_CONTENT_TOOLS
        w.Bit(1); // seq_choose_integer_mv = 1 → SELECT_INTEGER_MV
        if (opt.enable_order_hint)
            w.Bits(static_cast<uint32_t>(opt.order_hint_bits_minus_1), 3);
    }

    w.Bit(0); // enable_superres
    w.Bit(0); // enable_cdef
    w.Bit(0); // enable_restoration

    WriteColorConfig(w);

    w.Bit(0); // film_grain_params_present

    return MakeObu(1, w.Bytes()); // OBU_SEQUENCE_HEADER
}

// 只为测"解码器模型"这条不支持路径：timing_info 打开 + decoder_model_info 打开。
std::vector<uint8_t> SequenceHeaderWithDecoderModel() {
    BitWriter w;
    w.Bits(0, 3);   // seq_profile
    w.Bit(0);       // still_picture
    w.Bit(0);       // reduced_still_picture_header
    w.Bit(1);       // timing_info_present_flag
    w.Bits(1, 32);  // num_units_in_tick
    w.Bits(30, 32); // time_scale
    w.Bit(0);       // equal_picture_interval
    w.Bit(1);       // decoder_model_info_present_flag
    w.Bits(0, 5);   // decoder_buffer_delay_length_minus_1
    w.Bits(0, 32);  // num_units_in_decoding_tick
    w.Bits(0, 5);   // buffer_removal_time_length_minus_1
    w.Bits(0, 5);   // frame_presentation_time_length_minus_1
    w.Bit(0);       // initial_display_delay_present_flag
    w.Bits(0, 5);   // operating_points_cnt_minus_1
    w.Bits(0, 12);  // operating_point_idc
    w.Bits(8, 5);   // seq_level_idx
    w.Bit(0);       // decoder_model_param_present_flag
    w.Bit(0);       // display_model_param_present_flag
    w.Bits(7, 4);
    w.Bits(7, 4);
    w.Bits(63, 8);
    w.Bits(63, 8);
    w.Bit(0); // frame_id_numbers_present_flag
    w.Bit(0);
    w.Bit(0);
    w.Bit(0);
    w.Bit(0);     // enable_interintra_compound
    w.Bit(0);     // enable_masked_compound
    w.Bit(0);     // enable_warped_motion
    w.Bit(0);     // enable_dual_filter
    w.Bit(1);     // enable_order_hint
    w.Bit(0);     // enable_jnt_comp
    w.Bit(0);     // enable_ref_frame_mvs
    w.Bit(1);     // seq_choose_screen_content_tools
    w.Bit(1);     // seq_choose_integer_mv
    w.Bits(2, 3); // order_hint_bits_minus_1
    w.Bit(0);
    w.Bit(0);
    w.Bit(0);
    WriteColorConfig(w);
    w.Bit(0);
    return MakeObu(1, w.Bytes());
}

// 关键帧（frame_type = 0）。show_frame 恒为 1 的常见情形。
std::vector<uint8_t> KeyFrameObu(int order_hint, bool show_frame = true) {
    BitWriter w;
    w.Bit(0);     // show_existing_frame
    w.Bits(0, 2); // frame_type = KEY_FRAME
    w.Bit(show_frame ? 1 : 0);
    if (!show_frame)
        w.Bit(1); // showable_frame
    // KEY && show_frame：error_resilient_mode 恒 1，不占位
    w.Bit(0); // disable_cdf_update
    w.Bit(0); // allow_screen_content_tools = 0（seq 里是 SELECT，所以要读这一位）
    w.Bit(0); // frame_size_override_flag
    w.Bits(static_cast<uint32_t>(order_hint), 3);
    // KEY：refresh_frame_flags 恒 0xFF（show_frame 时），intra_only 不读 primary_ref_frame
    return MakeObu(6, w.Bytes());
}

// 帧间帧（frame_type = 1）。refresh_flags 的第 i 位 = 写进第 i 号参考槽。
std::vector<uint8_t> InterFrameObu(int order_hint, bool show_frame, int refresh_flags) {
    BitWriter w;
    w.Bit(0);     // show_existing_frame
    w.Bits(1, 2); // frame_type = INTER_FRAME
    w.Bit(show_frame ? 1 : 0);
    if (!show_frame)
        w.Bit(1); // showable_frame
    w.Bit(0);     // error_resilient_mode
    w.Bit(0);     // disable_cdf_update
    w.Bit(0);     // allow_screen_content_tools
    w.Bit(0);     // frame_size_override_flag
    w.Bits(static_cast<uint32_t>(order_hint), 3);
    w.Bits(0, 3); // primary_ref_frame（!error_resilient && 非 intra_only 才有）
    w.Bits(static_cast<uint32_t>(refresh_flags), 8);
    return MakeObu(6, w.Bytes());
}

// show_existing_frame：整段只有 8 位（1 位标志 + 3 位槽号）。
std::vector<uint8_t> ShowExistingObu(int slot) {
    BitWriter w;
    w.Bit(1);
    w.Bits(static_cast<uint32_t>(slot), 3);
    return MakeObu(6, w.Bytes());
}

// 静图序列头下的帧头：没有 show_existing / show_frame / order_hint。
std::vector<uint8_t> ReducedStillPictureFrameObu() {
    BitWriter w;
    w.Bit(0); // disable_cdf_update
    w.Bit(0); // allow_screen_content_tools
    return MakeObu(6, w.Bytes());
}

struct Feed {
    Av1FrameOrderTracker tracker;
    explicit Feed(int stream_index = 0) {
        tracker.Reset(stream_index);
    }
    void Obu(const std::vector<uint8_t>& data, int64_t packet_index = 0) {
        tracker.Feed(data.data(), data.size(), true, 0.0, packet_index);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// A) 位布局 / 序列头
// ---------------------------------------------------------------------------

TEST(Av1FrameOrderTracker, SequenceHeaderIsParsedAndRecorded) {
    SeqOptions opt;
    const auto seq = SequenceHeaderObu(opt);

    // 先钉住"生成器与解析器对同一份位流理解一致"，否则下面的用例全在测空气。
    // 这里走的是追踪器真实使用的那条路（拆 OBU → 解析 OBU），
    // 不是 ParseSequenceHeader（那条要先过 av1C 嗅探，裸 OBU 流会被判成未知格式）。
    const auto units = videoeye::ExtradataParser::ExtractObuUnits(seq.data(), seq.size());
    ASSERT_EQ(units.size(), 1u);
    const auto parsed = Av1BitstreamParser::ParseFromObuUnit(units[0]);
    ASSERT_TRUE(parsed.present);
    EXPECT_TRUE(parsed.enable_order_hint);
    EXPECT_EQ(parsed.order_hint_bits_minus_1, 2);
    EXPECT_EQ(parsed.level, 8) << "seq_level_idx 的 5 位里还藏着 tier 的分界，写错了这里会露";
    EXPECT_FALSE(parsed.frame_id_numbers_present_flag);
    EXPECT_EQ(parsed.frame_id_length, 0);

    // 序列头与关键帧同包（真实码流就是这样）：必须按 OBU 遍历顺序先见到序列头。
    Feed feed;
    feed.Obu(Concat({seq, KeyFrameObu(0)}));
    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.frames_observed, 1);
}

TEST(Av1FrameOrderTracker, ExtradataWithBareObuSequenceHeaderIsAccepted) {
    // Matroska 的 CodecPrivate 可能是一串裸 OBU：av1C 嗅探认不出来，
    // 但"里面真的有 sequence_header_obu"这件事足够当判据。
    const auto seq = SequenceHeaderObu(SeqOptions{});
    FrameOrderTracker tracker;
    tracker.Reset(AV_CODEC_ID_AV1, 0);
    ASSERT_TRUE(tracker.enabled());
    EXPECT_TRUE(tracker.FeedExtradata(seq.data(), seq.size()));

    const auto frame = KeyFrameObu(0);
    tracker.Feed(frame.data(), frame.size(), true, 0.0, 0);
    const FrameOrderTimeline& t = tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.frames_observed, 1);
}

TEST(Av1FrameOrderTracker, OrderSourceIsOrderHint) {
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    const auto frame = KeyFrameObu(0);
    EXPECT_EQ(feed.tracker.Feed(frame.data(), frame.size(), true, 0.0, 0), 1);

    const FrameOrderTimeline& t = feed.tracker.Finish();
    EXPECT_TRUE(t.available);
    EXPECT_EQ(t.codec, "av1");
    EXPECT_EQ(t.order_source, "order_hint");
    EXPECT_EQ(t.poc_type, -1) << "AV1 没有 pic_order_cnt_type，不能记 0 假装有";
    EXPECT_EQ(t.frames_observed, 1);
    ASSERT_EQ(t.frames.size(), 1u);
    EXPECT_EQ(t.frames[0].poc, 0);
    EXPECT_TRUE(t.frames[0].idr);
}

// ---------------------------------------------------------------------------
// B) order_hint 的顺序语义
// ---------------------------------------------------------------------------

TEST(Av1FrameOrderTracker, DecodeOrderEqualsDisplayOrder) {
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(0));
    for (int i = 1; i < 5; ++i)
        feed.Obu(InterFrameObu(i, true, 0x01));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.frames_observed, 5);
    EXPECT_EQ(t.reorder_events, 0);
    EXPECT_EQ(t.reorder_depth, 0);
    EXPECT_EQ(t.gop_count, 1);
}

TEST(Av1FrameOrderTracker, DisplayOrderReorderIsDetected) {
    // 解码顺序 0 → 2 → 1 → 3：第 3 帧的显示时间插在第 2 帧之前（一个 B 帧结构）。
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(0));
    feed.Obu(InterFrameObu(2, true, 0x01));
    feed.Obu(InterFrameObu(1, true, 0x01));
    feed.Obu(InterFrameObu(3, true, 0x01));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.reorder_events, 1);
    EXPECT_EQ(t.reorder_depth, 1);
    EXPECT_EQ(t.duplicate_poc_count, 0);
    EXPECT_EQ(t.poc_gap_slots, 0);
}

TEST(Av1FrameOrderTracker, OrderHintWrapIsNotAReorder) {
    // ❗ 本用例是这条实现的命门：order_hint 是模 8 的环形量。
    // 解码顺序 6 → 7 → 0 → 1 是**完全正常**的显示顺序前进，
    // 但如果拿原始值直接比大小，0 < 7 会被当成"回退 7 帧"。
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(6));
    feed.Obu(InterFrameObu(7, true, 0x01));
    feed.Obu(InterFrameObu(0, true, 0x01));
    feed.Obu(InterFrameObu(1, true, 0x01));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.reorder_events, 0) << "order_hint 回绕被当成了重排 —— 没做 get_relative_dist 解环";
    EXPECT_EQ(t.reorder_depth, 0);
    ASSERT_EQ(t.frames.size(), 4u);
    // 解环后的顺序值必须是单调的 6,7,8,9（而不是 6,7,0,1）
    EXPECT_EQ(t.frames[0].poc, 6);
    EXPECT_EQ(t.frames[1].poc, 7);
    EXPECT_EQ(t.frames[2].poc, 8);
    EXPECT_EQ(t.frames[3].poc, 9);
}

TEST(Av1FrameOrderTracker, WrapAroundWithRealReorderStillCounted) {
    // 回绕之后真的往回插一帧：7 → 0(=8) → 9(=1) → 8(=0 回退)
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(6));
    feed.Obu(InterFrameObu(7, true, 0x01));
    feed.Obu(InterFrameObu(1, true, 0x01));
    feed.Obu(InterFrameObu(0, true, 0x01));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.reorder_events, 1);
    EXPECT_EQ(t.reorder_depth, 1);
}

TEST(Av1FrameOrderTracker, ShowExistingRedisplaysSlotAndCountsDuplicate) {
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(0));
    feed.Obu(InterFrameObu(1, true, 0x01)); // 写进 0 号槽，可见
    feed.Obu(ShowExistingObu(0));           // 同一画面再放一次 → 出现两次

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.frames_observed, 2) << "show_existing_frame 不是编码帧，不能计入编码帧数";
    ASSERT_EQ(t.frames.size(), 3u) << "但它是**一次显示**：明细按显示事件落，同一帧出现两次";
    EXPECT_EQ(t.frames[1].poc, 1);
    EXPECT_EQ(t.frames[2].poc, 1) << "重显的顺序值是回查编码帧得到的，不是按事件序号重新编号";
    EXPECT_EQ(t.duplicate_poc_count, 1) << "统计层靠重复的顺序值发现它，不该再手工补一遍";
    EXPECT_EQ(t.reorder_events, 0) << "同一帧连放两次不是重排";
    EXPECT_EQ(t.gop_count, 1);

    // ❗ 这里刻意重显**帧间帧**而不是关键帧：统计层把 irap 当 GOP 边界，
    // 重显一个关键帧会开出一段新 GOP，段内只剩一个 POC，重复数就恒为 0 ——
    // 那样测到的是"边界判定"，不是本用例想钉的"重复检测"。
    // 真实码流里被 show_existing 调出来的也正是 ARF（隐藏的帧间帧）。
}

TEST(Av1FrameOrderTracker, HiddenFrameIsNotDisplayedUnlessOverlayShowsIt) {
    // 隐藏帧（alt-ref）自己从不输出。没有 show_existing 把它调出来时，
    // 它算进"扫到的编码帧"，但**不**是一幅显示过的画面 —— 这一点与 VP9 一致，
    // 与 H.264/HEVC（每帧都要显示）不同。
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(0));
    feed.Obu(InterFrameObu(2, false, 0x01)); // 隐藏，且没人 show_existing 它
    feed.Obu(InterFrameObu(1, true, 0x02));  // 可见

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.frames_observed, 3) << "编码了三帧";
    EXPECT_EQ(t.frames.size(), 2u) << "只显示了两帧";
    EXPECT_EQ(t.reorder_events, 0);
}

TEST(Av1FrameOrderTracker, ShowExistingRevealsHiddenFrameAtItsOwnOrder) {
    // 真实 libaom 的结构：隐藏帧(order_hint=2) 先编码，稍后由 show_existing
    // 把它调出来显示。顺序值必须取**它自己的** order_hint，而不是"第几次显示"。
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(0));
    feed.Obu(InterFrameObu(2, false, 0x01)); // 隐藏，写进 0 号槽
    feed.Obu(InterFrameObu(1, true, 0x02));  // 可见，写进 1 号槽，显示序在前
    feed.Obu(ShowExistingObu(0));            // 把 0 号槽的隐藏帧调出来 → 它的显示序是 2

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    ASSERT_EQ(t.frames.size(), 3u);
    EXPECT_EQ(t.frames[0].poc, 0);
    EXPECT_EQ(t.frames[1].poc, 1);
    EXPECT_EQ(t.frames[2].poc, 2) << "显示事件的顺序值必须回查编码帧，不能按事件序号重新编号";
    EXPECT_EQ(t.reorder_events, 0);
    EXPECT_EQ(t.duplicate_poc_count, 0);
}

TEST(Av1FrameOrderTracker, ShowExistingUnresolvedSlotIsTolerated) {
    // 码流从中间开始时，show_existing 指到的槽还没有帧 —— 不能崩，也不能算一帧。
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(ShowExistingObu(5));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_FALSE(t.unavailable_reason.empty()) << "不可用时必须写清原因，不能留空让人猜";
}

TEST(Av1FrameOrderTracker, GopBoundariesFollowKeyFrames) {
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(0));
    feed.Obu(InterFrameObu(1, true, 0x01));
    feed.Obu(InterFrameObu(2, true, 0x01));
    feed.Obu(KeyFrameObu(0)); // 新 GOP：order_hint 重新从 0 起算
    feed.Obu(InterFrameObu(1, true, 0x01));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.gop_count, 2);
    EXPECT_EQ(t.idr_count, 2);
    EXPECT_EQ(t.reorder_events, 0) << "关键帧把顺序值重置回 0，不是重排";
}

// ---------------------------------------------------------------------------
// C) 退化路径 / 不支持 / 边界
// ---------------------------------------------------------------------------

TEST(Av1FrameOrderTracker, NoOrderHintFallsBackToDisplayRank) {
    SeqOptions opt;
    opt.enable_order_hint = false;
    Feed feed;
    feed.Obu(SequenceHeaderObu(opt));
    // 没有 order_hint 时帧头里那几位不存在，KeyFrameObu 写的是 hint 位，
    // 这里直接用一个"只有 header 必需位"的帧更贴近真实码流。
    BitWriter w;
    w.Bit(0);     // show_existing_frame
    w.Bits(0, 2); // frame_type = KEY
    w.Bit(1);     // show_frame
    w.Bit(0);     // disable_cdf_update
    w.Bit(0);     // allow_screen_content_tools
    w.Bit(0);     // frame_size_override_flag
    feed.Obu(MakeObu(6, w.Bytes()));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.order_source, "display_rank") << "没有 order_hint 就不能声称顺序值来自码流";
    EXPECT_EQ(t.poc_type, -1);
    ASSERT_EQ(t.frames.size(), 1u);
    EXPECT_EQ(t.frames[0].poc, 0);
}

TEST(Av1FrameOrderTracker, ReducedStillPictureHeaderHasNoOrderHint) {
    SeqOptions opt;
    opt.reduced_still_picture_header = true;
    Feed feed;
    feed.Obu(SequenceHeaderObu(opt));
    feed.Obu(ReducedStillPictureFrameObu());

    const FrameOrderTimeline& t = feed.tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_EQ(t.order_source, "display_rank");
    EXPECT_EQ(t.frames_observed, 1);
}

TEST(Av1FrameOrderTracker, DecoderModelInfoIsReportedAsUnsupported) {
    Feed feed;
    feed.Obu(SequenceHeaderWithDecoderModel());
    feed.Obu(KeyFrameObu(0));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_NE(t.unavailable_reason.find("decoder_model_info"), std::string::npos)
        << "原因里必须点名 decoder_model_info，否则读者不知道该去改什么：" << t.unavailable_reason;
}

TEST(Av1FrameOrderTracker, FrameHeaderBeforeSequenceHeaderIsReported) {
    Feed feed;
    feed.Obu(KeyFrameObu(0));

    const FrameOrderTimeline& t = feed.tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_NE(t.unavailable_reason.find("序列头"), std::string::npos) << t.unavailable_reason;
}

TEST(Av1FrameOrderTracker, TruncatedFrameHeaderIsNotCounted) {
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    // frame_type 之后就没有了：show_frame / order_hint 都读不到
    BitWriter w;
    w.Bit(0);     // show_existing_frame
    w.Bits(1, 2); // frame_type = INTER
    const auto truncated = MakeObu(6, w.Bytes());
    feed.Obu(truncated);

    const FrameOrderTimeline& t = feed.tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_EQ(t.frames_observed, 0);
}

TEST(Av1FrameOrderTracker, FrameDetailLimitStillCountsStatistics) {
    videoeye::FrameOrderOptions opt;
    opt.max_frames = 2;
    Av1FrameOrderTracker tracker;
    tracker.Reset(0, opt);

    const auto seq = SequenceHeaderObu(SeqOptions{});
    tracker.Feed(seq.data(), seq.size(), false, 0.0, 0);
    const auto key = KeyFrameObu(0);
    tracker.Feed(key.data(), key.size(), true, 0.0, 0);
    for (int i = 1; i < 5; ++i) {
        const auto frame = InterFrameObu(i, true, 0x01);
        tracker.Feed(frame.data(), frame.size(), true, 0.0, i);
    }

    const FrameOrderTimeline& t = tracker.Finish();
    ASSERT_TRUE(t.available);
    EXPECT_TRUE(t.truncated);
    EXPECT_EQ(t.frames.size(), 2u);
    EXPECT_EQ(t.frames_observed, 5) << "统计量不能被明细上限影响";
    EXPECT_EQ(t.gop_count, 1);
}

TEST(Av1FrameOrderTracker, ResetClearsState) {
    Feed feed;
    feed.Obu(SequenceHeaderObu(SeqOptions{}));
    feed.Obu(KeyFrameObu(0));
    feed.Obu(InterFrameObu(1, true, 0x01));
    ASSERT_EQ(feed.tracker.Finish().frames_observed, 2);

    feed.tracker.Reset(3);
    const FrameOrderTimeline& t = feed.tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_EQ(t.frames_observed, 0);

    // 复位之后必须重新等序列头，不能沿用上一份
    feed.Obu(KeyFrameObu(0));
    EXPECT_FALSE(feed.tracker.Finish().available);
}

// ---------------------------------------------------------------------------
// D) 外壳分派
// ---------------------------------------------------------------------------

TEST(Av1FrameOrderTracker, DispatcherSelectsAv1) {
    FrameOrderTracker tracker;
    tracker.Reset(AV_CODEC_ID_AV1, 2);
    ASSERT_TRUE(tracker.enabled());
    EXPECT_STREQ(tracker.codec_name(), "av1");
    EXPECT_STREQ(tracker.order_source_name(), "order_hint");
    EXPECT_EQ(tracker.stream_index(), 2);
}

TEST(Av1FrameOrderTracker, DispatcherRejectsUnimplementedCodec) {
    // ❗ 这里原来拿 VVC 当"未实现"的代表（VVC 帧级解析落地后它必然红）。
    //    "未支持"要用一个永远不会进分派表的编码表达：支持清单只会变长，
    //    拿"当前最后一个没做的"当反例，每次补一个能力就要改一条无关的用例。
    FrameOrderTracker tracker;
    tracker.Reset(AV_CODEC_ID_MPEG2VIDEO, 0);
    EXPECT_FALSE(tracker.enabled());
    EXPECT_FALSE(tracker.disabled_reason().empty());
    // 原因要能让用户知道"换个能做的期待"，也就是必须列出当前覆盖的编码；
    // 光写一句"未实现"等于什么都没说（曾经就是这样）。
    EXPECT_NE(tracker.disabled_reason().find("H.264"), std::string::npos) << tracker.disabled_reason();
    EXPECT_NE(tracker.disabled_reason().find("VVC"), std::string::npos) << tracker.disabled_reason();
}

TEST(Av1FrameOrderTracker, OrderSourceNameFollowsSequenceHeader) {
    // 还没拿到序列头时只能按常见情形答（AV1 编码器默认都开 order_hint）。
    FrameOrderTracker unknown;
    unknown.Reset(AV_CODEC_ID_AV1, 0);
    EXPECT_STREQ(unknown.order_source_name(), "order_hint");

    // ❗ 这条防的是自相矛盾：日志里（order_source_name）写着 order_hint，
    // 报告里（timeline.order_source）写着 display_rank。两处必须同时说准。
    SeqOptions no_hint;
    no_hint.enable_order_hint = false;
    const auto seq_no_hint = SequenceHeaderObu(no_hint);
    FrameOrderTracker plain;
    plain.Reset(AV_CODEC_ID_AV1, 0);
    ASSERT_TRUE(plain.FeedExtradata(seq_no_hint.data(), seq_no_hint.size()));
    EXPECT_STREQ(plain.order_source_name(), "display_rank");

    BitWriter w;  // 没有 order_hint 时帧头里那 3 位不存在
    w.Bit(0);     // show_existing_frame
    w.Bits(0, 2); // frame_type = KEY
    w.Bit(1);     // show_frame
    w.Bit(0);     // disable_cdf_update
    w.Bit(0);     // allow_screen_content_tools
    w.Bit(0);     // frame_size_override_flag
    const auto frame = MakeObu(6, w.Bytes());
    plain.Feed(frame.data(), frame.size(), true, 0.0, 0);
    EXPECT_EQ(std::string(plain.order_source_name()), plain.Finish().order_source) << "日志与报告必须同一口径";

    const auto seq_with_hint = SequenceHeaderObu(SeqOptions{});
    FrameOrderTracker hinted;
    hinted.Reset(AV_CODEC_ID_AV1, 0);
    ASSERT_TRUE(hinted.FeedExtradata(seq_with_hint.data(), seq_with_hint.size()));
    EXPECT_STREQ(hinted.order_source_name(), "order_hint");
    const auto hint_frame = KeyFrameObu(0);
    hinted.Feed(hint_frame.data(), hint_frame.size(), true, 0.0, 0);
    EXPECT_EQ(std::string(hinted.order_source_name()), hinted.Finish().order_source);
}

// ---------------------------------------------------------------------------
// E) 真实语料对账（libaom 两遍编码，带 alt-ref 的 MP4）
// ---------------------------------------------------------------------------

TEST(Av1FrameOrderTracker, RealCorpusMatchesContainerDisplayOrder) {
    const std::string sample = videoeye_test::CodecSample("real_av1_altref_gop.mp4");
    videoeye_test::SkipIfMissing(sample, "real_av1_altref_gop.mp4");

    const auto facts = videoeye_test::DemuxForDisplayOrder(sample);
    ASSERT_TRUE(facts.ok);

    const auto analysis = videoeye_test::RunFrameOrderAnalysis(sample);
    ASSERT_EQ(analysis.frame_order_timelines.size(), 1u);
    const FrameOrderTimeline& t = analysis.frame_order_timelines.front();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.codec, "av1");
    EXPECT_EQ(t.order_source, "order_hint");

    // ❗ 这里对不上的是**显示事件数**，不是编码帧数：libaom 把
    // alt-ref（隐藏帧）和它之后的可见帧打进同一个样本，所以"编码帧 65 /
    // 容器样本 60"是正常的 —— 多出来的 5 个正是从不输出的 ARF。
    // 能对上的是"被显示的画面数 == 容器样本数"，这一条同时证明
    // show_existing_frame 的槽位映射没错（少显示或多显示一帧都会露）。
    EXPECT_EQ(t.frames.size(), facts.pts_in_decode_order.size());
    EXPECT_GT(t.frames_observed, static_cast<int64_t>(t.frames.size()))
        << "这份语料如果没有隐藏帧，就测不到 alt-ref 这条路径，样本该重生成";

    // 关键帧数：被测侧数 frame_type == KEY_FRAME。
    EXPECT_EQ(t.idr_count, 5) << "libaom -g 12 / 60 帧，应恰好 5 个关键帧";

    // 真正的对账：把 order_hint 解环后按值排序得到的"显示序 → 解码序"映射，
    // 必须与容器 PTS 排出来的一模一样。这是唯一不能靠"实现与规范自洽"糊过去
    // 的判据 —— 参考输出由编码器写进 ctts，与被测代码零共享。
    EXPECT_EQ(videoeye_test::DisplayOrderPerGop(t), facts.DecodeIndexInDisplayOrderPerGop());
}
