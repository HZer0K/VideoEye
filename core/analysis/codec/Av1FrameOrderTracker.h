#pragma once

// AV1 帧级显示顺序追踪器。
//
// ==========================================================================
// 为什么它和另外三个都不一样
//
//   AV1 **有** order_hint（与 VP9 相反，与 H.264/HEVC 相似），所以顺序值
//   是真读出来的，不是靠显示事件序凑的：
//     * sequence_header_obu 里 enable_order_hint 打开时，
//       order_hint_bits = order_hint_bits_minus_1 + 1（1..8）；
//     * 每个 frame_header_obu 在 frame_size_override_flag 之后紧跟
//       order_hint f(order_hint_bits)；
//     * show_existing_frame 分支**不读** order_hint —— 它显示的是某个槽里
//       已经解出来的画面，顺序值要回查那个槽。
//
//   ❗ order_hint 是模 2^order_hint_bits 的**环形量**。直接比大小会把
//   "255 → 0" 误判成"回退 255 帧"。必须先按规范的 get_relative_dist() 解环：
//       m = 1 << (bits - 1)
//       diff = (a - b) & ((1 << bits) - 1)   // 再按有符号解释
//   解环后的值才是可以拿去比较的"显示顺序值"，直接喂给 FrameOrderStats。
//
//   ❗ 但**不能假设 order_hint 一定存在**：enable_order_hint == 0 时
//   order_hint_bits == 0（一位都不读），reduced_still_picture_header 的静图
//   更是整段字段都没有。这两种情况下顺序值与 VP9 一样只能靠显示事件序，
//   于是沿用 Vp9FrameOrderTracker 的 display rank 路径，order_source 记
//   "display_rank" —— 报告据此换措辞，不让读者去码流里找 order_hint。
//
// ==========================================================================
// 明细按"显示事件"落，不按编码帧
//
//   AV1 的编码与输出不是一回事：隐藏帧（show_frame == 0，alt-ref）自己从不
//   输出，要等后面某个 show_existing_frame 把槽里的画面调出来才算显示。
//   libaom 两遍编码的实测结构（-g 12 / 60 样本）：
//
//     KEY(hint 0, 显示) → 隐藏(hint 11, showable=0) → 隐藏(hint 5) →
//     隐藏(hint 2) → 显示(hint 1) → show_existing(slot) → 显示(hint 3) …
//
//   一个 GOP 编码 13 帧、输出 12 帧。所以：
//     * frames_observed 记**编码帧**（13）—— 与 H.264/HEVC/VP9 同口径；
//     * timeline.frames 记**显示事件**（12），与容器样本数一一对应；
//     * show_existing 的输出顺序值要回查那一帧自己的 order_hint，
//       不能按"第几次显示"重新编号。
//   把隐藏帧当成普通帧落进明细，会让 frames 比容器多出 ARF 的个数，
//   还会把"ARF 的 hint 比后续帧大"误算成显示顺序重排。
//
// ==========================================================================
// 位序（照 libaom av1/decoder/decodeframe.c 的 read_uncompressed_header 逐位核对）
//
//   reduced_still_picture_header == 1:
//     show_existing = 0, frame_type = KEY, show_frame = 1, error_resilient = 1
//     （⚠️ 仍要吃掉 disable_cdf_update / screen content tools 那几位）
//   else:
//     show_existing(1)
//       ├ 1: frame_to_show_map_idx(3) → 结束
//       └ 0: frame_type(2) show_frame(1)
//             ├ !show_frame → showable_frame(1)
//             └ show_frame  → showable_frame = (frame_type != KEY)
//             error_resilient = (SWITCH || (KEY && show_frame)) ? 1 : f(1)
//   然后两种分支都要走的：
//     disable_cdf_update(1) → allow_screen_content_tools(条件 1 位)
//       → force_integer_mv(条件 1 位，仅当 allow 为真)
//   再往后（仅 !reduced_still_picture_header）：
//     [frame_id_numbers_present ? current_frame_id(field_len)]
//       → frame_size_override_flag(1，SWITCH 恒 1 不读)
//       → order_hint(order_hint_bits)        ← 到此为止，顺序值拿到了
//       → primary_ref_frame(3，仅 !error_resilient && !intra_only)
//       → refresh_frame_flags（KEY 且 show 时恒 0xFF 不读；SWITCH 恒 0xFF）
//
// ❗ showable_frame 这一位**只在 show_frame == 0 时存在**。写反了会多吃
// 一位，之后 order_hint 整体错位 —— 症状是"顺序值全是乱的"而不是解析失败。
//
// 明确不做：
//   * 不解析 refresh_frame_flags 之后的任何字段（tile_info 起就进布尔解码器）
//   * 不判断 P/B：AV1 的 frame_type 只有 KEY / INTER / INTRA_ONLY / SWITCH，
//     帧间帧到底是 P 还是 B 头里没有，slice_type 一律留 -1
//   * 序列头声明了 decoder_model_info 的流直接判"不支持"：那种帧头里夹着
//     buffer_removal_time，长度取决于每个 operating point 的参数，为它实现
//     一整套解析不值当 —— 宁可在明确的边界上停住并说清原因
//   * 不重建 DPB / 参考帧列表（只维护 8 个槽位的"哪一帧写进去了"映射，
//     供 show_existing_frame 回查）

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/FrameOrderTypes.h"
#include "core/domain/model/Av1BitstreamInfo.h"
#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {

class Av1FrameOrderTracker {
public:
    using Framing = NalFraming;
    using Options = FrameOrderOptions;

    void Reset(int stream_index, const Options& options = Options{});
    // AV1 没有 NAL 封装，这两个只为与 FrameOrderTracker 的其它实现保持同形。
    void SetFraming(Framing) {
    }
    void SetLengthPrefixBytes(int) {
    }

    // 喂一个媒体包（一包 = 若干 OBU）。返回这一包里解析成功的**编码帧**数
    // （show_existing_frame 不算编码帧）。
    int Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index);

    // MP4/MKV 的 extradata：av1C 里通常**不含**序列头，configOBUs 里有的话
    // 就取出来当初始状态（没有也不影响 —— 带内关键帧一定会重写一份）。
    bool FeedExtradata(const uint8_t* data, size_t size);

    const model::FrameOrderTimeline& Finish();
    const model::FrameOrderTimeline& timeline() const {
        return timeline_;
    }
    int stream_index() const {
        return stream_index_;
    }

    // 顺序值来源要等序列头才知道（enable_order_hint 关掉时退化成显示事件序）。
    // 调用方据此在"已经能判准"和"还得按常见情形猜"之间分开措辞。
    bool sequence_known() const {
        return have_seq_;
    }
    bool order_hint_enabled() const {
        return have_seq_ && hint_mode_;
    }

private:
    // frame_header_obu 里"判显示顺序要用"的那几位。
    struct FrameHead {
        bool show_existing = false;
        int show_slot = -1; // frame_to_show_map_idx
        int frame_type = 0; // 0=KEY 1=INTER 2=INTRA_ONLY 3=SWITCH
        bool show_frame = false;
        bool error_resilient = false;
        int refresh_flags = 0;
        int order_hint = 0;
        bool has_order_hint = false;
    };

    // 一帧在解码顺序上的位置。
    struct CodedFrame {
        int64_t packet_index = -1;
        double time_seconds = 0.0;
        bool has_time = false;
        int frame_type = 0;
        bool show_frame = false;
        int64_t order_value = 0; // 解环后的显示顺序值；无 order_hint 时等于 rank
        int rank = -1;           // 无 order_hint 时的显示序号；-1 = 从未显示
    };

    // 一次"显示"事件：哪一帧在哪个包被输出。
    //
    // ❗ 明细必须按**显示事件**落，不能按编码帧落：AV1 的隐藏帧（alt-ref）
    // 自己从不输出，它要等后面某个 show_existing_frame 把槽里的画面调出来才算
    // 显示。按编码帧落会多出"从没显示过"的条目，与容器里的样本数对不上。
    struct DisplayEvent {
        int coded_index = -1;
        int64_t packet_index = -1;
        double time_seconds = 0.0;
        bool has_time = false;
    };

    static constexpr int kKeyFrame = 0;
    static constexpr int kInterFrame = 1;
    static constexpr int kIntraOnlyFrame = 2;
    static constexpr int kSwitchFrame = 3;
    // AV1 的参考帧槽位数（规范 NUM_REF_FRAMES）
    static constexpr int kRefFrames = 8;
    // 追踪上限：超过之后槽位映射失真，宁可直接停（≈9 小时的 30fps 素材）
    static constexpr size_t kMaxTrackedFrames = 1000000;

    void ApplySequenceHeader(const model::Av1SequenceHeaderInfo& seq);
    bool ReadFrameHead(const uint8_t* payload, size_t size, FrameHead& out) const;
    bool HandleFrame(const uint8_t* payload, size_t size, bool has_time, double time_seconds, int64_t packet_index);
    void NoteDisplay(int coded_index, bool has_time, double time_seconds, int64_t packet_index);
    void NoteReason(const std::string& reason);
    // 规范 get_relative_dist()：把环形 order_hint 差解释成有符号距离。
    static int RelativeDist(int a, int b, int bits);

    int stream_index_ = -1;
    Options options_;

    model::Av1SequenceHeaderInfo seq_;
    bool have_seq_ = false;
    // 当前码流是否用 order_hint 当顺序值（enable_order_hint）。序列头可以中途
    // 重写，所以每次 ApplySequenceHeader 都刷新一次，Finish 以最后一次为准。
    bool hint_mode_ = false;
    // 序列头声明了 decoder_model_info：帧头里有我们跳不过去的字段，整条轨放弃。
    bool unsupported_ = false;

    std::vector<CodedFrame> coded_;
    std::vector<DisplayEvent> events_;
    // 参考帧槽 → 哪一帧（coded_ 下标）。只为 show_existing_frame 回查顺序值，
    // 不重建 DPB：槽里被谁覆盖、参考帧列表怎么排，与"显示的是哪一帧"无关。
    int slot_[kRefFrames] = {-1, -1, -1, -1, -1, -1, -1, -1};

    // 环形 order_hint 的解环状态
    bool have_last_hint_ = false;
    int last_hint_ = 0;
    int64_t hint_unwrapped_ = 0;
    // 没有 order_hint 时的显示序号
    int next_rank_ = 0;
    int unresolved_shows_ = 0;

    int64_t coded_observed_ = 0;
    bool overflow_ = false;
    std::string first_reason_;

    model::FrameOrderTimeline timeline_;
    bool finished_ = false;
};

} // namespace videoeye
