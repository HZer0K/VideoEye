#pragma once

// 无参考初始化段时，从源媒体分片的 in-band 参数集自建 ftyp+moov。
//
// 能力边界（必须由调用方写进报告，不能静默降级）：
//   * 只支持 H.264 / H.265 视频轨，且分片里必须出现完整 SPS/PPS（HEVC 还要求 VPS）；
//   * 必须有时基证据：sidx 的 timescale，或同源的解码时间证据；没有证据就拒绝，
//     因为猜 timescale 会静默改变播放速度；
//   * 不重建音频轨（raw AAC 的 AudioSpecificConfig 无法从载荷反推）。
//     出现音频轨时整体拒绝，而不是写一个占位的 mp4a 让产物"能打开但没声音"。

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairFragmentForensics.h"
#include "core/repair/RepairInitSegment.h"
#include "core/repair/RepairUtils.h"

namespace videoeye {
namespace repair {

// 时基证据分级的一次性渲染（唯一入口）：诊断、报告、JSON 都读同一份文案。
// `crosschecked` 只表示「拿源侧 moof/trun 的逐样本时长累加核对过、且落在容差内」，
// 不表示时基已被独立核验 —— 原始 mdhd 已丢失，不存在可比的基线。
inline std::string TimescaleCrosscheckText(bool crosschecked, double error_seconds, double tolerance_seconds) {
    if (!crosschecked)
        return "（源侧没有可累加的样本时长，未做事后核对）";
    return "（与源侧 moof/trun 逐样本时长累加核对：偏差 ≤ " + FormatSeconds(error_seconds) + " s，容差 " +
           FormatSeconds(tolerance_seconds) + " s）";
}

struct RepairSynthesizedInit {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> bytes; // ftyp + moov
    std::string codec_summary;
    std::vector<uint32_t> track_ids;
    std::vector<RepairReferenceTrack> tracks;
    uint32_t movie_timescale = 0;
    double estimated_duration_seconds = 0.0;
    std::string timescale_basis; // 证据来源，必须可解释
    // 时基证据分级（切片 S）：timescale 取自源侧 sidx **声明**，源片原始 mdhd 已丢失，
    // 不存在可直接比对的基线。唯一能做的核对是拿源侧 moof/trun 的逐样本时长累加
    // 反算一遍总时长，看它是否落在容差内 —— 这是「两组源侧声明互相印证」，
    // 不是「与原始 mdhd 核对」。没跑过核对 ≠ 核对通过，所以单独记一个布尔。
    bool timescale_crosschecked = false;
    double timescale_crosscheck_error_seconds = 0.0;     // 逐轨偏差的最大值（实测）
    double timescale_crosscheck_tolerance_seconds = 0.0; // 逐轨容差的最小值（最严那条）
    std::vector<std::string> limitations;
};

class RepairInitSegmentBuilder {
public:
    // source_path：源分片；parsed / forensics 复用 Inspector 已完成的解析与取证。
    // budget：共享执行预算，逐 NAL 扫描期间命中时 ok=false 且 error 写明取消/超时。
    static RepairSynthesizedInit Build(const std::string& source_path, const IsobmffFile& parsed,
                                       const RepairFragmentEvidence& forensics, const std::atomic<bool>* cancel,
                                       const RepairRunBudget* budget = nullptr);
};

} // namespace repair
} // namespace videoeye
