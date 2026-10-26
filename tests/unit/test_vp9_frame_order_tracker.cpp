// VP9 帧级显示顺序追踪的单元测试。
//
// 两组互补：
//   A) 合成位流 —— 按规范 6.2.2 的字段顺序手工造帧头，逐个位验证"读到的
//      refresh_frame_flags / show_existing 槽位对不对"。这一组能钉住位布局，
//      但与被测实现同源，只能证明"实现与规范写的一致"。
//   B) 真实语料 —— tests/corpus/codec/real_vp9_altref_gop.webm，libvpx 两遍
//      编码产出的 webm（4 个 superframe，每个含隐藏的 alt-ref + 可见帧）。
//      拿容器按 GOP 分段的关键帧数当独立事实对账。
//
// ❗ VP9 没有 POC：顺序值是"第几次显示"（display_rank），P 与 B 也无法区分。
//   这两点的正反例都在下面。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/FrameOrderTracker.h"
#include "core/analysis/codec/Vp9FrameOrderTracker.h"
#include "core/domain/model/FrameOrderInfo.h"
#include "tests/support/FrameOrderTestSupport.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::FrameOrderTracker;
using videoeye::Vp9FrameOrderTracker;
using videoeye::model::FrameOrderTimeline;

// MSB-first 位写入器（VP9 的 f(n) 是高位在前；VP8 才是低位在前，别混）
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

constexpr uint32_t kSyncCode = 0x498342;

// frame_marker(2)=2 + profile 0（两个 profile 位都写 0）
void WriteFramePrefix(BitWriter& w) {
    w.Bits(2, 2);
    w.Bit(0);
    w.Bit(0);
}

std::vector<uint8_t> KeyFrame(bool show_frame = true) {
    BitWriter w;
    WriteFramePrefix(w);
    w.Bit(0); // show_existing_frame
    w.Bit(0); // frame_type = KEY_FRAME
    w.Bit(show_frame ? 1 : 0);
    w.Bit(0); // error_resilient_mode
    w.Bits(kSyncCode, 24);
    w.Bits(2, 3); // color_space = BT709
    w.Bit(0);     // color_range（studio）
    return w.Bytes();
}

// 帧间帧。refresh_flags 的第 i 位 = 写进第 i 号参考槽。
// intra_only 只在 show_frame == 0 时存在于位流里（规范：show_frame 为 1 时
// intra_only 直接定义为 0，位流里没有这一位）。
std::vector<uint8_t> InterFrame(bool show_frame, int refresh_flags, bool intra_only = false,
                                bool error_resilient = false) {
    BitWriter w;
    WriteFramePrefix(w);
    w.Bit(0); // show_existing_frame
    w.Bit(1); // frame_type = INTER_FRAME
    w.Bit(show_frame ? 1 : 0);
    w.Bit(error_resilient ? 1 : 0);
    if (!show_frame)
        w.Bit(intra_only ? 1 : 0);
    if (!error_resilient)
        w.Bits(0, 2); // reset_frame_context
    if (intra_only) {
        w.Bits(kSyncCode, 24);
        // profile 0 的 intra_only 帧没有 color_config
    }
    w.Bits(static_cast<uint32_t>(refresh_flags), 8);
    if (!intra_only) {
        for (int i = 0; i < 3; ++i) { // ref_frame_idx(3) + sign_bias(1)
            w.Bits(0, 3);
            w.Bit(0);
        }
    }
    return w.Bytes();
}

std::vector<uint8_t> ShowExisting(int slot) {
    BitWriter w;
    WriteFramePrefix(w);
    w.Bit(1); // show_existing_frame
    w.Bits(static_cast<uint32_t>(slot), 3);
    return w.Bytes();
}

// 按规范把若干帧打成一个 superframe（尾部是 [marker][各帧长度…][marker]）
std::vector<uint8_t> Superframe(const std::vector<std::vector<uint8_t>>& frames, int mag = 2) {
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& frame : frames)
        out.insert(out.end(), frame.begin(), frame.end());

    const int count = static_cast<int>(frames.size());
    const uint8_t marker = static_cast<uint8_t>(0xC0 | ((mag - 1) << 3) | (count - 1));
    out.push_back(marker);
    for (const std::vector<uint8_t>& frame : frames) {
        for (int j = 0; j < mag; ++j)
            out.push_back(static_cast<uint8_t>((frame.size() >> (j * 8)) & 0xFF));
    }
    out.push_back(marker);
    return out;
}

struct FeedFixture {
    Vp9FrameOrderTracker tracker;

    explicit FeedFixture(videoeye::FrameOrderOptions options = videoeye::FrameOrderOptions()) {
        tracker.Reset(0, options);
    }

    void Feed(const std::vector<uint8_t>& bytes) {
        tracker.Feed(bytes.data(), bytes.size(), false, 0.0, next_packet_++);
    }

    const FrameOrderTimeline& Finish() {
        return tracker.Finish();
    }

private:
    int64_t next_packet_ = 0;
};

// ---------------------------------------------------------------------------
// 1) 关键帧 + 全可见帧：没有重排
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, VisibleFramesHaveNoReordering) {
    FeedFixture f;
    f.Feed(KeyFrame());
    for (int i = 0; i < 4; ++i)
        f.Feed(InterFrame(true, 0));

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames.size(), 5u);
    EXPECT_EQ(t.frames_observed, 5);
    EXPECT_EQ(t.order_source, "display_rank");
    EXPECT_EQ(t.gop_count, 1);
    EXPECT_EQ(t.reorder_events, 0);
    EXPECT_EQ(t.irap_count, 1);
    // 显示序就是解码序
    for (std::size_t i = 0; i < t.frames.size(); ++i)
        EXPECT_EQ(t.frames[i].poc, static_cast<int64_t>(i));
}

// ---------------------------------------------------------------------------
// 2) 隐藏帧（alt-ref）先编码、后显示 —— 这才是 VP9 的重排
// ---------------------------------------------------------------------------
// 解码顺序：KEY(可见) → ARF(隐藏) → F1(可见) → F2(可见) → show_existing(ARF)
// 显示顺序：KEY, F1, F2, ARF  → 显示序号 0,1,2,3
// 解码顺序上的序号：0,3,1,2 → 一处回退，幅度 2（3 → 1）
TEST(Vp9FrameOrderTrackerTest, HiddenAltRefShownLaterIsReordering) {
    FeedFixture f;
    f.Feed(KeyFrame());                // 槽位全部指向关键帧
    f.Feed(InterFrame(false, 1 << 0)); // ARF 隐藏，写进 0 号槽
    f.Feed(InterFrame(true, 0));
    f.Feed(InterFrame(true, 0));
    f.Feed(ShowExisting(0));

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 4u) << "隐藏帧自己不该进明细";
    EXPECT_EQ(t.frames_observed, 4) << "但已编码帧数要算上它";
    ASSERT_EQ(t.reorder_events, 1);
    EXPECT_EQ(t.reorder_depth, 2);
    EXPECT_EQ(t.frames[0].poc, 0);
    EXPECT_EQ(t.frames[1].poc, 3) << "ARF 是最后才显示的";
    EXPECT_EQ(t.frames[2].poc, 1);
    EXPECT_EQ(t.frames[3].poc, 2);
}

// ---------------------------------------------------------------------------
// 3) 隐藏帧从未被显示 → 不进明细，也不该让整条时间线不可用
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, HiddenFrameNeverShownIsExcluded) {
    FeedFixture f;
    f.Feed(KeyFrame());
    f.Feed(InterFrame(false, 1 << 0)); // 隐藏，且没有 show_existing 指它
    f.Feed(InterFrame(true, 0));

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames.size(), 2u);
    EXPECT_EQ(t.frames_observed, 3);
}

TEST(Vp9FrameOrderTrackerTest, AllHiddenFramesAreUnavailable) {
    FeedFixture f;
    f.Feed(KeyFrame(false)); // 关键帧自己也是隐藏的
    f.Feed(InterFrame(false, 1 << 0));

    const FrameOrderTimeline& t = f.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_FALSE(t.unavailable_reason.empty());
    EXPECT_NE(t.unavailable_reason.find("隐藏帧"), std::string::npos) << t.unavailable_reason;
}

// ---------------------------------------------------------------------------
// 4) 从 show_existing 开始时槽位是空的：不能崩，也不能编一个内容出来
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, ShowExistingOfEmptySlotIsNotFatal) {
    FeedFixture f;
    f.Feed(ShowExisting(5)); // 空槽
    f.Feed(KeyFrame());
    f.Feed(InterFrame(true, 0));

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames.size(), 2u) << "空槽那次显示没有内容可记";
    EXPECT_EQ(t.reorder_events, 0) << "不该因为一次无内容的显示就报重排";
}

// ---------------------------------------------------------------------------
// 5) intra_only 帧的 refresh_frame_flags 位置（在 sync_code + 无 color_config 之后）
// ---------------------------------------------------------------------------
// profile 0 的 intra_only 帧没有 color_config，refresh_frame_flags 紧跟同步码。
// 造错位一位都会让后面 show_existing 指不到它（或者指到别的帧）。
TEST(Vp9FrameOrderTrackerTest, IntraOnlyFrameRefreshesItsSlot) {
    FeedFixture f;
    f.Feed(KeyFrame());
    f.Feed(InterFrame(/*show_frame=*/false, /*refresh_flags=*/1 << 2, /*intra_only=*/true));
    f.Feed(InterFrame(true, 0));
    f.Feed(ShowExisting(2)); // 显示 intra_only 那一帧

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 3u);
    // intra_only 帧是 I 帧（它自己不带参考），显示序排在最后
    EXPECT_EQ(t.frames[1].slice_type, 2) << "intra_only 帧必须归成 I";
    EXPECT_EQ(t.frames[1].poc, 2);
    EXPECT_EQ(t.i_count, 2) << "关键帧 + intra_only 帧";
}

// ---------------------------------------------------------------------------
// 6) superframe：一个包里两帧，拆错就只有一帧
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, SuperframeYieldsBothFrames) {
    FeedFixture f;
    f.Feed(KeyFrame());
    const std::vector<uint8_t> super = Superframe({InterFrame(false, 1 << 0), InterFrame(true, 0)});
    EXPECT_EQ(f.tracker.Feed(super.data(), super.size(), false, 0.0, 1), 2);

    f.Feed(ShowExisting(0));
    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    // 关键帧 1 + superframe 里 2 帧 = 3 个已编码帧；最后那次 show_existing
    // 显示的是隐藏帧，不产生新的编码帧。
    EXPECT_EQ(t.frames_observed, 3);
    EXPECT_EQ(t.frames.size(), 3u) << "隐藏帧被显示后要进明细，拆不开就只剩 2 帧";
}

// 索引与负载不自洽时必须整包当普通帧处理，不能硬切
TEST(Vp9FrameOrderTrackerTest, SuperframeIndexMismatchFallsBackToSingleFrame) {
    std::vector<uint8_t> packet = InterFrame(true, 0);
    packet.push_back(0xC0); // 冒充 superframe 标记，但前面没有同值标记

    FeedFixture f;
    f.Feed(KeyFrame());
    EXPECT_EQ(f.tracker.Feed(packet.data(), packet.size(), false, 0.0, 1), 1);
    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 2);
}

// ---------------------------------------------------------------------------
// 7) 帧间帧的 slice_type 必须留空：VP9 的头里没有 P/B
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, InterFrameSliceTypeIsUnknown) {
    FeedFixture f;
    f.Feed(KeyFrame());
    f.Feed(InterFrame(true, 0));
    f.Feed(InterFrame(true, 0));

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    ASSERT_EQ(t.frames.size(), 3u);
    EXPECT_EQ(t.frames[0].slice_type, 2);
    for (std::size_t i = 1; i < t.frames.size(); ++i) {
        EXPECT_EQ(t.frames[i].slice_type, -1) << "帧间帧不区分 P/B，不许猜";
        EXPECT_EQ(t.frames[i].TypeChar(), '?');
    }
    EXPECT_EQ(t.p_count, 0);
    EXPECT_EQ(t.b_count, 0);
    EXPECT_EQ(t.other_count, 2);
    // 文案不能说"无 B 帧 → 解码顺序 = 显示顺序"（那是 H.264 的说法）
    EXPECT_NE(t.Summary().find("帧间帧"), std::string::npos) << t.Summary();
}

// ---------------------------------------------------------------------------
// 8) 同一画面显示两次 → 重复计数
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, ShowingSameFrameTwiceCountsAsDuplicate) {
    FeedFixture f;
    f.Feed(KeyFrame());      // 全槽 → 槽 0 指向它
    f.Feed(ShowExisting(0)); // 再显示一次关键帧
    f.Feed(InterFrame(true, 0));

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_GE(t.duplicate_poc_count, 1);
}

// ---------------------------------------------------------------------------
// 9) 关键帧分段：每段一个 GOP
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, GopCountFollowsKeyFrames) {
    FeedFixture f;
    for (int gop = 0; gop < 3; ++gop) {
        f.Feed(KeyFrame());
        for (int i = 0; i < 3; ++i)
            f.Feed(InterFrame(true, 0));
    }
    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.gop_count, 3);
    EXPECT_EQ(t.irap_count, 3);
    EXPECT_EQ(t.idr_count, 3);
    EXPECT_EQ(t.max_gop_frames, 4);
}

// ---------------------------------------------------------------------------
// 10) 明显的坏包：不该被记成帧，也不该把后面的帧带偏
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, GarbagePacketIsRejected) {
    FeedFixture f;
    const std::vector<uint8_t> garbage = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(f.tracker.Feed(garbage.data(), garbage.size(), false, 0.0, 0), 0);

    // frame_marker 对但同步码错的关键帧也要拒
    BitWriter w;
    WriteFramePrefix(w);
    w.Bit(0);
    w.Bit(0);
    w.Bit(1);
    w.Bit(0);
    w.Bits(0x123456, 24);
    const std::vector<uint8_t> bad_sync = w.Bytes();
    EXPECT_EQ(f.tracker.Feed(bad_sync.data(), bad_sync.size(), false, 0.0, 1), 0);

    f.Feed(KeyFrame());
    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames_observed, 1);
}

TEST(Vp9FrameOrderTrackerTest, TooShortPacketIsRejected) {
    FeedFixture f;
    const std::vector<uint8_t> tiny = {0x82, 0x00};
    EXPECT_EQ(f.tracker.Feed(tiny.data(), tiny.size(), false, 0.0, 0), 0);
    EXPECT_FALSE(f.Finish().available);
}

// ---------------------------------------------------------------------------
// 11) 明细上限：统计仍按全量，明细截断要声明
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, DetailLimitTruncatesButCountsAll) {
    videoeye::FrameOrderOptions options;
    options.max_frames = 3;
    FeedFixture f(options);
    f.Feed(KeyFrame());
    for (int i = 0; i < 9; ++i)
        f.Feed(InterFrame(true, 0));

    const FrameOrderTimeline& t = f.Finish();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.frames.size(), 3u);
    EXPECT_EQ(t.frames_observed, 10);
    EXPECT_TRUE(t.truncated);
    EXPECT_NE(t.Summary().find("仅统计前"), std::string::npos) << t.Summary();
}

// ---------------------------------------------------------------------------
// 12) 外壳分派：VP9 建槽，未实现的编码不建
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, DispatcherEnablesVp9OnlyForVp9) {
    FrameOrderTracker vp9;
    vp9.Reset(AV_CODEC_ID_VP9, 2);
    EXPECT_TRUE(vp9.enabled());
    EXPECT_STREQ(vp9.codec_name(), "vp9");
    EXPECT_EQ(vp9.stream_index(), 2);
    // VP9 没有参数集，extradata 路径恒返回 false
    EXPECT_FALSE(vp9.FeedExtradata(nullptr, 0));

    // ❗ 这里曾经拿 AV1 当"未实现"的代表，AV1 落地后本用例就红了。
    // 唯一还没实现的编码现在是 VVC —— 别再用 AV1/VP9 互相当反例。
    FrameOrderTracker vvc;
    vvc.Reset(AV_CODEC_ID_VVC, 0);
    EXPECT_FALSE(vvc.enabled());
    EXPECT_FALSE(vvc.disabled_reason().empty());
    EXPECT_STREQ(vvc.codec_name(), "");
}

// ---------------------------------------------------------------------------
// 13) 重置：状态必须彻底清掉（同一对象会被复用到下一条流）
// ---------------------------------------------------------------------------
TEST(Vp9FrameOrderTrackerTest, ResetClearsEverything) {
    Vp9FrameOrderTracker tracker;
    tracker.Reset(0);
    const std::vector<uint8_t> key = KeyFrame();
    EXPECT_EQ(tracker.Feed(key.data(), key.size(), false, 0.0, 0), 1);
    ASSERT_TRUE(tracker.Finish().available);

    tracker.Reset(1);
    const FrameOrderTimeline& t = tracker.Finish();
    EXPECT_FALSE(t.available);
    EXPECT_EQ(t.frames_observed, 0);
    EXPECT_EQ(tracker.stream_index(), 1);
}

// ---------------------------------------------------------------------------
// B) 真实语料：libvpx 两遍编码的 webm，带 alt-ref superframe
// ---------------------------------------------------------------------------
// 对账用两条独立通道：
//   * 容器按 GOP 分段的关键帧数（av_read_frame 的 AV_PKT_FLAG_KEY）
//   * 逐帧解出的关键帧数（frame_type == KEY_FRAME）
// 二者必须相等，且这份语料确实存在重排（否则测不出什么）。
TEST(Vp9FrameOrderTrackerTest, RealAltRefSampleReconcilesWithContainer) {
    const std::string path = videoeye_test::CodecSample("real_vp9_altref_gop.webm");
    videoeye_test::SkipIfMissing(path, "real_vp9_altref_gop.webm");

    const videoeye_test::DemuxedOrderFacts facts = videoeye_test::DemuxForDisplayOrder(path);
    ASSERT_TRUE(facts.ok) << "容器没读出至少两个视频包";

    const videoeye::model::AnalysisResult result = videoeye_test::RunFrameOrderAnalysis(path);
    ASSERT_FALSE(result.frame_order_timelines.empty()) << "VP9 轨应当建出帧顺序时间线";
    const FrameOrderTimeline& t = result.frame_order_timelines.front();
    ASSERT_TRUE(t.available) << t.unavailable_reason;
    EXPECT_EQ(t.codec, "vp9");
    EXPECT_EQ(t.order_source, "display_rank");

    const std::size_t container_keys = static_cast<std::size_t>(
        std::count(facts.is_key_in_decode_order.begin(), facts.is_key_in_decode_order.end(), true));
    ASSERT_GT(container_keys, 0u);
    EXPECT_EQ(static_cast<std::size_t>(t.irap_count), container_keys) << "逐帧解出的关键帧数与容器同步样本数不一致";

    // 这份语料是 50 个包、其中 4 个是 [ARF + 可见帧] 的 superframe：
    // 已编码帧数（54）必须**多于**容器包数（50），显示帧数则等于 50。
    EXPECT_EQ(t.frames.size(), facts.pts_in_decode_order.size())
        << "显示帧数应当等于容器包数（每个包正好输出一个画面）";
    EXPECT_EQ(t.frames_observed, 54) << "50 个包 + 4 个 superframe 里各多出 1 帧";
    // ❗ 实测结论（值得记下来）：libvpx 的 alt-ref 是**纯参考帧**，从不被
    // show_existing_frame 显示 —— 这 54 帧里有 4 个 show_frame=0 且 0 个
    // show_existing。所以 VP9 常态下**没有**显示顺序重排（它也没有 B 帧的概念），
    // 重排只可能出现在用 show_existing_frame 回放旧帧的流里（合成用例覆盖）。
    EXPECT_EQ(t.reorder_events, 0) << "这份语料没有 show_existing_frame，不该报重排";
    EXPECT_EQ(t.duplicate_poc_count, 0);
}

} // namespace
