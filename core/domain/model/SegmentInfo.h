#pragma once

// HLS / DASH 分片（segment）级数据契约
//
// 设计约束（与 Mp4SampleInfo.h 一致）:
//   - 只用 std::string / std::vector / 定长整数，不依赖 Qt 与第三方库，
//     便于在 tests/unit 里喂合成数据做单测，也便于跨线程按值传递。
//   - 时间统一用「秒」存 double：清单里给的往往是整数毫秒/微秒或 timescale 分数，
//     换算在解析器里做完，UI 与校验逻辑只认秒。
//   - 校验逻辑本身放在 analyzer 侧（HlsManifestAnalyzer / DashManifestAnalyzer /
//     SegmentQcAnalyzer::Validate），本文件只描述数据结构。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 分片的容器形态。决定用哪个已有分析器去深挖:
//   MPEG_TS -> TsStructureAnalyzer   （Qt 侧，由 ContainerStructureAnalyzer 分发）
//   FMP4    -> Mp4SampleTableAnalyzer / Mp4BoxAnalyzer（纯 C++，可单测）
enum class SegmentContainer {
    Unknown,     // 未识别 / 未探测
    MPEG_TS,     // .ts / .m2ts（HLS 传统封装）
    FMP4,        // .m4s / .mp4 / .cmfv（CMAF / fMP4）
    WebM,        // DASH-WebM 分片
    PackedAudio, // AAC ADTS / MP3 等裸音频分片
    Other
};

// 清单里的一个分片
struct SegmentInfo {
    // ---- 清单侧（解析器必填）----
    std::string uri;                // 清单里原样写的 URI（可能是相对路径或绝对 URL）
    std::string resolved_path;      // 解析成本地路径后的结果（空 = 网络 URI 或无法解析）
    uint64_t sequence = 0;          // 媒体序号（HLS: MEDIA-SEQUENCE + i；DASH: startNumber + i）
    double duration_seconds = 0.0;  // EXTINF / SegmentTimeline@d / SegmentTemplate@duration
    bool has_duration = false;      // 清单是否真的给了时长（DASH SegmentList 可能没有）
    int64_t byte_range_offset = -1; // EXT-X-BYTERANGE / DASH media range 起点
    int64_t byte_range_length = -1; // 上者长度

    // ---- 状态标记 ----
    bool discontinuity_before = false; // 段前带 EXT-X-DISCONTINUITY
    bool gap = false;                  // EXT-X-GAP（段被标记为不可用）
    bool has_init_section = false;     // 段前有 EXT-X-MAP / DASH 有 Initialization
    std::string init_uri;              // 初始化段 URI（HLS 的 EXT-X-MAP / DASH 的 initialization）
    bool partial = false;              // EXT-X-PART（LL-HLS 部分分片）
    bool preload_hint = false;         // EXT-X-PRELOAD-HINT（LL-HLS 预加载提示）

    // ---- 落盘情况（仅本地包；网络包全为 false / 0）----
    bool exists = false;
    int64_t file_size_bytes = 0;

    // ---- 容器探测结果（SegmentQcAnalyzer::ProbeSegments 填充）----
    SegmentContainer container = SegmentContainer::Unknown;
    bool probed = false;                 // 是否真的探测过
    bool container_parse_failed = false; // 已有容器分析器解析失败
    std::string container_error;
    // fMP4: 首个 moof 的 tfdt（媒体时基）+ 初始化段带来的 timescale
    uint32_t timescale = 0;
    uint64_t decode_time = 0;
    bool has_decode_time = false;
    // 段在媒体时间轴上的起点（秒）。fMP4 用 tfdt/timescale，其它用清单累计时长兜底。
    double start_seconds = 0.0;
    // 该段起点是否是关键帧。fMP4 能拿到 tfdt 即为真（CMAF 段必须以 IDR 开头）；
    // TS 按 ABR 惯例视为真（实际是否 IDR 需要 TsStructureAnalyzer 逐包确认）。
    // 只有为真的段才参与「多码率 variant 关键帧对齐」比对。
    bool starts_with_keyframe = false;

    std::string ContainerName() const {
        switch (container) {
        case SegmentContainer::MPEG_TS:
            return "MPEG-TS";
        case SegmentContainer::FMP4:
            return "fMP4/CMAF";
        case SegmentContainer::WebM:
            return "WebM";
        case SegmentContainer::PackedAudio:
            return "Packed Audio";
        case SegmentContainer::Other:
            return "Other";
        default:
            return "Unknown";
        }
    }

    double EndSeconds() const {
        return start_seconds + duration_seconds;
    }
    // 由落盘大小与时长推算的段码率（bps）；缺任一信息返回 0
    int64_t MeasuredBitrateBps() const {
        if (!exists || file_size_bytes <= 0 || duration_seconds <= 0.0)
            return 0;
        return static_cast<int64_t>(static_cast<double>(file_size_bytes) * 8.0 / duration_seconds);
    }
};

} // namespace model
} // namespace videoeye
