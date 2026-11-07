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
#include "core/repair/RepairFragmentForensics.h"
#include "core/repair/RepairInitSegment.h"

namespace videoeye {
namespace repair {

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
    std::vector<std::string> limitations;
};

class RepairInitSegmentBuilder {
public:
    // source_path：源分片；parsed / forensics 复用 Inspector 已完成的解析与取证。
    static RepairSynthesizedInit Build(const std::string& source_path, const IsobmffFile& parsed,
                                       const RepairFragmentEvidence& forensics, const std::atomic<bool>* cancel);
};

} // namespace repair
} // namespace videoeye
