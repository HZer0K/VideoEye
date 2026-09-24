#pragma once

// HLS / DASH 流媒体包（manifest + segment + 多码率 ladder）数据契约
//
// 与 QC 体系的关系:
//   HlsManifestAnalyzer / DashManifestAnalyzer / SegmentQcAnalyzer 负责"发现问题"，
//   QcRuleEngine 负责"决定是否上报、以什么级别上报"。二者通过 code 串起来
//   （规则 id 与 finding code 同名），与 Mp4ConsistencyIssue 完全一致的做法。
//
// 覆盖范围（第一阶段）:
//   - 只解析本地 manifest 与本地 segment（http(s):// 的 URI 只登记不下载）。
//   - HLS: master playlist / media playlist / EXT-X-TARGETDURATION / EXTINF /
//          EXT-X-MAP / EXT-X-DISCONTINUITY / EXT-X-KEY / EXT-X-PART。
//   - DASH: MPD / Period / AdaptationSet / Representation / SegmentTemplate /
//           SegmentTimeline / SegmentBase / SegmentList。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/model/DiagnosticIssue.h"
#include "core/model/SegmentInfo.h"

namespace videoeye {
namespace model {

// 清单种类
enum class StreamingKind {
    Unknown,
    HlsMaster, // m3u8 主播放列表（含 EXT-X-STREAM-INF）
    HlsMedia,  // m3u8 媒体播放列表（直接是分片序列）
    Dash       // MPD
};

inline const char* ToString(StreamingKind kind) {
    switch (kind) {
    case StreamingKind::HlsMaster:
        return "HLS 主播放列表";
    case StreamingKind::HlsMedia:
        return "HLS 媒体播放列表";
    case StreamingKind::Dash:
        return "DASH MPD";
    default:
        return "未知";
    }
}

// ---------------------------------------------------------------------------
// 问题码（同时用作 QC 规则 id）
//   container.hls.*     —— 只在 HLS 包上产出
//   container.dash.*    —— 只在 DASH 包上产出
//   container.streaming.* —— 两者通用（分片落盘、容器解析）
// ---------------------------------------------------------------------------
namespace StreamingIssueCode {
// ---- HLS 专用 ----
constexpr const char* kHlsMissingTargetDuration = "container.hls.missing_target_duration";
constexpr const char* kHlsSegmentOverTarget = "container.hls.segment_duration_over_target";
constexpr const char* kHlsSegmentJitter = "container.hls.segment_duration_jitter";
constexpr const char* kHlsDiscontinuityUnpaired = "container.hls.discontinuity_unpaired";
constexpr const char* kHlsMissingInitSection = "container.hls.missing_init_section";
constexpr const char* kHlsEncryptionKey = "container.hls.encryption_key";
constexpr const char* kHlsPartialSegment = "container.hls.partial_segment";
constexpr const char* kHlsBandwidthMismatch = "container.hls.variant_bandwidth_mismatch";
constexpr const char* kHlsVariantResolutionMismatch = "container.hls.variant_resolution_mismatch";
constexpr const char* kHlsVariantCodecMismatch = "container.hls.variant_codec_mismatch";
constexpr const char* kHlsVariantKeyframeMisalign = "container.hls.variant_keyframe_misalign";
constexpr const char* kHlsAvSegmentCountMismatch = "container.hls.av_segment_count_mismatch";

// ---- DASH 专用 ----
constexpr const char* kDashSegmentTimelineGap = "container.dash.segment_timeline_gap";
constexpr const char* kDashSegmentTimelineOverlap = "container.dash.segment_timeline_overlap";
constexpr const char* kDashMissingSegmentInfo = "container.dash.missing_segment_info";
constexpr const char* kDashBandwidthMismatch = "container.dash.representation_bandwidth_mismatch";
constexpr const char* kDashVariantResolutionMismatch = "container.dash.representation_resolution_mismatch";
constexpr const char* kDashVariantCodecMismatch = "container.dash.representation_codec_mismatch";
constexpr const char* kDashVariantKeyframeMisalign = "container.dash.variant_keyframe_misalign";
constexpr const char* kDashAvSegmentCountMismatch = "container.dash.av_segment_count_mismatch";

// ---- 通用 ----
constexpr const char* kSegmentMissingFile = "container.streaming.segment_missing_file";
constexpr const char* kSegmentContainerInvalid = "container.streaming.segment_container_invalid";
} // namespace StreamingIssueCode

// 一条流媒体包问题
struct StreamingIssue {
    std::string code; // 见 StreamingIssueCode
    std::string title;
    std::string detail; // 含实测值
    std::string suggestion;
    IssueSeverity severity = IssueSeverity::Warning;
    IssueCategory category = IssueCategory::Container;
    double metric_value = 0.0;
    double threshold = 0.0;
    int occurrence_count = 1;
    double at_seconds = -1.0; // >= 0 时把问题定位到该时间点，否则为全局问题
    int playlist_index = -1;  // 关联的 media playlist / representation（UI 定位用）
    int variant_index = -1;   // 关联的 variant
    int segment_index = -1;

    DiagnosticIssue ToDiagnosticIssue() const {
        DiagnosticIssue issue;
        issue.rule_id = code;
        issue.title = title;
        issue.detail = detail;
        issue.suggestion = suggestion;
        issue.severity = severity;
        issue.category = category;
        issue.stream_index = -1;
        issue.metric_value = metric_value;
        issue.threshold = threshold;
        issue.occurrence_count = occurrence_count;
        issue.range = (at_seconds >= 0.0) ? TimeRange::At(at_seconds) : TimeRange::Global();
        return issue;
    }
};

// Validate() 幂等的前提：每个分析器只删自己产出的那些 code，再重新追加。
// 直接 issues.clear() 会把别的分析器（Hls / Dash / SegmentQc 三者共享同一个 issues 数组）
// 的发现一起抹掉，所以按 code 精确删除。
inline void RemoveIssuesByCode(std::vector<StreamingIssue>& issues, const std::vector<const char*>& codes) {
    issues.erase(std::remove_if(issues.begin(), issues.end(),
                                [&codes](const StreamingIssue& issue) {
                                    for (const char* code : codes) {
                                        if (issue.code == code)
                                            return true;
                                    }
                                    return false;
                                }),
                 issues.end());
}

// ---------------------------------------------------------------------------
// HLS
// ---------------------------------------------------------------------------

// EXT-X-STREAM-INF 一行（一条码率流）
struct HlsVariantInfo {
    int index = -1;
    std::string uri;                   // 子播放列表 URI
    int64_t bandwidth_bps = 0;         // BANDWIDTH（峰值段码率）
    int64_t average_bandwidth_bps = 0; // AVERAGE-BANDWIDTH（可选）
    int width = 0;
    int height = 0;
    std::string resolution;  // RESOLUTION 原文，如 "1920x1080"
    double frame_rate = 0.0; // FRAME-RATE（可选）
    std::string codecs;      // CODECS 原文，如 "avc1.64001f,mp4a.40.2"
    std::string video_codec; // 从 CODECS 里挑出的视频四字符码
    std::string audio_codec; // 从 CODECS 里挑出的音频四字符码
    std::string audio_group; // AUDIO="..."
    std::string video_group;
    std::string subtitle_group;
    int playlist_index = -1; // 指向 StreamingPackageResult::playlists
};

// EXT-X-MEDIA 一行（备选音轨/字幕轨）
struct HlsRenditionInfo {
    std::string type; // AUDIO / VIDEO / SUBTITLES / CLOSED-CAPTIONS
    std::string group_id;
    std::string name;
    std::string language;
    std::string uri;
    bool is_default = false;
    bool autoselect = false;
    int playlist_index = -1;
};

// 一个 media playlist（可能是 variant 的视频轨，也可能是 EXT-X-MEDIA 的音轨）
struct MediaPlaylistInfo {
    int index = -1;
    std::string uri;
    std::string resolved_path; // 本地绝对路径（空 = 网络）
    std::string role;          // "video" / "audio" / "subtitles" / "main"
    int variant_index = -1;    // 主播放列表里的 variant 下标（-1 = 备选轨）
    std::string group_id;      // 备选轨所属 group（用于 A/V 配对）

    int version = 0;
    int target_duration_s = 0; // EXT-X-TARGETDURATION
    bool has_target_duration = false;
    int64_t media_sequence = 0;
    bool has_media_sequence = false;
    int64_t discontinuity_sequence = 0; // EXT-X-DISCONTINUITY-SEQUENCE
    bool has_discontinuity_sequence = false;
    bool playlist_type_vod = false; // EXT-X-PLAYLIST-TYPE:VOD
    bool has_endlist = false;       // EXT-X-ENDLIST（有即为点播，否则当作直播）
    bool independent_segments = false;

    // 加密标签（EXT-X-KEY）
    bool encrypted = false;
    std::string key_method; // NONE / AES-128 / SAMPLE-AES ...
    std::string key_uri;
    std::string key_format;

    // 初始化段（EXT-X-MAP，fMP4/CMAF 必需）
    bool has_init_section = false;
    std::string init_uri;
    std::string init_resolved_path;
    bool init_exists = false;
    int64_t init_file_size = 0;

    // LL-HLS（EXT-X-PART）
    bool low_latency = false;
    double part_target_seconds = 0.0;
    int64_t partial_count = 0;

    // EXT-X-DISCONTINUITY 统计（供"是否成对处理"校验）
    int64_t discontinuity_count = 0;
    bool discontinuity_dangling = false; // 末尾的 EXT-X-DISCONTINUITY 后面没有分片

    std::vector<SegmentInfo> segments;
    bool truncated = false; // 超过 max_segments_per_playlist 被截断
    bool parse_failed = false;
    std::string error_message;

    double total_duration_seconds = 0.0;

    bool IsLive() const {
        return !has_endlist && !playlist_type_vod;
    }
    // 只统计完整分片（EXT-X-PART 不算）
    int64_t SegmentCount() const {
        int64_t n = 0;
        for (const auto& s : segments) {
            if (!s.partial)
                ++n;
        }
        return n;
    }
    double AverageSegmentDuration() const {
        double sum = 0.0;
        int n = 0;
        for (const auto& s : segments) {
            if (s.partial || !s.has_duration)
                continue;
            sum += s.duration_seconds;
            ++n;
        }
        return n > 0 ? sum / n : 0.0;
    }
    double MaxSegmentDuration() const {
        double m = 0.0;
        for (const auto& s : segments) {
            if (s.partial || !s.has_duration)
                continue;
            if (s.duration_seconds > m)
                m = s.duration_seconds;
        }
        return m;
    }
    double MinSegmentDuration() const {
        double m = -1.0;
        for (const auto& s : segments) {
            if (s.partial || !s.has_duration)
                continue;
            if (m < 0.0 || s.duration_seconds < m)
                m = s.duration_seconds;
        }
        return m < 0.0 ? 0.0 : m;
    }
};

// ---------------------------------------------------------------------------
// DASH
// ---------------------------------------------------------------------------

// SegmentTimeline 里的一个 <S t= d= r= /> 条目
struct DashTimelineEntry {
    uint64_t t = 0; // 起点（timescale 单位）；首条才有 t
    bool has_t = false;
    uint64_t d = 0; // 时长
    uint32_t r = 0; // 重复次数（0 = 只此一段）
};

// 一个 Representation（一条码率流）
struct DashRepresentationInfo {
    int index = -1;
    int period_index = -1;
    int adaptation_index = -1;
    std::string id;
    int64_t bandwidth_bps = 0;
    int width = 0;
    int height = 0;
    std::string codecs;
    std::string video_codec;
    std::string audio_codec;
    std::string mime_type;
    std::string content_type; // video / audio / text
    double frame_rate = 0.0;
    std::string base_url;

    // SegmentTemplate（字段名与 DashAdaptationSetInfo 保持一致，
    // 这样解析时可以用一个泛型 lambda 同时写两处，不用复制一遍逻辑）
    bool has_template = false;
    uint32_t timescale = 0;
    uint64_t segment_duration = 0; // timescale 单位
    uint64_t start_number = 1;
    uint64_t presentation_time_offset = 0;
    std::string initialization_template;
    std::string media_template;
    bool has_segment_timeline = false;
    std::vector<DashTimelineEntry> timeline;

    bool has_segment_base = false; // SegmentBase（单文件 / onDemand）
    bool has_segment_list = false; // SegmentList + SegmentURL

    std::string init_resolved_path;
    bool init_exists = false;
    int64_t init_file_size = 0;

    std::vector<SegmentInfo> segments;
    bool truncated = false;
    bool segment_count_unknown = false; // 直播 MPD 无法预知分片数

    double total_duration_seconds = 0.0;

    double SegmentDurationSeconds() const {
        return timescale > 0 ? static_cast<double>(segment_duration) / timescale : 0.0;
    }
    double AverageSegmentDuration() const {
        double sum = 0.0;
        int n = 0;
        for (const auto& s : segments) {
            if (!s.has_duration)
                continue;
            sum += s.duration_seconds;
            ++n;
        }
        return n > 0 ? sum / n : 0.0;
    }
};

struct DashAdaptationSetInfo {
    int index = -1;
    std::string content_type; // video / audio / text
    std::string mime_type;
    std::string lang;
    std::string par; // 宽高比
    // AdaptationSet 级别的 SegmentTemplate（被下属 Representation 继承）
    bool has_template = false;
    uint32_t timescale = 0;
    uint64_t segment_duration = 0;
    uint64_t start_number = 1;
    uint64_t presentation_time_offset = 0;
    std::string initialization_template;
    std::string media_template;
    bool has_segment_timeline = false;
    std::vector<DashTimelineEntry> timeline;
    std::vector<int> representation_indices;
};

struct DashPeriodInfo {
    int index = -1;
    std::string id;
    double start_seconds = 0.0;
    double duration_seconds = 0.0;
    bool has_duration = false;
    std::vector<DashAdaptationSetInfo> adaptation_sets;
};

// ---------------------------------------------------------------------------
// Ladder（UI 用的统一视图）
// ---------------------------------------------------------------------------

// 把 HLS 的 variant 与 DASH 的 representation 投影成同一张「码率阶梯」表，
// UI 只认这个结构，不必区分两种协议。
struct StreamingLadderEntry {
    std::string label;     // "variant #0" / "Representation 1080p"
    int source_index = -1; // variants[] 或 representations[] 的下标
    int64_t bandwidth_bps = 0;
    int64_t average_bandwidth_bps = 0;
    int width = 0;
    int height = 0;
    std::string codecs;
    std::string video_codec;
    std::string audio_codec;
    std::string container_hint; // "fMP4/CMAF" / "MPEG-TS"
    int segment_count = 0;
    double avg_segment_duration_s = 0.0;
    double max_segment_duration_s = 0.0;
    bool has_init_section = false;
    // 关键帧（段起点）时间轴，秒。用于多码率对齐比对。
    std::vector<double> keyframe_times;
};

// ---------------------------------------------------------------------------
// 统一结果
// ---------------------------------------------------------------------------
struct StreamingPackageResult {
    std::string manifest_path;
    std::string manifest_dir; // 相对 URI 的解析基准（本地目录）
    StreamingKind kind = StreamingKind::Unknown;
    bool valid = false;
    std::string error_message;
    bool truncated = false; // 任一处被 max_* 上限截断
    bool remote = false;    // manifest 里出现 http(s) URI（第一阶段不下载）

    // ---- HLS ----
    std::vector<HlsVariantInfo> variants;
    std::vector<HlsRenditionInfo> renditions;
    std::vector<MediaPlaylistInfo> playlists;

    // ---- DASH ----
    std::string mpd_type; // "static" / "dynamic"
    std::string mpd_profiles;
    double media_presentation_duration_s = 0.0;
    double max_segment_duration_s = 0.0;
    double min_buffer_time_s = 0.0;
    std::vector<DashPeriodInfo> periods;
    // 展平后的 representation 列表（UI 与 ladder 直接消费）
    std::vector<DashRepresentationInfo> representations;

    // ---- 统一 ladder（由 SegmentQcAnalyzer::BuildLadder 填充）----
    std::vector<StreamingLadderEntry> ladder;

    std::vector<StreamingIssue> issues;

    // ---- 便捷访问 ----
    bool IsHls() const {
        return kind == StreamingKind::HlsMaster || kind == StreamingKind::HlsMedia;
    }
    bool IsDash() const {
        return kind == StreamingKind::Dash;
    }
    bool IsLive() const {
        if (IsDash())
            return mpd_type == "dynamic";
        for (const auto& p : playlists) {
            if (p.IsLive())
                return true;
        }
        return false;
    }
    int CountIssues(IssueSeverity severity) const {
        int n = 0;
        for (const auto& i : issues) {
            if (i.severity == severity)
                ++n;
        }
        return n;
    }
    bool HasIssue(const std::string& code) const {
        for (const auto& i : issues) {
            if (i.code == code)
                return true;
        }
        return false;
    }
    const StreamingIssue* FindIssue(const std::string& code) const {
        for (const auto& i : issues) {
            if (i.code == code)
                return &i;
        }
        return nullptr;
    }
    // 全部完整分片的个数（含所有 variant/representation）
    size_t TotalSegments() const {
        size_t n = 0;
        for (const auto& p : playlists)
            n += static_cast<size_t>(p.SegmentCount());
        for (const auto& r : representations)
            n += r.segments.size();
        return n;
    }
    // 问题码前缀：HLS 包用 container.hls.，DASH 包用 container.dash.
    std::string IssuePrefix() const {
        return IsDash() ? "container.dash." : "container.hls.";
    }
};

} // namespace model
} // namespace videoeye
