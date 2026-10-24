#include "core/analysis/streaming/SegmentQcAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/analysis/streaming/StreamingCancel.h"
#include "core/media/streaming/ManifestText.h"

namespace videoeye {
namespace {

namespace mt = videoeye::manifest;

std::string FormatSeconds(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds);
    return std::string(buf);
}

std::string FormatKbps(int64_t bps) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f kbps", static_cast<double>(bps) / 1000.0);
    return std::string(buf);
}

void PushIssue(model::StreamingPackageResult& out, const char* code, const char* title, const std::string& detail,
               const std::string& suggestion, model::IssueSeverity severity, double metric_value, double threshold,
               int occurrence_count, double at_seconds, int variant_index) {
    model::StreamingIssue issue;
    issue.code = code;
    issue.title = title;
    issue.detail = detail;
    issue.suggestion = suggestion;
    issue.severity = severity;
    issue.category = model::IssueCategory::Container;
    issue.metric_value = metric_value;
    issue.threshold = threshold;
    issue.occurrence_count = occurrence_count;
    issue.at_seconds = at_seconds;
    issue.variant_index = variant_index;
    out.issues.push_back(std::move(issue));
}

// 按当前包的协议挑问题码（HLS 与 DASH 语义相同，只是前缀不同）
const char* Code(const model::StreamingPackageResult& out, const char* hls_code, const char* dash_code) {
    return out.IsDash() ? dash_code : hls_code;
}

// 统计一个码率层的缺失分片，并把"直播滑窗淘汰的旧分片"单独挑出来。
//
// 判据：淘汰永远发生在窗口头部，真正传丢的分片前后都还有找得到的兄弟。
// 所以"排在第一个找得到的分片之前的那段连续缺失"按淘汰计；一旦某个分片找得到，
// 它后面再缺就一定是真丢。整条一层都找不到时不适用（那多半是路径不对，
// 不是窗口滚动），否则任何坏包都会被这条规则洗白。
void ScanMissing(const std::vector<model::SegmentInfo>& segments, bool live_window, int& missing, int& evicted,
                 std::string& first_missing) {
    int existing = 0;
    for (const model::SegmentInfo& seg : segments) {
        if (seg.partial || seg.resolved_path.empty())
            continue;
        if (seg.exists)
            ++existing;
    }
    const bool all_missing = (existing == 0);
    bool seen_existing = false;

    for (const model::SegmentInfo& seg : segments) {
        if (seg.partial || seg.resolved_path.empty())
            continue;
        if (seg.exists) {
            seen_existing = true;
            continue;
        }
        if (live_window && !all_missing && !seen_existing) {
            ++evicted;
            continue;
        }
        ++missing;
        if (first_missing.empty())
            first_missing = seg.uri;
    }
}

// ---------------------------------------------------------------------------
// fMP4 探测
// ---------------------------------------------------------------------------

// 从初始化段（含 moov）拿 timescale；拿不到返回 0
//
// cancel: 只用来决定"要不要开工"。初始化段是单个文件，比整包探测短得多，
// 但大包（上百个分片）叠起来也必须能被一次取消收住 —— 所以这里一旦看到取消
// 就直接返回 0，让它由 ProbeSegmentRange 的外层循环收尾。
uint32_t ProbeInitTimescale(const std::string& init_path, const std::atomic<bool>* cancel) {
    if (init_path.empty())
        return 0;
    if (IsStreamingCancelled(cancel))
        return 0;
    Mp4SampleTableOptions opt;
    opt.expand_samples = false;
    opt.max_samples_per_track = 1;
    model::Mp4SampleTableResult res;
    Mp4SampleTableAnalyzer analyzer;
    if (!analyzer.AnalyzeFile(init_path, res, opt, cancel))
        return 0;
    uint32_t timescale = 0;
    for (const model::Mp4TrackSampleTable& t : res.tracks) {
        if (t.type == "video" && t.media_timescale > 0)
            return t.media_timescale;
        if (timescale == 0 && t.media_timescale > 0)
            timescale = t.media_timescale;
    }
    return timescale;
}

// 探测一个 fMP4 媒体分片：tfdt 给起点时间，sample_flags 给"能不能独立解码"
//
// ❗ 这两件事必须分开取证。旧实现拿到 tfdt 就置 starts_with_keyframe=true，
// 依据是"CMAF 分片必须以 IDR 开头"这条**约定**；一旦上游切档/拼接没遵守约定，
// 结论依然是"有随机访问点"，问题就被永久掩盖。现在改成：
//   有 sample_flags  → 按 sample_is_non_sync_sample 实证（SyncFlag / NotSync）
//   没有 sample_flags → 只能标 Declared（仍置 true 但证据等级不同）
bool ProbeFmp4Segment(model::SegmentInfo& seg, uint32_t fallback_timescale, const std::atomic<bool>* cancel) {
    if (IsStreamingCancelled(cancel))
        return false; // 别开工：开了也只是白跑一趟然后被外层丢掉
    Mp4SampleTableOptions opt;
    opt.expand_samples = false; // 只要 moof/traf/tfdt，展开样本表是纯浪费
    opt.max_samples_per_track = 1;
    model::Mp4SampleTableResult res;
    Mp4SampleTableAnalyzer analyzer;
    if (!analyzer.AnalyzeFile(seg.resolved_path, res, opt, cancel)) {
        // 解析失败要记账（UI 上"分片容器解析失败"这条 issue 就是它）；
        // 被取消则不清账 —— 取消不是分片的问题，不该让用户看到假问题。
        if (!IsStreamingCancelled(cancel)) {
            seg.container_parse_failed = true;
            seg.container_error = res.error_message;
        }
        return false;
    }
    if (res.fragments.empty())
        return false;

    uint32_t timescale = fallback_timescale;
    if (timescale == 0) {
        for (const model::Mp4TrackSampleTable& t : res.tracks) {
            if (t.media_timescale > 0) {
                timescale = t.media_timescale;
                break;
            }
        }
    }
    // 取最早的 tfdt（多 traf 时第一个 moof 里的最小起点）。
    // 同时记住这一条 fragment —— 首样本标志只能从**它**读，不能从别的 traf 上读。
    const model::Mp4FragmentInfo* first = nullptr;
    uint64_t best = 0;
    for (const model::Mp4FragmentInfo& f : res.fragments) {
        if (!f.has_tfdt)
            continue;
        if (first == nullptr || f.base_media_decode_time < best) {
            best = f.base_media_decode_time;
            first = &f;
        }
    }
    if (first == nullptr || timescale == 0)
        return false;

    seg.timescale = timescale;
    seg.decode_time = best;
    seg.has_decode_time = true;
    seg.start_seconds = static_cast<double>(best) / static_cast<double>(timescale);

    // 段内实际媒体时长：与清单声明的 duration 分账，供跨段连续性实证用。
    if (first->duration > 0) {
        seg.media_duration_seconds = static_cast<double>(first->duration) / static_cast<double>(timescale);
        seg.has_media_duration = true;
    }

    if (first->has_first_sample_flags) {
        seg.has_first_sample_flags = true;
        seg.first_sample_flags = first->first_sample_flags;
        seg.first_sample_is_sync = first->first_sample_is_sync;
        seg.first_sample_depends_on = first->first_sample_depends_on;
        seg.starts_with_keyframe = first->first_sample_is_sync;
        seg.keyframe_evidence =
            first->first_sample_is_sync ? model::KeyframeEvidence::SyncFlag : model::KeyframeEvidence::NotSync;
        if (first->first_sample_is_sync)
            seg.first_frame_type = (first->first_sample_depends_on == 2) ? "I" : "sync";
        else
            seg.first_frame_type = (first->first_sample_depends_on == 1) ? "P/B" : "non-sync";
    } else {
        // 没有 flags 来源：CMAF 的缺省值在 init 段的 trex 里，单文件视角看不到。
        // 保持旧行为（按约定视为真），但把证据等级降到 Declared，
        // 让上层能把"已验证"和"按约定假定"分开统计。
        seg.starts_with_keyframe = true;
        seg.keyframe_evidence = model::KeyframeEvidence::Declared;
        seg.first_frame_type.clear();
    }
    return true;
}

// 探测一组分片（HLS 的一个 media playlist / DASH 的一个 representation）
void ProbeSegmentRange(std::vector<model::SegmentInfo>& segments, uint32_t init_timescale,
                       const SegmentQcOptions& options, const std::atomic<bool>* cancel) {
    uint32_t probed = 0;
    for (model::SegmentInfo& seg : segments) {
        // 这层是逐分片的磁盘 stat + fMP4 头解析，大型包最耗时的就是它
        if (ShouldCheckStreamingCancel(probed) && IsStreamingCancelled(cancel))
            return;
        if (seg.partial)
            continue;
        if (probed >= options.max_probe_segments)
            break;
        ++probed;
        if (seg.resolved_path.empty())
            continue;
        seg.probed = true;
        seg.exists = mt::FileSizeOf(seg.resolved_path, seg.file_size_bytes);
        if (!seg.exists)
            continue;
        if (seg.container == model::SegmentContainer::FMP4) {
            ProbeFmp4Segment(seg, init_timescale, cancel);
        }
    }
}

// ---------------------------------------------------------------------------
// ladder
// ---------------------------------------------------------------------------

std::string ContainerHintOf(const std::vector<model::SegmentInfo>& segments) {
    for (const model::SegmentInfo& seg : segments) {
        if (seg.partial)
            continue;
        if (seg.container != model::SegmentContainer::Unknown)
            return seg.ContainerName();
    }
    return std::string();
}

std::vector<double> KeyframeTimesOf(const std::vector<model::SegmentInfo>& segments) {
    std::vector<double> times;
    for (const model::SegmentInfo& seg : segments) {
        if (seg.partial || seg.gap)
            continue;
        if (!seg.starts_with_keyframe)
            continue;
        times.push_back(seg.start_seconds);
    }
    std::sort(times.begin(), times.end());
    return times;
}

} // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

SegmentQcAnalyzer::StageStatus SegmentQcAnalyzer::ProbeSegments(model::StreamingPackageResult& result,
                                                                const SegmentQcOptions& options,
                                                                const std::atomic<bool>* cancel) {
    uint32_t probed_units = 0;
    const uint32_t kMaxUnits = 64; // package 级上限：再多的码率层也没必要全探测

    for (model::MediaPlaylistInfo& pl : result.playlists) {
        if (IsStreamingCancelled(cancel))
            return StageStatus::kCancelled;
        if (probed_units >= kMaxUnits)
            break;
        ++probed_units;
        uint32_t timescale = 0;
        if (pl.has_init_section && !pl.init_resolved_path.empty()) {
            timescale = ProbeInitTimescale(pl.init_resolved_path, cancel);
        }
        ProbeSegmentRange(pl.segments, timescale, options, cancel);
    }

    for (model::DashRepresentationInfo& rep : result.representations) {
        if (IsStreamingCancelled(cancel))
            return StageStatus::kCancelled;
        if (probed_units >= kMaxUnits)
            break;
        ++probed_units;
        uint32_t timescale = 0;
        if (!rep.init_resolved_path.empty()) {
            timescale = ProbeInitTimescale(rep.init_resolved_path, cancel);
        }
        ProbeSegmentRange(rep.segments, timescale, options, cancel);
    }

    if (IsStreamingCancelled(cancel))
        return StageStatus::kCancelled;
    return StageStatus::kDone;
}

void SegmentQcAnalyzer::BuildLadder(model::StreamingPackageResult& result) {
    result.ladder.clear();

    if (result.IsDash()) {
        // 先收视频轨；一个视频都没有时（纯音频包）再退回全部
        std::vector<const model::DashRepresentationInfo*> picked;
        for (const model::DashRepresentationInfo& rep : result.representations) {
            if (rep.content_type == "video")
                picked.push_back(&rep);
        }
        if (picked.empty()) {
            for (const model::DashRepresentationInfo& rep : result.representations) {
                picked.push_back(&rep);
            }
        }
        for (const model::DashRepresentationInfo* rep : picked) {
            model::StreamingLadderEntry entry;
            entry.label = "Representation " + (rep->id.empty() ? std::to_string(rep->index) : rep->id);
            entry.source_index = rep->index;
            entry.bandwidth_bps = rep->bandwidth_bps;
            entry.width = rep->width;
            entry.height = rep->height;
            entry.codecs = rep->codecs;
            entry.video_codec = rep->video_codec;
            entry.audio_codec = rep->audio_codec;
            entry.container_hint = ContainerHintOf(rep->segments);
            entry.segment_count = static_cast<int>(rep->segments.size());
            entry.avg_segment_duration_s = rep->AverageSegmentDuration();
            for (const model::SegmentInfo& seg : rep->segments) {
                if (seg.duration_seconds > entry.max_segment_duration_s) {
                    entry.max_segment_duration_s = seg.duration_seconds;
                }
            }
            entry.has_init_section = !rep->initialization_template.empty();
            entry.keyframe_times = KeyframeTimesOf(rep->segments);
            result.ladder.push_back(std::move(entry));
        }
        return;
    }

    // ---- HLS ----
    if (result.variants.empty()) {
        // 单个 media playlist：也给它一条 ladder，UI 不至于空着
        for (size_t i = 0; i < result.playlists.size(); ++i) {
            const model::MediaPlaylistInfo& pl = result.playlists[i];
            model::StreamingLadderEntry entry;
            entry.label = pl.role.empty() ? ("playlist #" + std::to_string(i)) : (pl.role + " #" + std::to_string(i));
            entry.source_index = static_cast<int>(i);
            entry.container_hint = ContainerHintOf(pl.segments);
            entry.segment_count = static_cast<int>(pl.SegmentCount());
            entry.avg_segment_duration_s = pl.AverageSegmentDuration();
            entry.max_segment_duration_s = pl.MaxSegmentDuration();
            entry.has_init_section = pl.has_init_section;
            entry.keyframe_times = KeyframeTimesOf(pl.segments);
            result.ladder.push_back(std::move(entry));
        }
        return;
    }

    for (const model::HlsVariantInfo& variant : result.variants) {
        // EXT-X-I-FRAME-STREAM-INF（纯 I 帧快进档）不是 ABR 的一级：
        // 它的分片时长天然与正片不同，混进 ladder 会让"关键帧不对齐"成片误报。
        if (variant.iframe_only)
            continue;
        model::StreamingLadderEntry entry;
        entry.label = "variant #" + std::to_string(variant.index);
        entry.source_index = variant.index;
        entry.bandwidth_bps = variant.bandwidth_bps;
        entry.average_bandwidth_bps = variant.average_bandwidth_bps;
        entry.width = variant.width;
        entry.height = variant.height;
        entry.codecs = variant.codecs;
        entry.video_codec = variant.video_codec;
        entry.audio_codec = variant.audio_codec;
        entry.has_init_section = false;
        if (variant.playlist_index >= 0 && static_cast<size_t>(variant.playlist_index) < result.playlists.size()) {
            const model::MediaPlaylistInfo& pl = result.playlists[variant.playlist_index];
            entry.segment_count = static_cast<int>(pl.SegmentCount());
            entry.avg_segment_duration_s = pl.AverageSegmentDuration();
            entry.max_segment_duration_s = pl.MaxSegmentDuration();
            entry.has_init_section = pl.has_init_section;
            entry.container_hint = ContainerHintOf(pl.segments);
            entry.keyframe_times = KeyframeTimesOf(pl.segments);
        }
        result.ladder.push_back(std::move(entry));
    }
}

SegmentQcAnalyzer::StageStatus SegmentQcAnalyzer::Validate(model::StreamingPackageResult& result,
                                                           const SegmentQcOptions& options,
                                                           const std::atomic<bool>* cancel) {
    static const std::vector<const char*> kOwnCodes = {
        model::StreamingIssueCode::kHlsVariantResolutionMismatch,
        model::StreamingIssueCode::kHlsVariantCodecMismatch,
        model::StreamingIssueCode::kHlsVariantKeyframeMisalign,
        model::StreamingIssueCode::kHlsAvSegmentCountMismatch,
        model::StreamingIssueCode::kHlsBandwidthMismatch,
        model::StreamingIssueCode::kDashVariantResolutionMismatch,
        model::StreamingIssueCode::kDashVariantCodecMismatch,
        model::StreamingIssueCode::kDashVariantKeyframeMisalign,
        model::StreamingIssueCode::kDashAvSegmentCountMismatch,
        model::StreamingIssueCode::kDashBandwidthMismatch,
        model::StreamingIssueCode::kSegmentMissingFile,
        model::StreamingIssueCode::kSegmentContainerInvalid,
        model::StreamingIssueCode::kSegmentEvictedLiveWindow,
        // 首帧实证 + 声明层对照（1b）
        model::StreamingIssueCode::kSegmentStartsNonSync,
        model::StreamingIssueCode::kSegmentKeyframeUnverified,
        model::StreamingIssueCode::kSegmentDeclarationContradicts,
        model::StreamingIssueCode::kSegmentIndependenceUndeclared,
        // 跨分片时间轴实证（1c）
        model::StreamingIssueCode::kSegmentTimelineGapActual,
        model::StreamingIssueCode::kSegmentTimelineOverlapActual,
        model::StreamingIssueCode::kSegmentTimelineRegress,
    };
    model::RemoveIssuesByCode(result.issues, kOwnCodes);

    if (result.ladder.empty())
        BuildLadder(result);

    // ---- 1) 分片文件缺失 / 容器解析失败 ----
    // 这里是全函数里迭代次数最多的地方: 分片数 × 码率层, 大包轻松上百万次。
    // 内层按 ShouldCheckStreamingCancel 的间隔查, 外层每个 playlist / representation 必查
    // (单条流也可能很长, 只查外层会漏掉"只有一条流但分片特别多"的包)。
    {
        int missing = 0;
        int invalid = 0;
        int evicted = 0;
        unsigned long long scanned = 0;
        std::string first_missing;
        for (const model::MediaPlaylistInfo& pl : result.playlists) {
            if (IsStreamingCancelled(cancel))
                return StageStatus::kCancelled;
            // 只有滑窗直播才会淘汰头部旧分片；EVENT / VOD 缺片一律按真缺失算。
            ScanMissing(pl.segments, pl.IsSlidingWindowLive(), missing, evicted, first_missing);
            for (const model::SegmentInfo& seg : pl.segments) {
                if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                    return StageStatus::kCancelled;
                if (seg.partial || seg.resolved_path.empty())
                    continue;
                if (seg.exists && seg.container_parse_failed)
                    ++invalid;
            }
        }
        for (const model::DashRepresentationInfo& rep : result.representations) {
            if (IsStreamingCancelled(cancel))
                return StageStatus::kCancelled;
            ScanMissing(rep.segments, result.IsLive(), missing, evicted, first_missing);
            for (const model::SegmentInfo& seg : rep.segments) {
                if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                    return StageStatus::kCancelled;
                if (seg.resolved_path.empty())
                    continue;
                if (seg.exists && seg.container_parse_failed)
                    ++invalid;
            }
        }
        if (missing > 0) {
            PushIssue(result, model::StreamingIssueCode::kSegmentMissingFile, "分片文件不存在",
                      "清单声明了 " + std::to_string(missing) +
                          " 个本地分片，但磁盘上找不到（首个缺失: " + first_missing + "）。",
                      "补齐分片文件或修正清单里的相对路径；若分片在远端，"
                      "请以完整包（含分片）的形式提供，当前版本不自动下载。",
                      model::IssueSeverity::Warning, static_cast<double>(missing), 0.0, missing, -1.0, -1);
        }
        if (evicted > 0) {
            PushIssue(result, model::StreamingIssueCode::kSegmentEvictedLiveWindow, "旧分片已随直播窗口滚动删除",
                      "有 " + std::to_string(evicted) +
                          " 个分片在清单里位于窗口头部但已不在磁盘上，"
                          "与滑窗直播的淘汰行为一致（这些分片对应更早的时间点）。",
                      "这不是缺陷：直播源只保留窗口长度内的分片，旧分片必然随时间被删除。"
                      "若要完整体检这段内容，请在它仍处于时移窗口内时导出整包，或直接分析录制归档件。",
                      model::IssueSeverity::Info, static_cast<double>(evicted), 0.0, evicted, -1.0, -1);
        }
        if (invalid > 0) {
            PushIssue(result, model::StreamingIssueCode::kSegmentContainerInvalid, "分片容器解析失败",
                      "有 " + std::to_string(invalid) +
                          " 个分片存在，但用已有的容器分析器（fMP4 走 IsobmffParser）解析失败。",
                      "用 VideoEye 单独打开该分片看具体报错；常见原因是分片被截断或缺少初始化段。",
                      model::IssueSeverity::Error, static_cast<double>(invalid), 0.0, invalid, -1.0, -1);
        }
    }

    // ---- 1b) 分片首帧随机访问实证 ----
    //
    // 放在"多码率"早退之前：单码率包同样需要这个结论（点播从第 N 片起播花屏，
    // 往往只是那一片首帧不是 IDR）。三件事分开报：
    //   * 实证到 non-sync   —— 硬问题：该段不能独立解码
    //   * 只有 Declared     —— 覆盖不足：结论没证据，不能当"已验证"
    //   * 有 flags 的段数   —— 让用户知道这一批里有多少是真验过的
    if (options.check_keyframe_alignment) {
        int probed_fmp4 = 0;
        int verified = 0;
        int unverified = 0;
        int non_sync = 0;
        std::string first_non_sync;
        double first_non_sync_at = -1.0;

        // ---- 声明层统计：清单承诺了什么 ----
        //
        // 这一层以前完全没被消费：HLS 的 EXT-X-INDEPENDENT-SEGMENTS 解析进
        // MediaPlaylistInfo 之后没有任何读者，DASH 的 startWithSAP 连解析都没有。
        // 少了它只能说"内容不能独立解码"，说不出"清单承诺过、内容却没做到"
        // —— 后者才是拼接/换源/清单被复用的直接线索。
        int declared_streams = 0;      // 声明了独立分片的 playlist / representation 数
        int contradicted_streams = 0;  // 其中实测有 non-sync 分片的
        int contradicted_segments = 0; // 这些流里 non-sync 分片总数
        std::string first_contradicted;
        int undeclared_streams = 0; // 实测全 sync、但清单未声明的流数
        int undeclared_segments = 0;
        std::string first_undeclared;

        unsigned long long scanned = 0;
        // declared_independent 由调用方按清单算好传入：
        //   HLS   —— media playlist 自己声明，或 master playlist 声明（后者对所有 variant 生效）
        //   DASH  —— startWithSAP ∈ {1,2}：1 = 分片以 IDR 开头，2 = 以 I 帧开头，
        //            两种都承诺"分片首帧可随机访问"；3/4 是更低等级的 SAP，不构成这个承诺
        auto scan = [&](const std::vector<model::SegmentInfo>& segments, bool declared_independent) {
            int local_probed = 0;
            int local_verified = 0;
            int local_non_sync = 0;
            std::string local_first_non_sync;
            std::string local_first_verified;
            for (const model::SegmentInfo& seg : segments) {
                if (seg.partial || seg.container != model::SegmentContainer::FMP4)
                    continue;
                if (!seg.probed || seg.container_parse_failed)
                    continue;
                ++probed_fmp4;
                ++local_probed;
                switch (seg.keyframe_evidence) {
                case model::KeyframeEvidence::SyncFlag:
                    ++verified;
                    ++local_verified;
                    if (local_first_verified.empty())
                        local_first_verified = seg.uri;
                    break;
                case model::KeyframeEvidence::Declared:
                    ++unverified;
                    break;
                case model::KeyframeEvidence::NotSync:
                    ++non_sync;
                    ++local_non_sync;
                    if (first_non_sync.empty()) {
                        first_non_sync = seg.uri;
                        first_non_sync_at = seg.start_seconds;
                    }
                    if (local_first_non_sync.empty())
                        local_first_non_sync = seg.uri;
                    break;
                default:
                    break;
                }
            }
            // 一条流里一个分片都没探到，就没有可对照的对象（不能当成"声明未兑现"）
            if (local_probed == 0)
                return;
            if (declared_independent) {
                ++declared_streams;
                if (local_non_sync > 0) {
                    ++contradicted_streams;
                    contradicted_segments += local_non_sync;
                    if (first_contradicted.empty())
                        first_contradicted = local_first_non_sync;
                }
            } else if (local_non_sync == 0 && local_verified > 0) {
                // 反向情况：实测全达标，清单没承诺。不是缺陷，但值得说一句。
                ++undeclared_streams;
                undeclared_segments += local_verified;
                if (first_undeclared.empty())
                    first_undeclared = local_first_verified;
            }
        };
        for (const model::MediaPlaylistInfo& pl : result.playlists) {
            if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                return StageStatus::kCancelled;
            scan(pl.segments, pl.independent_segments || result.master_independent_segments);
        }
        for (const model::DashRepresentationInfo& rep : result.representations) {
            if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                return StageStatus::kCancelled;
            scan(rep.segments, rep.start_with_sap >= 1 && rep.start_with_sap <= 2);
        }
        if (non_sync > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kSegmentStartsNonSync,
                           model::StreamingIssueCode::kSegmentStartsNonSync),
                      "分片首帧不是随机访问点",
                      "已实证 " + std::to_string(non_sync) +
                          " 个 fMP4 分片的首样本 "
                          "sample_is_non_sync_sample=1（首个: " +
                          first_non_sync +
                          "）。"
                          "这些分片无法独立起播：从该片开始播放会花屏/黑屏，直到下一个 IDR。",
                      "让编码器在每个分片起点强制 IDR（-force_key_frames 对齐分片边界，"
                      "或 -g 与分片时长严格对齐），并确认封装时没有把非关键帧切到段首。",
                      model::IssueSeverity::Error, static_cast<double>(non_sync), 0.0, non_sync, first_non_sync_at, -1);
        }
        if (unverified > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kSegmentKeyframeUnverified,
                           model::StreamingIssueCode::kSegmentKeyframeUnverified),
                      "分片首帧随机访问点未获实证",
                      "有 " + std::to_string(unverified) + "/" + std::to_string(probed_fmp4) +
                          " 个 fMP4 分片查不到首样本 sample_flags（trun 与 tfhd 都没给），"
                          "只能按「CMAF 分片必须 IDR 开头」的约定视为可独立解码。"
                          "这不等于验证通过。",
                      "这类分片通常靠初始化段的 trex 给默认 sample_flags；"
                      "把它与该分片的初始化段一起提供即可完成实证。",
                      model::IssueSeverity::Info, static_cast<double>(unverified), 0.0, unverified, -1.0, -1);
        }
        // 声明与内容矛盾：清单承诺了独立分片，内容却有非同步首帧。
        // 严重度给 Warning 而不是 Error —— 内容的硬问题已经由上面那条计入 Error，
        // 这一条要说明的是"清单层面也不一致"（清单可能是从别处抄的、
        // 或者内容在打包后被替换过），修复动作与上面不同，所以单独列。
        if (contradicted_streams > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kSegmentDeclarationContradicts,
                           model::StreamingIssueCode::kSegmentDeclarationContradicts),
                      "清单声明独立分片，但内容做不到",
                      "有 " + std::to_string(contradicted_streams) + "/" + std::to_string(declared_streams) +
                          " 条声明了独立分片的流里出现 " + std::to_string(contradicted_segments) +
                          " 个首样本非同步的分片（首个: " + first_contradicted +
                          "）。"
                          "声明来源：" +
                          (result.IsDash() ? "DASH AdaptationSet/Representation@startWithSAP"
                                           : "HLS EXT-X-INDEPENDENT-SEGMENTS") +
                          "。播放器会据此在任意分片边界切换码率或拼接，"
                          "一旦内容没做到就会在切换点花屏。",
                      "两条都要查：① 分片边界是否对齐了 IDR；② 清单的这条声明是不是"
                      "从别的包沿用过来、或内容被重新封装后没有同步改正。"
                      "不能只改清单把声明删掉 —— 那只是让播放器不再依赖它，"
                      "内容依然无法从分片起点起播。",
                      model::IssueSeverity::Warning, static_cast<double>(contradicted_segments), 0.0,
                      contradicted_segments, -1.0, -1);
        }
        // 反向：实测全达标但清单没声明。不是缺陷，是漏掉了本可以给的承诺。
        if (undeclared_streams > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kSegmentIndependenceUndeclared,
                           model::StreamingIssueCode::kSegmentIndependenceUndeclared),
                      "分片实测可独立解码，但清单未声明",
                      "有 " + std::to_string(undeclared_streams) + " 条流共 " + std::to_string(undeclared_segments) +
                          " 个分片的第一个样本都是同步样本（首个: " + first_undeclared +
                          "），内容本身满足独立分片的要求；但这些流既没有 " +
                          (result.IsDash() ? "startWithSAP=\"1|2\"" : "EXT-X-INDEPENDENT-SEGMENTS") +
                          "，播放器按规范不能假定分片可独立拼接，"
                          "也就不会做无边界的码率切换与秒开优化。",
                      std::string("内容不用改。若确实每个分片都从关键帧开始，") +
                          (result.IsDash() ? "在 AdaptationSet 上加 startWithSAP=\"1\"。"
                                           : "在 media playlist 加 #EXT-X-INDEPENDENT-SEGMENTS。"),
                      model::IssueSeverity::Info, static_cast<double>(undeclared_segments), 0.0, undeclared_segments,
                      -1.0, -1);
        }
    }

    // ---- 1c) 跨分片时间轴连续性（实证层）----
    //
    // 数据源是文件里的 tfdt 与段内实测时长，不是清单的 EXTINF / <S t= d=>。
    // 因此它和 DASH 侧的 segment_timeline_gap 不重复：那个查清单自洽性，
    // 这个查"封装器写进文件的时基"是否首尾相接。清单完全正确、但 tfdt 写错
    // 的包（拼接工具、转封装工具常见）只有这一层能抓到。
    //
    // 严重度分级依据：
    //   * 起点倒退 —— tfdt 直接比较，硬证据，Error
    //   * 缺口/重叠 —— 依赖"段内实测时长"，而该时长取的是首个 traf 的样本时长和，
    //                  多轨分片下会偏短，所以只给 Warning，并在文案里注明依据
    if (options.check_segment_continuity) {
        const double tol = options.segment_continuity_tolerance_s;
        int gap_count = 0;
        int overlap_count = 0;
        int regress_count = 0;
        double worst_gap = 0.0;
        double worst_overlap = 0.0;
        double gap_at = -1.0;
        double overlap_at = -1.0;
        double regress_at = -1.0;
        std::string gap_pair;
        std::string overlap_pair;
        std::string regress_pair;
        unsigned long long scanned = 0;

        auto check = [&](const std::vector<model::SegmentInfo>& segments) {
            const model::SegmentInfo* prev = nullptr;
            for (const model::SegmentInfo& seg : segments) {
                // 只对"探测过且拿到了 tfdt 的 fMP4 分片"下结论。
                // 中间夹一个没探到的段不重置 prev：连续性说的是相邻两个**有时间**的段，
                // 断在这里会漏掉跨过该段的真缺口。
                if (seg.partial || seg.container != model::SegmentContainer::FMP4 || !seg.probed ||
                    seg.container_parse_failed || !seg.has_decode_time) {
                    continue;
                }
                if (seg.discontinuity_before) {
                    // EXT-X-DISCONTINUITY 是规范允许的时基重置，不能按"倒退"报。
                    prev = &seg;
                    continue;
                }
                if (prev != nullptr) {
                    // 上一段的终点：优先用段内实测时长，回落到清单声明时长。
                    // 两者都没有时只能判倒退，不能判缺口/重叠（否则会把"时长未知"当成 0）。
                    const bool prev_has_span = prev->has_media_duration || prev->has_duration;
                    const double prev_span =
                        prev->has_media_duration ? prev->media_duration_seconds : prev->duration_seconds;
                    const double prev_end = prev->start_seconds + prev_span;

                    if (seg.start_seconds < prev->start_seconds - tol) {
                        ++regress_count;
                        if (regress_pair.empty())
                            regress_pair = prev->uri + " -> " + seg.uri;
                        if (regress_at < 0.0)
                            regress_at = seg.start_seconds;
                    } else if (prev_has_span) {
                        if (seg.start_seconds > prev_end + tol) {
                            const double hole = seg.start_seconds - prev_end;
                            ++gap_count;
                            if (hole > worst_gap) {
                                worst_gap = hole;
                                gap_pair = prev->uri + " -> " + seg.uri;
                                gap_at = prev_end;
                            }
                        } else if (seg.start_seconds < prev_end - tol) {
                            const double dup = prev_end - seg.start_seconds;
                            ++overlap_count;
                            if (dup > worst_overlap) {
                                worst_overlap = dup;
                                overlap_pair = prev->uri + " -> " + seg.uri;
                                overlap_at = seg.start_seconds;
                            }
                        }
                    }
                }
                prev = &seg;
            }
        };
        for (const model::MediaPlaylistInfo& pl : result.playlists) {
            if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                return StageStatus::kCancelled;
            check(pl.segments);
        }
        for (const model::DashRepresentationInfo& rep : result.representations) {
            if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                return StageStatus::kCancelled;
            check(rep.segments);
        }
        if (regress_count > 0) {
            PushIssue(
                result,
                Code(result, model::StreamingIssueCode::kSegmentTimelineRegress,
                     model::StreamingIssueCode::kSegmentTimelineRegress),
                "分片时间轴倒退",
                "有 " + std::to_string(regress_count) + " 处相邻分片的 tfdt 起点比上一段更早（首个: " + regress_pair +
                    "），"
                    "且两段之间没有 EXT-X-DISCONTINUITY。"
                    "播放器会把这些分片排到已播过的位置上，表现为跳播、回退或直接丢弃。",
                "检查封装/拼接工具是否把不同来源的分片写进了同一条时间轴；"
                "如果这确实是一次合法的时间轴重置（比如插播后重置），"
                "应在清单里用 EXT-X-DISCONTINUITY 显式标注。",
                model::IssueSeverity::Error, static_cast<double>(regress_count), 0.0, regress_count, regress_at, -1);
        }
        if (gap_count > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kSegmentTimelineGapActual,
                           model::StreamingIssueCode::kSegmentTimelineGapActual),
                      "分片之间存在时间空洞",
                      "有 " + std::to_string(gap_count) + " 处相邻分片首尾不相接，最大缺口 " +
                          FormatSeconds(worst_gap) + " 秒（" + gap_pair +
                          "）。"
                          "判定依据是文件里的 tfdt 与段内实测时长（不是清单声明的 EXTINF）。"
                          "这种空洞意味着这段时间的媒体样本不在任何分片里。",
                      "找出被漏掉的分片并补回清单；若是切档/广告替换造成的，"
                      "确认拼接点两侧的 tfdt 是否被正确重写。",
                      model::IssueSeverity::Warning, worst_gap, tol, gap_count, gap_at, -1);
        }
        if (overlap_count > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kSegmentTimelineOverlapActual,
                           model::StreamingIssueCode::kSegmentTimelineOverlapActual),
                      "分片之间存在时间重叠",
                      "有 " + std::to_string(overlap_count) + " 处相邻分片覆盖了同一段时间，最大重叠 " +
                          FormatSeconds(worst_overlap) + " 秒（" + overlap_pair +
                          "）。"
                          "判定依据是文件里的 tfdt 与段内实测时长。"
                          "重叠部分会被播放两次，表现为卡顿一下或音画重复。",
                      "检查分片切分点是否按样本边界对齐；转封装时常见原因是把上一段的"
                      "尾样本重复写进了下一段。",
                      model::IssueSeverity::Warning, worst_overlap, tol, overlap_count, overlap_at, -1);
        }
    }

    if (result.ladder.size() < 2)
        return StageStatus::kDone;

    // ---- 2) 分辨率一致性 ----
    if (options.check_variant_consistency) {
        int missing_resolution = 0;
        for (const model::StreamingLadderEntry& e : result.ladder) {
            if (e.width <= 0 || e.height <= 0)
                ++missing_resolution;
        }
        if (missing_resolution > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kHlsVariantResolutionMismatch,
                           model::StreamingIssueCode::kDashVariantResolutionMismatch),
                      "码率阶梯存在未声明分辨率的流",
                      "ladder 里有 " + std::to_string(missing_resolution) + "/" + std::to_string(result.ladder.size()) +
                          " 条流没有分辨率信息，客户端无法按屏幕尺寸/视窗选流，只能退化为按带宽猜。",
                      result.IsDash() ? "给每个 Representation 补上 width / height（视频轨必填）。"
                                      : "给每个 EXT-X-STREAM-INF 补上 RESOLUTION=宽x高。",
                      model::IssueSeverity::Warning, static_cast<double>(missing_resolution), 0.0, missing_resolution,
                      -1.0, -1);
        } else {
            // 分辨率应随码率单调不减：带宽更高却分辨率更低，说明 ladder 排错了
            std::vector<const model::StreamingLadderEntry*> ordered;
            for (const model::StreamingLadderEntry& e : result.ladder)
                ordered.push_back(&e);
            std::sort(ordered.begin(), ordered.end(),
                      [](const model::StreamingLadderEntry* a, const model::StreamingLadderEntry* b) {
                          return a->bandwidth_bps < b->bandwidth_bps;
                      });
            std::string bad;
            for (size_t i = 1; i < ordered.size(); ++i) {
                const long long prev_area = static_cast<long long>(ordered[i - 1]->width) * ordered[i - 1]->height;
                const long long cur_area = static_cast<long long>(ordered[i]->width) * ordered[i]->height;
                if (cur_area < prev_area) {
                    bad = ordered[i - 1]->label + " (" + FormatKbps(ordered[i - 1]->bandwidth_bps) + ", " +
                          std::to_string(ordered[i - 1]->width) + "x" + std::to_string(ordered[i - 1]->height) +
                          ") 的分辨率高于 " + ordered[i]->label + " (" + FormatKbps(ordered[i]->bandwidth_bps) + ", " +
                          std::to_string(ordered[i]->width) + "x" + std::to_string(ordered[i]->height) + ")";
                    break;
                }
            }
            if (!bad.empty()) {
                PushIssue(result,
                          Code(result, model::StreamingIssueCode::kHlsVariantResolutionMismatch,
                               model::StreamingIssueCode::kDashVariantResolutionMismatch),
                          "码率阶梯的分辨率与码率顺序矛盾", bad + "。",
                          "按码率从低到高重排 ladder，并保证分辨率单调不减；"
                          "否则 ABR 算法升档时画面反而变糊。",
                          model::IssueSeverity::Warning, 1.0, 0.0, 1, -1.0, -1);
            }
        }
    }

    // ---- 3) 编码一致性 ----
    if (options.check_variant_consistency) {
        std::set<std::string> codecs;
        for (const model::StreamingLadderEntry& e : result.ladder) {
            if (!e.video_codec.empty())
                codecs.insert(e.video_codec);
        }
        if (codecs.size() > 1) {
            std::string joined;
            for (const std::string& c : codecs) {
                if (!joined.empty())
                    joined += " / ";
                joined += c;
            }
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kHlsVariantCodecMismatch,
                           model::StreamingIssueCode::kDashVariantCodecMismatch),
                      "码率阶梯混用了不同视频编码",
                      "ladder 里出现了 " + std::to_string(codecs.size()) + " 种视频编码: " + joined +
                          "。ABR 切换时解码器要重建，绝大多数播放器会直接切换失败或黑屏。",
                      "同一条 ladder 必须使用同一种编码（含 profile/level 兼容）；"
                      "若确实要提供多编码，请拆成多组并让播放器显式选择。",
                      model::IssueSeverity::Warning, static_cast<double>(codecs.size()), 0.0, 1, -1.0, -1);
        }
    }

    // ---- 4) 关键帧（分片起点）时间轴对齐 ----
    // 逐条流 × 逐关键帧的 O(n·m) 比对: 上千分片的包在这里要跑上百万次比较,
    // 不查取消的话"点取消"要等这一整段跑完才有反应。
    if (options.check_keyframe_alignment) {
        // 直播包各层的绝对起点本来就不一样：HLS 各轨有自己的 MEDIA-SEQUENCE，
        // DASH 各 Representation 有各自的 PTO 与 availabilityStartTime，
        // 谁先"进入当前窗口"取决于各自的上一次重载时刻。这时比绝对秒必然误报，
        // 改比"相对本层第一个关键帧的偏移"：整层被平移整数个分片是直播的正常现象，
        // 而真正的 GOP 错位会表现为逐片漂移，照样逃不掉。
        const bool live_package = result.IsLive();
        auto comparable_times = [live_package](const std::vector<double>& times) {
            if (!live_package || times.empty())
                return times;
            std::vector<double> shifted = times;
            const double base = shifted.front();
            for (double& t : shifted)
                t -= base;
            return shifted;
        };

        const model::StreamingLadderEntry* reference = nullptr;
        int entries_with_keyframes = 0;
        for (const model::StreamingLadderEntry& e : result.ladder) {
            if (e.keyframe_times.empty())
                continue;
            ++entries_with_keyframes;
            if (reference == nullptr || e.keyframe_times.size() > reference->keyframe_times.size()) {
                reference = &e;
            }
        }
        if (reference != nullptr && entries_with_keyframes >= 2) {
            const std::vector<double> reference_times = comparable_times(reference->keyframe_times);
            int mismatched = 0;
            double worst_delta = 0.0;
            double first_at = -1.0;
            std::string worst_detail;
            unsigned long long compared = 0;
            for (const model::StreamingLadderEntry& e : result.ladder) {
                if (&e == reference || e.keyframe_times.empty())
                    continue;
                if (IsStreamingCancelled(cancel))
                    return StageStatus::kCancelled;
                const std::vector<double> current_times = comparable_times(e.keyframe_times);
                const size_t n = std::min(reference_times.size(), current_times.size());
                double local_worst = 0.0;
                int local_bad = 0;
                for (size_t i = 0; i < n; ++i) {
                    if (ShouldCheckStreamingCancel(compared++) && IsStreamingCancelled(cancel))
                        return StageStatus::kCancelled;
                    const double delta = std::fabs(current_times[i] - reference_times[i]);
                    if (delta > options.keyframe_align_tolerance_s) {
                        ++local_bad;
                        if (delta > local_worst)
                            local_worst = delta;
                    }
                }
                const bool count_short = e.keyframe_times.size() < reference->keyframe_times.size() &&
                                         e.segment_count >= static_cast<int>(reference->keyframe_times.size());
                if (local_bad > 0 || count_short) {
                    ++mismatched;
                    if (local_worst > worst_delta)
                        worst_delta = local_worst;
                    if (first_at < 0.0 && !e.keyframe_times.empty())
                        first_at = e.keyframe_times.front();
                    if (worst_detail.empty()) {
                        worst_detail = e.label + " 与 " + reference->label + " 有 " + std::to_string(local_bad) +
                                       " 个关键帧时间点偏差超过 " + FormatSeconds(options.keyframe_align_tolerance_s) +
                                       " 秒";
                        if (count_short) {
                            worst_detail += "，且关键帧数量更少（" + std::to_string(e.keyframe_times.size()) + " vs " +
                                            std::to_string(reference->keyframe_times.size()) + "）";
                        }
                    }
                }
            }
            if (mismatched > 0) {
                PushIssue(result,
                          Code(result, model::StreamingIssueCode::kHlsVariantKeyframeMisalign,
                               model::StreamingIssueCode::kDashVariantKeyframeMisalign),
                          "多码率关键帧不对齐", worst_detail + "。",
                          "ABR 只能在关键帧处切换：让所有码率层使用相同的 GOP 长度与切片起点"
                          "（同一套 -force_key_frames / -g -keyint_min），并让分片边界严格对齐。",
                          model::IssueSeverity::Warning, worst_delta, options.keyframe_align_tolerance_s, mismatched,
                          first_at, -1);
            }
        }
    }

    // ---- 5) 声明码率 vs 实测峰值段码率 ----
    // 每条流都要把它的全部分片扫一遍取峰值, 与第 1 段同量级。
    {
        int over = 0;
        double worst_ratio = 0.0;
        std::string worst_label;
        unsigned long long scanned = 0;
        for (const model::StreamingLadderEntry& e : result.ladder) {
            if (e.bandwidth_bps <= 0)
                continue;
            if (IsStreamingCancelled(cancel))
                return StageStatus::kCancelled;
            int64_t peak = 0;
            if (result.IsDash()) {
                if (e.source_index < 0 || static_cast<size_t>(e.source_index) >= result.representations.size()) {
                    continue;
                }
                for (const model::SegmentInfo& seg : result.representations[e.source_index].segments) {
                    if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                        return StageStatus::kCancelled;
                    peak = std::max(peak, seg.MeasuredBitrateBps());
                }
            } else {
                const model::HlsVariantInfo* variant = nullptr;
                for (const model::HlsVariantInfo& v : result.variants) {
                    if (v.index == e.source_index) {
                        variant = &v;
                        break;
                    }
                }
                if (variant == nullptr || variant->playlist_index < 0 ||
                    static_cast<size_t>(variant->playlist_index) >= result.playlists.size()) {
                    continue;
                }
                for (const model::SegmentInfo& seg : result.playlists[variant->playlist_index].segments) {
                    if (ShouldCheckStreamingCancel(scanned++) && IsStreamingCancelled(cancel))
                        return StageStatus::kCancelled;
                    peak = std::max(peak, seg.MeasuredBitrateBps());
                }
            }
            if (peak <= 0)
                continue;
            const double declared = static_cast<double>(e.bandwidth_bps);
            const double ratio = static_cast<double>(peak) / declared;
            if (ratio > 1.0 + options.bandwidth_tolerance_ratio) {
                ++over;
                if (ratio > worst_ratio) {
                    worst_ratio = ratio;
                    worst_label = e.label;
                }
            }
        }
        if (over > 0) {
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kHlsBandwidthMismatch,
                           model::StreamingIssueCode::kDashBandwidthMismatch),
                      "实测峰值段码率高于声明带宽",
                      worst_label + " 的实测峰值段码率是声明值的 " + FormatSeconds(worst_ratio) + " 倍（共 " +
                          std::to_string(over) + " 条流超标，允许偏差 " +
                          FormatSeconds(options.bandwidth_tolerance_ratio) + "）。",
                      "BANDWIDTH 必须不小于任一分片的峰值码率，否则客户端按声明值选流会缓冲。"
                      "用实测峰值更新声明值，或给码率加约束（VBV / maxrate）让分片码率更平。",
                      model::IssueSeverity::Warning, worst_ratio, 1.0 + options.bandwidth_tolerance_ratio, over, -1.0,
                      -1);
        }
    }

    // ---- 6) 音视频分片数量 ----
    if (options.check_av_segment_count) {
        auto report = [&](const std::string& video_label, int video_count, double video_total,
                          const std::string& audio_label, int audio_count, double audio_total) {
            if (video_count <= 0 || audio_count <= 0)
                return;
            if (video_count == audio_count)
                return;
            const double tolerance =
                std::max(video_total / std::max(1, video_count), audio_total / std::max(1, audio_count));
            if (std::fabs(video_total - audio_total) <= tolerance)
                return; // 时长对得上就不算问题
            PushIssue(result,
                      Code(result, model::StreamingIssueCode::kHlsAvSegmentCountMismatch,
                           model::StreamingIssueCode::kDashAvSegmentCountMismatch),
                      "音视频分片数量/时长对不上",
                      video_label + " 有 " + std::to_string(video_count) + " 个分片（合计 " +
                          FormatSeconds(video_total) + " 秒），而 " + audio_label + " 有 " +
                          std::to_string(audio_count) + " 个分片（合计 " + FormatSeconds(audio_total) +
                          " 秒），总时长差超过一个分片。",
                      "让音视频使用相同的分片时长并对齐起点；"
                      "总时长不一致会导致尾部静音、黑帧或音画不同步。",
                      model::IssueSeverity::Warning, std::fabs(video_total - audio_total), tolerance, 1, -1.0, -1);
        };

        if (result.IsDash()) {
            // 每个 Period 内取第一条视频 Representation 与第一条音频 Representation 比对
            for (const model::DashPeriodInfo& period : result.periods) {
                // 内层是 O(period × representation), 多 Period 长清单下迭代量不小
                if (IsStreamingCancelled(cancel))
                    return StageStatus::kCancelled;
                const model::DashRepresentationInfo* video = nullptr;
                const model::DashRepresentationInfo* audio = nullptr;
                for (const model::DashRepresentationInfo& rep : result.representations) {
                    if (rep.period_index != period.index)
                        continue;
                    if (rep.content_type == "video" && video == nullptr)
                        video = &rep;
                    if (rep.content_type == "audio" && audio == nullptr)
                        audio = &rep;
                }
                if (video == nullptr || audio == nullptr)
                    continue;
                report("Period " + period.id + " 视频轨", static_cast<int>(video->segments.size()),
                       video->total_duration_seconds, "Period " + period.id + " 音频轨",
                       static_cast<int>(audio->segments.size()), audio->total_duration_seconds);
            }
        } else {
            for (const model::HlsVariantInfo& variant : result.variants) {
                if (variant.playlist_index < 0 || variant.audio_group.empty())
                    continue;
                if (static_cast<size_t>(variant.playlist_index) >= result.playlists.size())
                    continue;
                if (IsStreamingCancelled(cancel))
                    return StageStatus::kCancelled;
                const model::MediaPlaylistInfo& video_pl = result.playlists[variant.playlist_index];
                for (const model::HlsRenditionInfo& r : result.renditions) {
                    if (r.type != "audio" || r.group_id != variant.audio_group)
                        continue;
                    if (r.playlist_index < 0 || static_cast<size_t>(r.playlist_index) >= result.playlists.size()) {
                        continue;
                    }
                    const model::MediaPlaylistInfo& audio_pl = result.playlists[r.playlist_index];
                    report("variant #" + std::to_string(variant.index) + " 视频轨",
                           static_cast<int>(video_pl.SegmentCount()), video_pl.total_duration_seconds, "音轨 " + r.name,
                           static_cast<int>(audio_pl.SegmentCount()), audio_pl.total_duration_seconds);
                }
            }
        }
    }
    return StageStatus::kDone;
}

SegmentQcAnalyzer::StageStatus SegmentQcAnalyzer::Analyze(model::StreamingPackageResult& result,
                                                          const SegmentQcOptions& options,
                                                          const std::atomic<bool>* cancel) {
    if (!result.valid)
        return StageStatus::kFailed;
    if (options.probe_segments) {
        const StageStatus probe = ProbeSegments(result, options, cancel);
        if (probe != StageStatus::kDone)
            return probe;
    }
    // 已被取消就别再建 ladder / 跑校验了：结果注定会被调用方丢弃，
    // 继续跑只是让"取消"更晚生效。
    if (IsStreamingCancelled(cancel))
        return StageStatus::kCancelled;
    BuildLadder(result);
    // Validate 现在自己会报告"中途被取消", 必须接着它的结论走: 它提前返回时 issues
    // 只补了一半(result 是半截状态), 继续当 kDone 处理会让调用方拿这份半截结果去收尾。
    const StageStatus validated = Validate(result, options, cancel);
    if (validated != StageStatus::kDone)
        return validated;
    if (IsStreamingCancelled(cancel))
        return StageStatus::kCancelled;
    return StageStatus::kDone;
}

} // namespace videoeye
