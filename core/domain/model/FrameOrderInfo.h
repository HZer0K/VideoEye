// 帧级解码顺序 / 显示顺序（POC）的最小实证结果。
//
// 为什么需要这一层：容器给的是**解码顺序**（sample 顺序 + DTS），而播放要的是
// **显示顺序**。两者之间的重排信息只存在于码流的 POC 里。没有 POC 时，工具只能
// 说"这里有 B 帧"（靠 slice_type），说不出"B 帧重排了几帧"、"显示顺序有没有跳"、
// "这一段是不是拼接过来的"。
//
// 结构本身与编码器无关：H.264 与 HEVC 的 POC 推导公式不同（H.264 有
// pic_order_cnt_type 0/1/2，HEVC 只有 lsb 一套），VP9 连 POC 都没有（显示顺序
// 靠 show_frame / show_existing_frame 的事件序还原），但产出的是同一组字段：
// 一幅画面在解码顺序上的位置 + 它的显示顺序值 + 它是不是随机接入点。
// 顺序值的来源写在 order_source 里，读的人不必猜。
//
// 边界（明确不做，避免用户误以为已支持）：
//   * 不重建参考帧列表（H.264 的 MMCO / HEVC 的 RPL，都要按解码过程维护 DPB）
//   * H.264 不支持 pic_order_cnt_type == 1（需要 offset_for_ref_frame 表，素材极少）
//   * field coding（H.264 隔行）只做保守处理：POC 仍算得出来，但不据此推断"缺帧"
//   * 不区分 temporal sub-layer（POC 推导里 prevTid0Pic 的那些细节），逐幅图像累进
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 一帧（一幅图像）在解码顺序上的位置与它的 POC。
// 同一幅图像可能有多个 slice，这里按 first_mb_in_slice == 0 只记一条。
struct FrameOrderEntry {
    // 编码器原生的 NAL 单元类型。
    //   H.264：1 = 非 IDR slice，5 = IDR slice（上层只关心这两个）
    //   HEVC ：0..9 普通 VCL，16..23 IRAP（19/20 = IDR，16..18 = BLA，21 = CRA）
    // 两者语义不同，判"是不是随机接入点"请用 irap / idr 字段，别看数字。
    int nal_unit_type = -1;
    int slice_type = -1; // 归一化后：0=P 1=B 2=I 3=SP 4=SI
    // POC 归零的随机接入点（H.264 IDR / HEVC IDR、BLA，以及码流起点上的 CRA）。
    // GOP 是以它分段、重排也不跨它计数。
    bool idr = false;
    // 随机接入点（HEVC 的 16..23 全部，含 CRA；H.264 与 idr 同义）。
    //
    // ❗ 与 idr 必须分开：开放 GOP 里 CRA 之后解码出来的是 RASL/leading picture，
    // 它们的 POC 本来就在 CRA 之前 —— 那不是"B 帧重排"，据 idr 之外再看这一位，
    // 才能把"IRAP 后面的第一处 POC 回退"排除在重排统计之外。
    bool irap = false;
    int frame_num = 0; // H.264 的 frame_num；HEVC 没有这个字段，恒为 0
    int64_t poc = 0;   // 显示顺序值（小于 0 是正常的：POC 可以回绕到负值）
    int64_t packet_index = -1;
    bool has_time = false;
    double time_seconds = 0.0;

    // 'I' / 'P' / 'B' / 'S'（SP/SI）/ '?'（未知）
    char TypeChar() const {
        switch (slice_type) {
        case 2:
            return 'I';
        case 0:
            return 'P';
        case 1:
            return 'B';
        case 3:
        case 4:
            return 'S';
        default:
            return '?';
        }
    }
};

// 一条流的帧顺序时间线。
struct FrameOrderTimeline {
    int stream_index = -1; // 所属视频流；-1 = 未绑定（只有手构造的测试对象会是）
    // 产出这条时间线的编码（"h264" / "hevc"）。
    // 由管线填（domain 不认 AVCodecID），也可以是空串（手构造的测试对象）。
    // 用途只有文案：HEVC 没有 frame_num，报"IDR"也不如报"随机接入点"准确。
    std::string codec;
    bool available = false;
    std::string unavailable_reason; // available=false 时必填，不能空着让人猜
    // H.264 的 pic_order_cnt_type（0/1/2）；HEVC 只有一套公式，恒记 0。
    // VP9 没有这个字段（见 order_source），恒为 -1。
    // -1 = 还没拿到参数集，或该编码没有 POC 的概念。
    int poc_type = -1;
    // 显示顺序值的来源，决定报告与 UI 的措辞：
    //   "poc"          —— 从码流 POC 推出（H.264 / HEVC）
    //   "display_rank" —— 由显示事件序还原的显示序号（VP9；该编码没有 POC）
    // 空串 = 手构造的测试对象或旧数据，按 "poc" 处理。
    std::string order_source;
    bool field_coded = false;

    std::vector<FrameOrderEntry> frames; // 上限见 Options::max_frames
    int64_t frames_observed = 0;         // 实际扫到的帧数（含未入库的）
    bool truncated = false;

    int i_count = 0;
    int p_count = 0;
    int b_count = 0;
    int other_count = 0;
    // POC 归零的随机接入点个数（H.264 的 IDR；HEVC 的 IDR / BLA，以及码流起点上的 CRA）
    int idr_count = 0;
    // 随机接入点个数（HEVC 的 16..23 全部，含插在中间的 CRA；H.264 与 idr_count 相同）。
    // 与 idr_count 分开是因为开放 GOP 里这两者会差得很多：GOP 段数按后者分段，
    // 但"这个文件有几个可跳转的点"要按前者回答。
    int irap_count = 0;

    // 解码顺序里 POC 回退的次数与最大回退幅度。
    // 这是"存在显示顺序重排（B 帧）"的直接证据；回退幅度就是重排深度。
    int reorder_events = 0;
    int64_t reorder_depth = 0;

    // 两帧 POC 完全相同：同一幅图像被重复写入，或不同来源的帧撞在了一起。
    // 与"同帧多 slice"无关（那种情况在入库前已经合并）。
    int duplicate_poc_count = 0;

    // POC 槽位缺失数：按观测到的最小正步长推断"本应有多少帧"，
    // 与实际出现的不同 POC 值个数之差。仅在不含 field coding 时给出。
    int64_t poc_gap_slots = 0;

    // GOP 结构：以 IDR（以及"POC 回退到更早位置的非 IDR I 帧"）为边界。
    //
    // 最短/最长/平均按**所有**段统计，末尾那段（还没看到下一个边界的）也计入 ——
    // 排除它的话，40 帧、IDR 在第 0 与第 25 帧的文件会报成
    // "GOP 2 段（最短 25 / 最长 25）"，与"第二段只有 15 帧"直接矛盾。
    // 唯一例外是明细被上限截断（truncated == true）：那时末尾那段没扫完，不计。
    int gop_count = 0;
    int64_t min_gop_frames = 0;
    int64_t max_gop_frames = 0;
    double avg_gop_frames = 0.0;

    bool HasReordering() const {
        return reorder_events > 0;
    }

    // 一行摘要，给报告/UI 直接用；没有数据时返回原因。
    std::string Summary() const;
};

} // namespace model
} // namespace videoeye
