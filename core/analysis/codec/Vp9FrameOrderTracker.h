#pragma once

// VP9 帧级显示顺序追踪器。
//
// ==========================================================================
// 为什么它和 H.264 / HEVC 那两个长得不一样
//
//   **VP9 没有 POC**。规范 6.2.2 的 uncompressed header 里没有 order_hint
//   这类字段（libvpx 的 read_uncompressed_header 里也找不到它），所以显示顺序
//   推不出来 —— 它是**读出来**的，靠显示事件序：
//     * show_existing_frame == 1 → 立刻显示 frame_to_show_map_idx 槽里的那帧
//     * 否则 show_frame == 1     → 立刻显示当前这帧
//     * 否则                     → 隐藏帧（alt-ref），自己从不显示
//   既然每次"显示"都能指名道姓到某个已编码帧，"第几次显示"就是该帧的显示序号
//   （display rank）。把 rank 当占用顺序值，按解码顺序排开喂给
//   FrameOrderStats，重排 / GOP / 重复那套判据就与 H.264/HEVC 完全一致 ——
//   唯一的差别写在 FrameOrderTimeline::order_source 里（"display_rank"）。
//
//   规范允许的"重排"长这样：ARF(隐藏) → F1 → F2 … → show_existing(ARF)，
//   于是解码顺序上的显示序号是 15,0,1,…,14：第一处回退正好等于 lag。
//
//   ❗ 但要先摆正预期：**主流的 VP9 码流没有重排**。VP9 没有 B 帧，显示顺序
//   天然等于解码顺序；libvpx 的 auto-alt-ref 产出的 ARF 是**纯参考帧**
//   （show_frame=0 且没有任何 show_existing 指它）。实测一份 54 帧的
//   libvpx 两遍编码样本：5 个关键帧、4 个隐藏帧、**0 个 show_existing**。
//   所以这里报"重排 0 次"是对的，不是没算出来 —— 那条判据留给真正用
//   show_existing_frame 回放旧帧的流（SVC / 拼接产物）。
//
// ==========================================================================
// 位序（照 libvpx read_uncompressed_header，一位都不能省）
//
//   frame_marker(2)==2 → profile(2[,reserved 1]) → show_existing_frame(1)
//     ├ 1: frame_to_show_map_idx(3)                      ← 到此结束
//     └ 0: frame_type(1) show_frame(1) error_resilient_mode(1)
//          关键帧: frame_sync_code(24) → color_config() → refresh_frame_flags = 0xFF
//          帧间帧: intra_only = show_frame ? 0 : f(1)
//                  reset_frame_context(2, 仅 error_resilient == 0)
//                  intra_only ? [sync_code(24) color_config(profile>0) refresh_flags(8)]
//                             : [refresh_flags(8) 3×(ref_idx(3) sign_bias(1)) …]
//
// ❗ 帧间帧的 refresh_frame_flags **在** reset_frame_context 之后，而
//   intra_only 帧的 refresh_frame_flags 在 color_config **之后** —— 两处顺序
//   不同，照抄一份会错位 8 位以上。
//
// 明确不做：
//   * 不解 frame_size / loop_filter / quantization / tile_info（tile_info 起
//     就进布尔解码器，为了拿 QP 实现一整套算术解码不划算）
//   * 不判断 P/B：VP9 的头里只有 KEY / INTER，帧间帧到底是 P 还是 B 无从得知，
//     slice_type 一律留 -1（UI 显示 '?'），不编一个看起来合理的数
//   * 不推断"缺了哪一帧"：VP9 没有顺序计数，缺帧只能靠容器时间戳看出来
//   * 不做 superframe 之外的恢复：一个包里有几帧由包尾的 superframe index
//     给出；index 不自洽就整包丢弃，不猜边界

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/codec/FrameOrderTypes.h"
#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {

class Vp9FrameOrderTracker {
public:
    using Framing = NalFraming;
    using Options = FrameOrderOptions;

    void Reset(int stream_index, const Options& options = Options{});
    // VP9 没有 NAL 封装，这两个只为与 FrameOrderTracker 的其它实现保持同形。
    void SetFraming(Framing) {
    }
    void SetLengthPrefixBytes(int) {
    }

    // 喂一个媒体包的负载。返回这一包里解析成功的**已编码帧**数
    // （show_existing_frame 不算编码帧，superframe 里的每一帧各算一个）。
    int Feed(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index);

    // VP9 没有参数集：配置全在关键帧头里，没有 extradata 可喂。恒返回 false。
    bool FeedExtradata(const uint8_t* data, size_t size);

    // 收尾：把显示事件折成显示序号，算出 GOP / 重排 / 重复。幂等。
    const model::FrameOrderTimeline& Finish();
    const model::FrameOrderTimeline& timeline() const {
        return timeline_;
    }
    int stream_index() const {
        return stream_index_;
    }

private:
    // uncompressed header 里"判显示顺序要用"的那几位。
    struct FrameHead {
        bool show_existing = false;
        int show_slot = -1; // frame_to_show_map_idx
        bool key_frame = false;
        bool intra_only = false;
        bool show_frame = false;
        bool error_resilient = false;
        int refresh_flags = 0; // 8 位，第 i 位 = 第 i 号槽被本帧覆盖
        int profile = 0;
    };

    // 一帧在解码顺序上的位置。
    struct CodedFrame {
        int64_t packet_index = -1;
        double time_seconds = 0.0;
        int rank = -1;           // 第一次被显示时的显示序号；-1 = 从未显示
        int show_times = 0;      // 被显示了几次（>1 = 同一画面放了两次）
        bool key = false;        // frame_type == KEY_FRAME
        bool intra_only = false; // 帧间帧里的 intra_only（参考图集不同，仍然算 I）
        bool has_time = false;
    };

    // VP9 参考帧槽位数（规范 REF_FRAMES）
    static constexpr int kRefFrames = 8;
    // 一包里最多几帧（superframe index 的 frames 字段只有 3 位）
    static constexpr int kMaxFramesPerPacket = 8;
    // 追踪上限：超过之后槽位映射会失真，宁可直接停（≈9 小时的 30fps 素材）
    static constexpr size_t kMaxTrackedFrames = 1000000;

    bool ReadFrameHead(const uint8_t* data, size_t size, FrameHead& out);
    bool HandleFrame(const uint8_t* data, size_t size, bool has_time, double time_seconds, int64_t packet_index);
    // 包尾是否有自洽的 superframe index；有则填每帧长度
    static bool ParseSuperframeIndex(const uint8_t* data, size_t size, uint32_t (&sizes)[kMaxFramesPerPacket],
                                     int& count);
    void ShowFrame(int coded_index);
    void NoteReason(const std::string& reason);

    int stream_index_ = -1;
    Options options_;

    std::vector<CodedFrame> coded_;                           // 解码顺序的已编码帧
    int slot_[kRefFrames] = {-1, -1, -1, -1, -1, -1, -1, -1}; // 槽位 → coded_ 下标
    int next_rank_ = 0;
    int unresolved_shows_ = 0; // show_existing 指到空槽（码流从中间开始）
    int64_t coded_observed_ = 0;
    bool overflow_ = false;
    std::string first_reason_;

    model::FrameOrderTimeline timeline_;
    bool finished_ = false;
};

} // namespace videoeye
