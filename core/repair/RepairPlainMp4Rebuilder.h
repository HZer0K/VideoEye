#pragma once

// P2-12 普通 MP4 无参考 moov 重建（H.264 / AAC 受限版）。
//
// 适用场景：文件有 ftyp + 单个 mdat，但 moov 完全丢失或损坏，且没有同源参考
// 初始化段。此时只能直接扫描 mdat 载荷，重建样本表与 stsd。
//
// ⚠️ 严格门槛（满足不了就拒绝，绝不猜时基）：
//   * 恰好一个 mdat，且载荷能被完整解释成"纯 H.264（AVCC）"或"纯 ADTS AAC"；
//     音视频交织的 mdat 无法在无索引的情况下拆分，直接拒绝。
//   * H.264：必须能解析 SPS/PPS。帧率证据只认两类：SPS VUI 的
//     num_units_in_tick / time_scale（源侧证据），或调用方显式传入的帧率分数
//     （`RepairTimelineAssumption`，报告里标成 user_input）。两者都没有就不生成
//     moov，避免静默改变播放速度。VUI 声明 fixed_frame_rate_flag=0 时仍按名义帧率
//     重建，但不写 ctts 并在报告里标注"逐帧时长不可验证"。
//   * AAC：必须能完整解析 ADTS 帧（采样率 / 声道 / profile 来自 ADTS 头）。
//   * 只重建单轨；重建结果不含 edts / ctts，含 B 帧的 AVCC 可能显示顺序不符。
//
// 产物是一个完整的、带样本表的普通 MP4，之后仍需走重新封装与验证流程。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/RecoveredSample.h"
#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

class RepairRunBudget;

struct RepairPlainMp4Probe {
    bool attemptable = false;
    std::string kind; // "h264" / "aac" / 空
    std::string codec_summary;
    uint32_t track_id = 1;
    std::string reason; // attemptable=false 时的拒绝原因
    std::vector<std::string> limitations;
    // 探测阶段就能确定的时基依据（mode=unknown 表示没拿到证据）。
    model::RepairTimelineBasis timeline_basis;
};

// 显式帧率（切片 F）：源侧没有帧率证据时才允许用。写成分数是为了精确 ——
// "29.97" 这种十进制近似会把时间轴按 ppm 级误差拉偏，三十秒就能差出半帧。
struct RepairTimelineAssumption {
    uint32_t num = 0;
    uint32_t den = 0;

    bool Valid() const {
        return num > 0 && den > 0;
    }
};

// 帧率分数 → mdhd 的 (timescale, sample_delta)。
//
// timescale = num * m、sample_delta = den * m（m 取最小正整数使 timescale >= 1000），
// 因此 delta / timescale 严格等于 den / num，不做四舍五入。`samples` 参与 32 位
// mdhd 上限守卫：本版本不写 mdhd version 1，所以 delta * samples 溢出时直接报错，
// 绝不写出被截断的时长。**`samples == 0` 表示"样本数还不知道"**（SPS 解析阶段
// 就是这样），此时跳过上限检查，调用方拿到样本总数后必须再算一次。
struct RepairTimelineTicks {
    uint32_t timescale = 0;
    uint32_t sample_delta = 0;
};

bool ComputeTimelineTicks(uint32_t frame_rate_num, uint32_t frame_rate_den, uint64_t samples,
                          RepairTimelineTicks* ticks, std::string* error);

struct RepairPlainMp4RebuildResult {
    bool attempted = false;
    bool ok = false;
    std::string error;
    std::string note;
    RepairPlainMp4Probe probe;
    uint64_t samples_rebuilt = 0;
    uint32_t rebuilt_tracks = 0;
    uint64_t output_bytes = 0;
    // 扫描阶段由统一账本实测的样本事实（不是再统计一遍；samples_rebuilt 直接取它的总数）。
    model::RepairSampleLedgerSummary sample_ledger;
    // 切片 E：显示顺序（POC → ctts）的实测结论。analyzed=false 表示这次没做测量。
    model::RepairDisplayOrderSummary display_order;
    // 切片 F：时基（每帧时长）是从哪来的。mode=unknown 表示没有证据。
    model::RepairTimelineBasis timeline_basis;
};

class RepairPlainMp4Rebuilder {
public:
    // 轻量探测：扫描 mdat 载荷判断能否在无 moov 的情况下重建；带取消 + 共享预算检查。
    // budget 为空时退回只看取消标志（独立调用 / 旧用例）。
    // scan_chunk_bytes 只影响分块大小，不影响样本边界：0 = 默认块。验收时用不同块大小
    // 重建同一文件，产物必须逐字节一致；载荷永不整段读入内存。
    // assumption 非空且合法时才允许在"没有 VUI timing"的情况下用显式帧率；
    // 传空表示不许猜时基（缺证据就拒绝）。
    static RepairPlainMp4Probe Probe(const std::string& source_path, const std::atomic<bool>* cancel,
                                     const RepairRunBudget* budget = nullptr, std::size_t scan_chunk_bytes = 0,
                                     const RepairTimelineAssumption* assumption = nullptr);

    // 重建：写出带 ftyp+moov+mdat 的完整普通 MP4 到 output_path。
    static RepairPlainMp4RebuildResult Rebuild(const std::string& source_path, const std::string& output_path,
                                               const std::atomic<bool>* cancel, const RepairRunBudget* budget = nullptr,
                                               std::size_t scan_chunk_bytes = 0,
                                               const RepairTimelineAssumption* assumption = nullptr);
};

} // namespace repair
} // namespace videoeye
