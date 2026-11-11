#pragma once

// 显示顺序证据层（切片 E）：解码序 → 显示序，并给出可写进 ctts 的偏移。
//
// 为什么单独一层：容器只给**解码序**（样本顺序 + DTS）。B 帧重排后的**显示序**
// 只存在于码流 POC 里。没有这一层，重建出来的 moov 只能证明"样本字节对上了"，
// 不能证明"播放顺序对上了"—— 而后者才是用户看到的东西。
//
// 四条边界（避免用"看起来像"冒充"已验证"）：
//   * POC 只用于**显示顺序**推导；绝对帧率 / 时间间隔另找证据（这里只认传进来的
//     `RepairFrameDurationEvidence`，不与 POC 互相顶替）。
//   * 追踪器不支持 `pic_order_cnt_type == 1`、缺参数集时给"不可用 + 原因"，
//     绝不退化成 0 或直接按解码序冒充显示序。
//   * 只在**同一随机接入点段内**排序：POC 跨 RAP 不连续是编码语义，不是重排。
//   * 开放 GOP 的前导图像（RASL/RADL）在当前范围外 —— 它们的 POC 按定义就小于所属
//     RAP，混进排序会把整段显示顺序算错，所以遇到直接判不可用。
//
// ❗ 这个头只出现 domain 值类型与 std::，命令空间里没有 FFmpeg / POC 追踪器类型：
// 追踪器与字节源都在 .cpp 里用（`VideoEyeRepair` 已经私有链接 `VideoEyeCodec`）。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

class SampleByteSource;
class RepairRunBudget;

// 一个访问单元在载荷窗口内的相对位置（生产链取重建器扫描出来的边界，
// 不在这一层重算 —— 两处各扫一遍迟早会不一致）。
struct RepairAuRange {
    uint64_t relative_offset = 0;
    uint32_t size = 0;
};

// 帧时长证据。ctts 的单位是 mdhd timescale，所以 tick 与秒都要留。
struct RepairFrameDurationEvidence {
    // "vui_timing" / "stts_constant" / "none"
    std::string source = "none";
    uint32_t timescale = 0;
    uint32_t sample_delta = 0;
    // 只有恒定时才允许生成 ctts：变帧率下"第 i 帧的时长"不是常数，
    // 用首帧时长乘偏移会把整条时间轴压错。
    bool constant = false;
    bool fractional_rate = false;
    std::string detail;
};

// 一帧在解码序里的位置与它的 POC。
struct RepairDisplayFrame {
    uint64_t decode_index = 0;
    int64_t poc = 0;
    bool irap = false;
    bool idr = false;
    bool leading = false;
    // 段内按 POC 升序稳定排序后的全局显示序号（0 基）。
    uint64_t display_index = 0;
    uint32_t segment = 0;
};

struct RepairDisplayOrderPlan {
    bool available = false;
    std::string unavailable_reason; // available=false 时必填
    std::string codec;              // "h264" / "hevc"
    // 顺序值来源（"poc" / "display_rank" / "order_hint"），与
    // model::FrameOrderTimeline::order_source 同口径。
    std::string order_source;
    std::string basis; // 推导依据（给人看，进报告）
    // verified = 每帧 POC 实测 + 段内无重复 POC + 首帧是 RAP
    // derived  = 有 POC，但存在下列之一：首帧不在 RAP、段内 POC 重复、明细被上限截断
    std::string confidence = "unknown";
    RepairFrameDurationEvidence duration;
    std::vector<RepairDisplayFrame> frames;
    // 追踪器实际扫到的帧数（可能大于 frames.size()，见 truncated）。
    uint64_t frames_observed = 0;
    bool truncated = false;
    uint64_t rap_segments = 0;
    // 显示序 ≠ 解码序的帧数与最大偏移幅度（帧）。
    uint64_t reordered_frames = 0;
    uint64_t max_reorder = 0;
    // 与 frames 一一对应的有符号合成时间偏移（单位：timescale tick）。
    // 只在 available && duration.constant 时非空；调用方必须核对条数。
    std::vector<int64_t> composition_offsets;
    bool ctts_required = false;
    std::vector<std::string> limitations;
};

// 纯推导：给定解码序里的逐帧 POC 与帧时长证据，产出显示序 / 重排统计 / ctts 偏移。
// 不做 I/O、不碰码流，便于用表驱动用例把边界钉死。
RepairDisplayOrderPlan DeriveDisplayOrder(std::vector<RepairDisplayFrame> frames,
                                          const RepairFrameDurationEvidence& duration);

// 实测：从载荷里逐 AU 跑 POC 追踪（分块读取、逐单元检查取消与共享预算），
// 再把结果交给 DeriveDisplayOrder。
// `parameter_sets` 接受标准 avcC / hvcC 配置记录，也接受 Annex B 参数集 ——
// 具体格式由追踪器统一识别。
RepairDisplayOrderPlan MeasureDisplayOrder(SampleByteSource& source, const std::vector<RepairAuRange>& access_units,
                                           const std::vector<uint8_t>& parameter_sets, const std::string& codec,
                                           const RepairFrameDurationEvidence& duration,
                                           const std::atomic<bool>* cancel = nullptr,
                                           const RepairRunBudget* budget = nullptr);

// ctts 盒字节（version 1，有符号偏移，连续相同偏移做 run-length 压缩）。
// plan 不可用、没有重排、偏移数与帧数不符时返回空 —— 调用方据此"不写"，
// 而不是写一个空表冒充"已重建显示顺序"。
std::vector<uint8_t> BuildCttsBox(const RepairDisplayOrderPlan& plan);

// 供结果模型 / 报告使用的摘要（plan 的向量明细不进 RepairResult）。
model::RepairDisplayOrderSummary SummarizeDisplayOrder(const RepairDisplayOrderPlan& plan, bool ctts_written);

} // namespace repair
} // namespace videoeye
