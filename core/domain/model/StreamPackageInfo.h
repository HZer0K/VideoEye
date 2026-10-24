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
#include <utility>
#include <vector>

#include "core/domain/model/DiagnosticIssue.h"
#include "core/domain/model/SegmentInfo.h"

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
// EXT-X-DATERANGE（广告/章节标记）定位不了结束点：既没有 END-DATE/DURATION，
// 也没有 END-ON-NEXT + CLASS 的"下一段开始即结束"语义。
constexpr const char* kHlsDateRangeOpenEnded = "container.hls.date_range_open_ended";
// EXT-X-DATERANGE 的 START-DATE 不是合法 ISO 8601 —— 广告插入点无法换算成时间。
constexpr const char* kHlsDateRangeInvalidDate = "container.hls.date_range_invalid_date";
// EXT-X-PROGRAM-DATE-TIME 无法解析，或与 EXTINF 累计时长之间出现了无法解释的跳变。
constexpr const char* kHlsProgramDateTimeInvalid = "container.hls.program_date_time_invalid";
constexpr const char* kHlsProgramDateTimeDrift = "container.hls.program_date_time_drift";
// EXT-X-I-FRAME-STREAM-INF 声明了快进档但给不出可用信息（缺 URI / 缺 BANDWIDTH /
// 没有任何主 variant 与它对应）。
constexpr const char* kHlsIframeVariantIncomplete = "container.hls.iframe_variant_incomplete";
// EXT-X-START 的 TIME-OFFSET 超出了这份清单能覆盖的时长，起点落空。
constexpr const char* kHlsStartOffsetOutOfRange = "container.hls.start_offset_out_of_range";
// EXT-X-SKIP（delta update）没有 EXT-X-MEDIA-SEQUENCE：被跳过的分片无法定位。
constexpr const char* kHlsDeltaUpdateWithoutSequence = "container.hls.delta_update_without_sequence";

// ---- DASH 专用 ----
constexpr const char* kDashSegmentTimelineGap = "container.dash.segment_timeline_gap";
constexpr const char* kDashSegmentTimelineOverlap = "container.dash.segment_timeline_overlap";
constexpr const char* kDashMissingSegmentInfo = "container.dash.missing_segment_info";
constexpr const char* kDashBandwidthMismatch = "container.dash.representation_bandwidth_mismatch";
constexpr const char* kDashVariantResolutionMismatch = "container.dash.representation_resolution_mismatch";
constexpr const char* kDashVariantCodecMismatch = "container.dash.representation_codec_mismatch";
constexpr const char* kDashVariantKeyframeMisalign = "container.dash.variant_keyframe_misalign";
constexpr const char* kDashAvSegmentCountMismatch = "container.dash.av_segment_count_mismatch";

// ---- DASH 描述符（ContentProtection / Role / EssentialProperty / UTCTiming / EventStream）----
// 加密流只登记不解密，所以"有 DRM"本身是 Info；真正的问题在下面几条里。
constexpr const char* kDashDrmContentProtection = "container.dash.content_protection";
// CENC（mp4protection）声明了加密却没有 default_KID —— 客户端不知道用哪把钥匙。
constexpr const char* kDashContentProtectionMissingKeyId = "container.dash.content_protection_missing_key_id";
// 只有无法识别的 schemeIdUri（既不是 mp4protection 也不是已知 DRM 系统）。
constexpr const char* kDashContentProtectionUnknownScheme = "container.dash.content_protection_unknown_scheme";
// 直播 MPD 没有 UTCTiming：客户端只能信本地时钟，起播点与时钟恢复都会偏。
constexpr const char* kDashLiveMissingUtcTiming = "container.dash.live_missing_utc_timing";
// UTCTiming 的 schemeIdUri / value 不完整，等同于没声明。
constexpr const char* kDashUtcTimingInvalid = "container.dash.utc_timing_invalid";
// EventStream / Event 自己不自洽（timescale=0、既有 presentationTime 又缺 duration、messageData 空）。
constexpr const char* kDashEventStreamInvalid = "container.dash.event_stream_invalid";
// 同一 Period 内有多条同类型流却没有 Role 标注，客户端无法确定默认轨。
constexpr const char* kDashMissingRole = "container.dash.missing_role";
// EssentialProperty 声明了客户端"必须理解"的扩展，本工具只登记不解释。
constexpr const char* kDashEssentialProperty = "container.dash.essential_property";
// EssentialProperty 的 schemeIdUri 为空，等价于一条无法处理的强制扩展。
constexpr const char* kDashEssentialPropertyUnknown = "container.dash.essential_property_unknown";
// 含事件流（DASH 侧的广告/章节标记，SCTE-35 over DASH 走这条）。
constexpr const char* kDashEventStream = "container.dash.event_stream";

// ---- 直播（live）语义 ----
// 直播包与点播包的差别不在于清单格式，而在于「这份清单只是一段时间的快照」：
//   - HLS：滑窗每次重载都会淘汰头部旧分片，序号必须靠 EXT-X-MEDIA-SEQUENCE 标识；
//   - DASH：dynamic MPD 靠 availabilityStartTime + minimumUpdatePeriod 描述
//     "此刻哪些分片可用、多久重载一次"，缺失其一客户端就无从算可用性；
//   - 两者：旧分片被淘汰造成的文件缺失是正常现象，不能按点播口径报"分片丢失"。
// 这一类检查产出的 finding 单独给 code，才能在规则表里独立调整级别/开关。
constexpr const char* kDashLiveMissingAvailabilityStartTime = "container.dash.live_missing_availability_start_time";
constexpr const char* kDashLiveMissingMinimumUpdatePeriod = "container.dash.live_missing_minimum_update_period";
constexpr const char* kDashLiveUpdatePeriodTooLong = "container.dash.live_update_period_too_long";
constexpr const char* kDashLiveMissingTimeShiftBuffer = "container.dash.live_missing_time_shift_buffer_depth";
constexpr const char* kDashLiveSegmentCountUnknown = "container.dash.live_segment_count_unknown";
constexpr const char* kHlsLiveMissingMediaSequence = "container.hls.live_missing_media_sequence";

// ---- 通用 ----
constexpr const char* kSegmentMissingFile = "container.streaming.segment_missing_file";
constexpr const char* kSegmentContainerInvalid = "container.streaming.segment_container_invalid";
// 直播窗口滚动时被淘汰的旧分片（ NOT a defect）—— 与上面的"真的找不到"分开计，
// 否则每次分析直播包都会被一条无法修复的告警刷屏。
constexpr const char* kSegmentEvictedLiveWindow = "container.streaming.segment_evicted_live_window";
// 分片首样本实证为 non-sync：该段不能独立起播（切档/随机访问会花屏）。
// 与"未实证"必须分开：前者是缺陷、后者是覆盖不足。
constexpr const char* kSegmentStartsNonSync = "container.streaming.segment_starts_non_sync";
constexpr const char* kSegmentKeyframeUnverified = "container.streaming.segment_keyframe_unverified";
// 清单**声明**了独立分片（HLS EXT-X-INDEPENDENT-SEGMENTS / DASH startWithSAP<=1），
// 但实测有分片首样本是 non-sync —— 声明与内容互相矛盾。
//
// 为什么不与 kSegmentStartsNonSync 合并：两者的修复动作不同。前者要改编码器
// （分片边界对齐 IDR），后者还要改清单（或者说明内容被替换过、清单是从别处抄来的）。
// 严重度用 Warning 而非 Error：内容的硬问题已经被 kSegmentStartsNonSync 计入 Error，
// 这里只是补充"清单层面也不一致"，重复计 Error 会把一个缺陷算成两个。
constexpr const char* kSegmentDeclarationContradicts = "container.streaming.segment_declaration_contradicts";
// 实测全部达标（每片首样本都是 sync），但清单没声明独立分片。
// 不是缺陷：内容质量没问题，只是清单没给出客户端可以依赖的承诺，
// 播放器就不能据此做无边界的码率切换/拼接优化。
constexpr const char* kSegmentIndependenceUndeclared = "container.streaming.segment_independence_undeclared";
// 跨分片时间轴实证（用 tfdt 与段内实测时长，不是清单声明的 EXTINF）：
//   * gap     —— 本段起点晚于上段终点：中间有媒体时间没有被任何分片覆盖（丢样本）
//   * overlap —— 本段起点早于上段终点：两段覆盖了同一段时间（重复样本）
//   * regress —— 本段起点早于上段起点且无 DISCONTINUITY：时基倒退
// 与 DASH 的 segment_timeline_gap 是不同数据源：那个比对 <S t/d/r> 声明，
// 这个比对文件里的 tfdt。清单正确而封装器写错 tfdt 的情况只有这条能抓到。
constexpr const char* kSegmentTimelineGapActual = "container.streaming.segment_timeline_gap_actual";
constexpr const char* kSegmentTimelineOverlapActual = "container.streaming.segment_timeline_overlap_actual";
constexpr const char* kSegmentTimelineRegress = "container.streaming.segment_timeline_regress";
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

// 一处 EXT-X-PROGRAM-DATE-TIME 采样。
//
// 只展示第一处就够了（后面的都由 EXTINF 推算），但要判"墙钟与媒体时间轴有没有
// 走偏"就必须把所有采样都比一遍 —— 所以单独留这个结构。
struct HlsProgramDateTimeSample {
    int segment_index = -1;        // 紧跟其后的分片下标（-1 = 末尾悬挂）
    double relative_seconds = 0.0; // 该分片在清单里的起点（秒）
    double epoch_seconds = 0.0;    // 原文换算出的 UTC 秒
};

// EXT-X-DATERANGE 一条（广告 / 章节 / 节目边界标记）
//
// 它既是"业务时间轴"，也是 SCTE-35 的唯一合法载体：广告系统在插播点写
// SCTE35-OUT/IN，播放器据此触发插播。过去这个标签完全没解析，于是
// "这份清单有没有广告位、广告位能不能定位到结束点"在报告里是空洞。
struct HlsDateRangeInfo {
    int index = -1;
    std::string id;         // ID（同一 CLASS 内唯一）
    std::string class_id;   // CLASS
    std::string start_date; // START-DATE 原文（ISO 8601，必填）
    std::string end_date;   // END-DATE 原文（可选）
    double duration_seconds = 0.0;
    bool has_duration = false;
    double planned_duration_seconds = 0.0;
    bool has_planned_duration = false;
    bool end_on_next = false; // END-ON-NEXT=YES：下一段 START-DATE 即本段结束
    bool has_scte35_out = false;
    bool has_scte35_in = false;
    bool has_scte35_cmd = false;
    std::string scte35_cmd; // 原样保留（base64/hex）
    std::string scte35_out;
    std::string scte35_in;
    // 额外属性（X- 前缀等）原文，供 UI 展示，键值对按出现顺序存
    std::vector<std::pair<std::string, std::string>> client_attributes;

    int playlist_index = -1;
    int segment_index = -1;   // 标记落在哪个分片之前（-1 = 无法对应到分片）
    double at_seconds = -1.0; // 相对清单起点的秒（能算出时 >= 0）

    bool HasScte35() const {
        return has_scte35_out || has_scte35_in || has_scte35_cmd;
    }
    // 这一段有没有一个可判定的结束点
    bool HasDefiniteEnd() const {
        return !end_date.empty() || has_duration || end_on_next;
    }
};

// EXT-X-STREAM-INF 一行（一条码率流）
struct HlsVariantInfo {
    int index = -1;
    std::string uri; // 子播放列表 URI
    // EXT-X-I-FRAME-STREAM-INF（快进/拖动用的纯 I 帧档）也落在同一个 vector 里，
    // 靠 iframe_only 区分：URI 写在标签属性上（没有紧随其后的一行）。
    bool iframe_only = false;
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
    std::string playlist_type;      // EXT-X-PLAYLIST-TYPE 原文（小写 vod / event），空 = 未声明
    bool has_endlist = false;       // EXT-X-ENDLIST（有即为点播，否则当作直播）
    // EXT-X-INDEPENDENT-SEGMENTS：每个媒体分片都可以不依赖其它分片独立解码。
    //
    // 这是**声明**，不是证据。规范（RFC 8216 §4.3.5.1）只要求播放器据此可以
    // 在任意分片边界拼接/切换，内容是否真的做到要另看 sample_flags。
    // 2026-10 之前这个字段解析了就没人读 —— 等于把「清单承诺」这半边证据丢掉了。
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

    // ---- EXT-X-PROGRAM-DATE-TIME（墙钟）----
    // 只记第一处（规范上它出现在某个分片之前，之后的分片按 EXTINF 累加推算）。
    // epoch 是把原文字符串按"YYYY-MM-DDThh:mm:ss[.fff]Z|±hh:mm"算出来的 UTC 秒；
    // 解析失败时 has_program_date_time 仍为 true（原文在），valid 为 false ——
    // 这两种情况在报告里要分开说："没写"和"写了但不合法"。
    bool has_program_date_time = false;
    bool program_date_time_valid = false;
    std::string program_date_time;
    double program_date_time_epoch_s = 0.0;
    std::vector<HlsProgramDateTimeSample> program_date_time_samples;

    // ---- EXT-X-START（推荐起点）----
    bool has_start = false;
    double start_time_offset_s = 0.0;
    bool start_precise = false;

    // ---- EXT-X-SKIP（delta update）----
    // 出现即表示这份播放列表是"相对于上一份的增量"：SKIPPED-SEGMENTS 个分片
    // 被省略，客户端必须靠 EXT-X-MEDIA-SEQUENCE 才能把它们补回时间轴。
    bool has_skip = false;
    int64_t skipped_segments = 0;

    // ---- EXT-X-DATERANGE ----
    std::vector<HlsDateRangeInfo> date_ranges;

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
    // 真正的"滑窗直播"：既不是 VOD，也不是只增不减的 EVENT，且没有 ENDLIST。
    //
    // 只有这一种清单每次重载都会淘汰头部旧分片 —— 于是两个后果:
    //   1) 序号会从 media_sequence 起算，缺 EXT-X-MEDIA-SEQUENCE 就无法区分新旧同序号分片；
    //   2) 头部若干分片在磁盘上找不到（已被淘汰），不是缺陷。
    // EVENT 列表只会不断追加，缺少 MEDIA-SEQUENCE 没有这两重影响，不该拿它去刷告警。
    bool IsSlidingWindowLive() const {
        return IsLive() && playlist_type != "event";
    }
    // 窗口时长：这份快照实际覆盖的秒数。直播侧的真实缓冲长度由服务端决定，
    // 离线单张快照只能给出"清单里这些分片合计多长"这一个可观测值。
    double WindowSeconds() const {
        return total_duration_seconds;
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

// <Role schemeIdUri="urn:mpeg:dash:role:2011" value="main"/> 与
// <EssentialProperty schemeIdUri="..." value="..."/> 共用同一个形状：
// DASH 里这一类"scheme + value"的描述符（Role / EssentialProperty /
// SupplementalProperty / Accessibility）字段只有这两个，没必要各写一个结构。
struct DashDescriptor {
    std::string scheme_id_uri;
    std::string value;
    std::string id; // 少数描述符带 id
};

// <ContentProtection>（DRM）。加密流以前只能看到一个空壳：Representation 照旧、
// 分片照旧，但"这是不是加密流、用哪个 DRM 系统、默认 KID 是多少"全部丢失。
struct DashContentProtectionInfo {
    std::string scheme_id_uri; // urn:mpeg:dash:mp4protection:2011 / widevine / playready ...
    std::string value;         // mp4protection 的 value 即保护方案（cenc / cbcs / cens）
    std::string default_kid;   // cenc:default_KID
    std::string robustness;    // 少数系统在子元素里给
    bool has_pssh = false;     // 有没有 <cenc:pssh>（缺它只能靠清单外的带外约定）

    // mp4protection 是"这是 CENC 加密"的通用声明；其余是具体 DRM 系统
    bool IsCencDeclaration() const {
        return scheme_id_uri == "urn:mpeg:dash:mp4protection:2011";
    }
};

// <UTCTiming>（直播对时）
struct DashUtcTimingInfo {
    std::string scheme_id_uri; // urn:mpeg:dash:utc:direct:2014 / .../http-head:2014 / ntp ...
    std::string value;
};

// <Event>/<EventStream>（广告 / SCTE-35 / 章节）
struct DashEventInfo {
    bool has_presentation_time = false;
    double presentation_time_s = 0.0;
    bool has_duration = false;
    double duration_s = 0.0;
    std::string id;
    bool has_message_data = false;
};

struct DashEventStreamInfo {
    int period_index = -1;
    std::string scheme_id_uri;
    std::string value;
    uint32_t timescale = 1;
    std::vector<DashEventInfo> events;
    bool truncated = false;
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
    // startWithSAP：与 DashAdaptationSetInfo 同名字段同义，Representation 未声明时
    // 从 AdaptationSet 继承（见 DashManifestAnalyzer 的继承段）。-1 = 未声明。
    int start_with_sap = -1;

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

    // ---- 描述符（继承自 AdaptationSet，Representation 上也可以各自声明）----
    std::vector<DashDescriptor> roles;
    std::vector<DashDescriptor> essential_properties;
    std::vector<DashContentProtectionInfo> content_protections;
    bool has_content_protection = false;

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
    // startWithSAP（ISO/IEC 23009-1 §5.3.7.3）：声明每个媒体分片的第一个媒体段
    // 达到 SAP 类型 startWithSAP 或更低。startWithSAP="1" 即「分片以瞬时解码器刷新
    // 点（IDR 级）开头」，是 DASH 侧对应 EXT-X-INDEPENDENT-SEGMENTS 的声明。
    // -1 = 未声明。规范允许省略，但省略时客户端**不能**假定分片可独立切换，
    // 所以「未声明」和「声明为 0」是两种不同情况，必须分开存。
    int start_with_sap = -1;
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

    // ---- 描述符（Representation 未声明时继承这里的值）----
    std::vector<DashDescriptor> roles;
    std::vector<DashDescriptor> essential_properties;
    std::vector<DashContentProtectionInfo> content_protections;
    bool has_content_protection = false;
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
    // master playlist 里的 EXT-X-INDEPENDENT-SEGMENTS（RFC 8216 §4.3.5.1）。
    // 单独存而不下发给每个 MediaPlaylistInfo：media playlist 自己也声明时，
    // 那份解析结果会覆盖 pl.independent_segments，「声明来源」就丢了。
    // 判定独立分片要取 (pl.independent_segments || 本字段)，报告里两者分开说。
    bool master_independent_segments = false;

    // ---- DASH ----
    std::string mpd_type; // "static" / "dynamic"
    std::string mpd_profiles;
    double media_presentation_duration_s = 0.0;
    double max_segment_duration_s = 0.0;
    double min_buffer_time_s = 0.0;

    // ---- DASH 直播（type="dynamic"）语义 ----
    // availabilityStartTime 是 dynamic MPD 的必填项：客户端用它加上分片起点算出
    // "这一片现在能不能取"。保留原文而不换算成 epoch —— 跨时区/闰秒的换算结果与
    // 播放器口径可能不一致，校验只需要"有没有声明"，展示只需要原文。
    std::string availability_start_time;
    bool has_availability_start_time = false;
    std::string publish_time; // MPD@publishTime（最近一次发布时间，原文）
    // minimumUpdatePeriod：客户端应当隔多久重载一次 MPD。比单个分片还长的话，
    // 播放器在两次重载之间可能整段新分片都拿不到。
    double minimum_update_period_s = 0.0;
    bool has_minimum_update_period = false;
    // timeShiftBufferDepth：服务端保留多久的历史分片（即可回退的时移窗口）。
    // 它同时是"这份快照大致覆盖多长"的唯一线索 —— dynamic MPD 通常不写 Period@duration。
    double time_shift_buffer_depth_s = 0.0;
    bool has_time_shift_buffer_depth = false;
    double suggested_presentation_delay_s = 0.0; // 推荐的直播时延
    bool has_suggested_presentation_delay = false;

    // ---- DASH 描述符 ----
    // UTCTiming 在 <MPD> 下，可以有多个（按顺序尝试）。
    std::vector<DashUtcTimingInfo> utc_timings;
    // EventStream 在 <Period> 下，Element 里可能带很多 Event —— 用 max_events 截断。
    std::vector<DashEventStreamInfo> event_streams;
    bool event_streams_truncated = false;

    // 给定的线索里能不能推出本次快照的可用窗口长度（秒）。推不出来就是 0。
    double EffectiveWindowSeconds() const {
        if (media_presentation_duration_s > 0.0)
            return media_presentation_duration_s;
        return has_time_shift_buffer_depth ? time_shift_buffer_depth_s : 0.0;
    }
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
    // 有没有加密（HLS 的 EXT-X-KEY / DASH 的 ContentProtection 任一出现）。
    // 报告里"分片不可读、码流类结论仅供参考"这句提示要靠它来触发。
    bool IsEncrypted() const {
        if (IsDash()) {
            for (const auto& r : representations) {
                if (r.has_content_protection)
                    return true;
            }
            return false;
        }
        for (const auto& p : playlists) {
            if (p.encrypted)
                return true;
        }
        return false;
    }
    // EXT-X-DATERANGE 总数（广告/章节标记）
    size_t DateRangeCount() const {
        size_t n = 0;
        for (const auto& p : playlists)
            n += p.date_ranges.size();
        return n;
    }
    // EXT-X-I-FRAME-STREAM-INF 档数（快进/拖动用）
    size_t IframeVariantCount() const {
        size_t n = 0;
        for (const auto& v : variants) {
            if (v.iframe_only)
                ++n;
        }
        return n;
    }
    size_t EventCount() const {
        size_t n = 0;
        for (const auto& es : event_streams)
            n += es.events.size();
        return n;
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
    // 真正做过落盘探测 / 容器头解析的分片数。
    // 与 TotalSegments() 配对使用：前者是"这一轮真看过的"，后者是"清单里声称存在的"。
    // 探测是有上限的（SegmentQcOptions::max_probe_segments，TS 深度解析还另有更严的
    // 上限），所以两者不相等是常态 —— UI 与报告必须把它当抽样结果如实标出来，
    // 不能让"N 个分片"看起来像"N 个都验过"。
    size_t ProbedSegments() const {
        size_t n = 0;
        for (const auto& p : playlists) {
            for (const auto& s : p.segments) {
                if (!s.partial && s.probed)
                    ++n;
            }
        }
        for (const auto& r : representations) {
            for (const auto& s : r.segments) {
                if (!s.partial && s.probed)
                    ++n;
            }
        }
        return n;
    }
    // 抽查是否覆盖了清单里的全部分片。false 表示结果是抽样，不代表没有缺陷。
    bool SegmentProbeComplete() const {
        return ProbedSegments() >= TotalSegments();
    }
    // 问题码前缀：HLS 包用 container.hls.，DASH 包用 container.dash.
    std::string IssuePrefix() const {
        return IsDash() ? "container.dash." : "container.hls.";
    }
};

} // namespace model
} // namespace videoeye
