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

// "分片首样本是随机访问点"这条结论的证据强度。
//
// 为什么必须分开：tfdt 只说明**解码起点时间**，不能推出"首样本可独立解码"。
// 2026-10 之前 SegmentQcAnalyzer 读到 tfdt 就直接置 starts_with_keyframe=true，
// 于是"某码率层切档后首帧花屏"这类问题永远查不出来 —— 结论本身没有证据支撑。
enum class KeyframeEvidence {
    Unknown,  // 没有探测 / 探测失败 / 没有可用的 flags 来源
    Declared, // 只有清单或约定（HLS EXT-X-INDEPENDENT-SEGMENTS、ABR 惯例、tfdt 存在）
    SyncFlag, // 已从 trun/tfhd 的 sample_flags 实证首样本 sync=true
    NotSync,  // 已实证首样本是 non-sync（问题！该段不能独立解码）
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
    // 该段起点是否是关键帧（= 能不能从这一段的第一个字节开始解码）。
    //
    // ❗ 这个字段是**结论**，必须有证据才能置 true；证据强度记在 keyframe_evidence。
    //   * SyncFlag  —— fMP4 里从 trun.first_sample_flags / 首样本 sample_flags /
    //                  tfhd.default_sample_flags 实证到 sample_is_non_sync_sample==0
    //   * Declared  —— 只有清单/约定（TS 的 ABR 惯例、HLS EXT-X-INDEPENDENT-SEGMENTS）
    //   * NotSync   —— 实证到 non-sync，此时 starts_with_keyframe=false 且要报问题
    // 只有 keyframe_evidence != Unknown 的段才参与"多码率 variant 关键帧对齐"比对，
    // 比对时按 Declared 降级给出的对齐结论也要标注。
    bool starts_with_keyframe = false;
    KeyframeEvidence keyframe_evidence = KeyframeEvidence::Unknown;
    // 首样本 sample_flags 原值（诊断用；0 且 has_first_sample_flags=false 表示没拿到）
    bool has_first_sample_flags = false;
    uint32_t first_sample_flags = 0;
    bool first_sample_is_sync = false;
    int first_sample_depends_on = -1; // 1=依赖前帧 2=独立帧 -1=未声明
    // 段内所有样本覆盖的时长（fMP4 = trun 样本时长和 / timescale）。
    // 用来做"上一段起点 + 时长 ≈ 本段起点"的跨分片连续性实证，
    // 与清单声明的 duration_seconds 是两回事（后者可能被篡改/漂移）。
    double media_duration_seconds = 0.0;
    bool has_media_duration = false;
    // 实证到的首个视频帧类型（"IDR"/"CRA"/"I"/"P" …，空 = 未实证）
    std::string first_frame_type;

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
