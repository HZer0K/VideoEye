// 帧级解码顺序 / 显示顺序（H.264 POC）的最小实证结果。
//
// 为什么需要这一层：容器给的是**解码顺序**（sample 顺序 + DTS），而播放要的是
// **显示顺序**。两者之间的重排信息只存在于码流的 POC 里。没有 POC 时，工具只能
// 说"这里有 B 帧"（靠 slice_type），说不出"B 帧重排了几帧"、"显示顺序有没有跳"、
// "这一段是不是拼接过来的"。
//
// 边界（明确不做，避免用户误以为已支持）：
//   * 不重建参考帧列表（需要 MMCO / RPL，且要按解码过程维护 DPB）
//   * 不支持 pic_order_cnt_type == 1（需要 offset_for_ref_frame 表，实际素材极少）
//   * field coding（隔行）只做保守处理：POC 仍算得出来，但不据此推断"缺帧"
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 一帧（一幅图像）在解码顺序上的位置与它的 POC。
// 同一幅图像可能有多个 slice，这里按 first_mb_in_slice == 0 只记一条。
struct FrameOrderEntry {
    int nal_unit_type = -1; // 1 = 非 IDR slice，5 = IDR slice
    int slice_type = -1;    // 归一化后：0=P 1=B 2=I 3=SP 4=SI
    bool idr = false;
    int frame_num = 0;
    int64_t poc = 0; // 显示顺序值（小于 0 是正常的：POC 可以回绕到负值）
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
    bool available = false;
    std::string unavailable_reason; // available=false 时必填，不能空着让人猜
    int poc_type = -1;
    bool field_coded = false;

    std::vector<FrameOrderEntry> frames; // 上限见 Options::max_frames
    int64_t frames_observed = 0;         // 实际扫到的帧数（含未入库的）
    bool truncated = false;

    int i_count = 0;
    int p_count = 0;
    int b_count = 0;
    int other_count = 0;
    int idr_count = 0;

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
