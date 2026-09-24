#include "SegmentQcAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "utils/ManifestText.h"

namespace videoeye {
namespace analyzer {
namespace {

namespace mt = videoeye::utils::manifest;

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

// ---------------------------------------------------------------------------
// fMP4 探测
// ---------------------------------------------------------------------------

// 从初始化段（含 moov）拿 timescale；拿不到返回 0
uint32_t ProbeInitTimescale(const std::string& init_path) {
    if (init_path.empty())
        return 0;
    Mp4SampleTableOptions opt;
    opt.expand_samples = false;
    opt.max_samples_per_track = 1;
    model::Mp4SampleTableResult res;
    Mp4SampleTableAnalyzer analyzer;
    if (!analyzer.AnalyzeFile(init_path, res, opt))
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

// 探测一个 fMP4 媒体分片：拿首个 moof 的 tfdt 作为关键帧时间
bool ProbeFmp4Segment(model::SegmentInfo& seg, uint32_t fallback_timescale) {
    Mp4SampleTableOptions opt;
    opt.expand_samples = false; // 只要 moof/traf/tfdt，展开样本表是纯浪费
    opt.max_samples_per_track = 1;
    model::Mp4SampleTableResult res;
    Mp4SampleTableAnalyzer analyzer;
    if (!analyzer.AnalyzeFile(seg.resolved_path, res, opt)) {
        seg.container_parse_failed = true;
        seg.container_error = res.error_message;
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
    // 取最早的 tfdt（多 traf 时第一个 moof 里的最小起点）
    uint64_t best = 0;
    bool found = false;
    for (const model::Mp4FragmentInfo& f : res.fragments) {
        if (!f.has_tfdt)
            continue;
        if (!found || f.base_media_decode_time < best) {
            best = f.base_media_decode_time;
            found = true;
        }
    }
    if (!found || timescale == 0)
        return false;

    seg.timescale = timescale;
    seg.decode_time = best;
    seg.has_decode_time = true;
    seg.start_seconds = static_cast<double>(best) / static_cast<double>(timescale);
    // CMAF 分片必须以 IDR 开头，拿到 tfdt 就等于确认了起点是关键帧
    seg.starts_with_keyframe = true;
    return true;
}

// 探测一组分片（HLS 的一个 media playlist / DASH 的一个 representation）
void ProbeSegmentRange(std::vector<model::SegmentInfo>& segments, uint32_t init_timescale,
                       const SegmentQcOptions& options) {
    uint32_t probed = 0;
    for (model::SegmentInfo& seg : segments) {
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
            ProbeFmp4Segment(seg, init_timescale);
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

bool SegmentQcAnalyzer::ProbeSegments(model::StreamingPackageResult& result, const SegmentQcOptions& options) {
    uint32_t probed_units = 0;
    const uint32_t kMaxUnits = 64; // package 级上限：再多的码率层也没必要全探测

    for (model::MediaPlaylistInfo& pl : result.playlists) {
        if (probed_units >= kMaxUnits)
            break;
        ++probed_units;
        uint32_t timescale = 0;
        if (pl.has_init_section && !pl.init_resolved_path.empty()) {
            timescale = ProbeInitTimescale(pl.init_resolved_path);
        }
        ProbeSegmentRange(pl.segments, timescale, options);
    }

    for (model::DashRepresentationInfo& rep : result.representations) {
        if (probed_units >= kMaxUnits)
            break;
        ++probed_units;
        uint32_t timescale = 0;
        if (!rep.init_resolved_path.empty()) {
            timescale = ProbeInitTimescale(rep.init_resolved_path);
        }
        ProbeSegmentRange(rep.segments, timescale, options);
    }

    return true;
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

void SegmentQcAnalyzer::Validate(model::StreamingPackageResult& result, const SegmentQcOptions& options) {
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
    };
    model::RemoveIssuesByCode(result.issues, kOwnCodes);

    if (result.ladder.empty())
        BuildLadder(result);

    // ---- 1) 分片文件缺失 / 容器解析失败 ----
    {
        int missing = 0;
        int invalid = 0;
        std::string first_missing;
        for (const model::MediaPlaylistInfo& pl : result.playlists) {
            for (const model::SegmentInfo& seg : pl.segments) {
                if (seg.partial || seg.resolved_path.empty())
                    continue;
                if (!seg.exists) {
                    ++missing;
                    if (first_missing.empty())
                        first_missing = seg.uri;
                } else if (seg.container_parse_failed) {
                    ++invalid;
                }
            }
        }
        for (const model::DashRepresentationInfo& rep : result.representations) {
            for (const model::SegmentInfo& seg : rep.segments) {
                if (seg.resolved_path.empty())
                    continue;
                if (!seg.exists) {
                    ++missing;
                    if (first_missing.empty())
                        first_missing = seg.uri;
                } else if (seg.container_parse_failed) {
                    ++invalid;
                }
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
        if (invalid > 0) {
            PushIssue(result, model::StreamingIssueCode::kSegmentContainerInvalid, "分片容器解析失败",
                      "有 " + std::to_string(invalid) +
                          " 个分片存在，但用已有的容器分析器（fMP4 走 IsobmffParser）解析失败。",
                      "用 VideoEye 单独打开该分片看具体报错；常见原因是分片被截断或缺少初始化段。",
                      model::IssueSeverity::Error, static_cast<double>(invalid), 0.0, invalid, -1.0, -1);
        }
    }

    if (result.ladder.size() < 2)
        return;

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
    if (options.check_keyframe_alignment) {
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
            int mismatched = 0;
            double worst_delta = 0.0;
            double first_at = -1.0;
            std::string worst_detail;
            for (const model::StreamingLadderEntry& e : result.ladder) {
                if (&e == reference || e.keyframe_times.empty())
                    continue;
                const size_t n = std::min(reference->keyframe_times.size(), e.keyframe_times.size());
                double local_worst = 0.0;
                int local_bad = 0;
                for (size_t i = 0; i < n; ++i) {
                    const double delta = std::fabs(e.keyframe_times[i] - reference->keyframe_times[i]);
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
    {
        int over = 0;
        double worst_ratio = 0.0;
        std::string worst_label;
        for (const model::StreamingLadderEntry& e : result.ladder) {
            if (e.bandwidth_bps <= 0)
                continue;
            int64_t peak = 0;
            if (result.IsDash()) {
                if (e.source_index < 0 || static_cast<size_t>(e.source_index) >= result.representations.size()) {
                    continue;
                }
                for (const model::SegmentInfo& seg : result.representations[e.source_index].segments) {
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
}

bool SegmentQcAnalyzer::Analyze(model::StreamingPackageResult& result, const SegmentQcOptions& options) {
    if (!result.valid)
        return false;
    if (options.probe_segments)
        ProbeSegments(result, options);
    BuildLadder(result);
    Validate(result, options);
    return true;
}

} // namespace analyzer
} // namespace videoeye
