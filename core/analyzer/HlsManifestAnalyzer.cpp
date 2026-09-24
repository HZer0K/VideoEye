#include "HlsManifestAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>

#include "utils/ManifestText.h"

namespace videoeye {
namespace analyzer {
namespace {

namespace mt = videoeye::utils::manifest;

// ---------------------------------------------------------------------------
// 文本小工具
// ---------------------------------------------------------------------------

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < text.size()) {
        const size_t nl = text.find('\n', i);
        std::string line = (nl == std::string::npos) ? text.substr(i) : text.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos)
            break;
        i = nl + 1;
    }
    return lines;
}

// "#EXTINF:4.0,title" -> "#EXTINF"
std::string TagName(const std::string& line) {
    const size_t colon = line.find(':');
    std::string name = (colon == std::string::npos) ? line : line.substr(0, colon);
    return mt::Trim(name);
}

// "#EXTINF:4.0,title" -> "4.0,title"
std::string TagBody(const std::string& line) {
    const size_t colon = line.find(':');
    return (colon == std::string::npos) ? std::string() : mt::Trim(line.substr(colon + 1));
}

bool ParseIntText(const std::string& text, int64_t& out) {
    const std::string s = mt::Trim(text);
    if (s.empty())
        return false;
    try {
        size_t pos = 0;
        const long long v = std::stoll(s, &pos);
        if (pos != s.size())
            return false;
        out = static_cast<int64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseDoubleText(const std::string& text, double& out) {
    const std::string s = mt::Trim(text);
    if (s.empty())
        return false;
    try {
        size_t pos = 0;
        const double v = std::stod(s, &pos);
        if (pos != s.size())
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// "1920x1080" -> 1920, 1080
bool ParseResolution(const std::string& text, int& w, int& h) {
    const size_t x = text.find('x');
    if (x == std::string::npos)
        return false;
    int64_t a = 0;
    int64_t b = 0;
    if (!ParseIntText(text.substr(0, x), a))
        return false;
    if (!ParseIntText(text.substr(x + 1), b))
        return false;
    w = static_cast<int>(a);
    h = static_cast<int>(b);
    return true;
}

// 按 URI 后缀猜分片容器。猜不出来就 Other —— 宁可漏判也不要误判。
model::SegmentContainer ContainerFromUri(const std::string& uri) {
    const std::string ext = mt::ExtensionOf(uri);
    if (ext == "ts" || ext == "m2ts" || ext == "mts")
        return model::SegmentContainer::MPEG_TS;
    if (ext == "m4s" || ext == "mp4" || ext == "m4v" || ext == "cmfv" || ext == "cmfa" || ext == "cmf2" ||
        ext == "m4i") {
        return model::SegmentContainer::FMP4;
    }
    if (ext == "webm")
        return model::SegmentContainer::WebM;
    if (ext == "aac" || ext == "adts" || ext == "ac3" || ext == "ec3" || ext == "mp3") {
        return model::SegmentContainer::PackedAudio;
    }
    return model::SegmentContainer::Other;
}

bool ReadFileText(const std::string& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    text = ss.str();
    return true;
}

// ---------------------------------------------------------------------------
// media playlist 正文
// ---------------------------------------------------------------------------

void ParseMediaPlaylistBody(const std::vector<std::string>& lines, const std::string& base_dir,
                            model::MediaPlaylistInfo& pl, const HlsManifestOptions& options) {
    bool pending_discontinuity = false;
    bool pending_gap = false;
    double pending_duration = 0.0;
    bool pending_has_duration = false;
    int64_t pending_range_len = -1;
    int64_t pending_range_off = -1;
    uint64_t full_index = 0;
    uint64_t partial_index = 0;
    double cursor = 0.0;

    auto reset_pending = [&]() {
        pending_discontinuity = false;
        pending_gap = false;
        pending_duration = 0.0;
        pending_has_duration = false;
        pending_range_len = -1;
        pending_range_off = -1;
    };

    for (const std::string& raw_line : lines) {
        const std::string raw = mt::Trim(raw_line);
        if (raw.empty())
            continue;

        // ---- URI 行：紧跟在 #EXTINF / #EXT-X-PART 之后 ----
        if (raw[0] != '#') {
            if (full_index >= options.max_segments_per_playlist) {
                pl.truncated = true;
                reset_pending();
                continue;
            }
            model::SegmentInfo seg;
            seg.uri = raw;
            seg.resolved_path = mt::ResolveLocalUri(base_dir, raw);
            seg.sequence = pl.media_sequence + full_index;
            seg.duration_seconds = pending_duration;
            seg.has_duration = pending_has_duration;
            seg.discontinuity_before = pending_discontinuity;
            seg.gap = pending_gap;
            seg.byte_range_offset = pending_range_off;
            seg.byte_range_length = pending_range_len;
            seg.has_init_section = pl.has_init_section;
            seg.init_uri = pl.init_uri;
            seg.start_seconds = cursor;
            seg.container = ContainerFromUri(raw);
            // TS 段按 ABR 惯例视为以 IDR 开头（实际是否 IDR 要逐包解析才知道）；
            // fMP4 段要等 ProbeSegments 拿到 tfdt 才置真。
            seg.starts_with_keyframe = (seg.container == model::SegmentContainer::MPEG_TS);
            if (pending_discontinuity)
                ++pl.discontinuity_count;
            if (pending_has_duration)
                cursor += pending_duration;
            ++full_index;
            pl.segments.push_back(std::move(seg));
            reset_pending();
            continue;
        }

        const std::string name = TagName(raw);
        const std::string body = TagBody(raw);

        if (name == "#EXT-X-VERSION") {
            int64_t v = 0;
            if (ParseIntText(body, v))
                pl.version = static_cast<int>(v);
        } else if (name == "#EXT-X-TARGETDURATION") {
            int64_t v = 0;
            if (ParseIntText(body, v)) {
                pl.target_duration_s = static_cast<int>(v);
                pl.has_target_duration = true;
            }
        } else if (name == "#EXT-X-MEDIA-SEQUENCE") {
            int64_t v = 0;
            if (ParseIntText(body, v)) {
                pl.media_sequence = v;
                pl.has_media_sequence = true;
            }
        } else if (name == "#EXT-X-DISCONTINUITY-SEQUENCE") {
            int64_t v = 0;
            if (ParseIntText(body, v)) {
                pl.discontinuity_sequence = v;
                pl.has_discontinuity_sequence = true;
            }
        } else if (name == "#EXT-X-PLAYLIST-TYPE") {
            pl.playlist_type_vod = (mt::ToLower(body) == "vod");
        } else if (name == "#EXT-X-ENDLIST") {
            pl.has_endlist = true;
        } else if (name == "#EXT-X-INDEPENDENT-SEGMENTS") {
            pl.independent_segments = true;
        } else if (name == "#EXT-X-DISCONTINUITY") {
            pending_discontinuity = true;
        } else if (name == "#EXT-X-GAP") {
            pending_gap = true;
        } else if (name == "#EXT-X-KEY") {
            const auto attrs = mt::ParseHlsAttributes(body);
            const std::string method = mt::ToLower(mt::AttrString(attrs, "METHOD"));
            if (method.empty() || method == "none")
                continue; // METHOD=NONE 是显式声明"不加密"
            pl.encrypted = true;
            pl.key_method = mt::AttrString(attrs, "METHOD");
            pl.key_uri = mt::AttrString(attrs, "URI");
            pl.key_format = mt::AttrString(attrs, "KEYFORMAT");
        } else if (name == "#EXT-X-MAP") {
            const auto attrs = mt::ParseHlsAttributes(body);
            pl.init_uri = mt::AttrString(attrs, "URI");
            pl.has_init_section = !pl.init_uri.empty();
        } else if (name == "#EXT-X-BYTERANGE") {
            const size_t at = body.find('@');
            if (at == std::string::npos) {
                if (ParseIntText(body, pending_range_len))
                    pending_range_off = -1;
            } else {
                ParseIntText(body.substr(0, at), pending_range_len);
                ParseIntText(body.substr(at + 1), pending_range_off);
            }
        } else if (name == "#EXTINF") {
            const size_t comma = body.find(',');
            const std::string dur = (comma == std::string::npos) ? body : body.substr(0, comma);
            double d = 0.0;
            if (ParseDoubleText(dur, d)) {
                pending_duration = d;
                pending_has_duration = true;
            }
        } else if (name == "#EXT-X-PART") {
            // LL-HLS 部分分片：单独成列，不占用媒体序号，也不计入 SegmentCount()
            if (partial_index >= options.max_partials_per_playlist) {
                pl.truncated = true;
                continue;
            }
            const auto attrs = mt::ParseHlsAttributes(body);
            model::SegmentInfo seg;
            seg.uri = mt::AttrString(attrs, "URI");
            seg.resolved_path = mt::ResolveLocalUri(base_dir, seg.uri);
            seg.partial = true;
            seg.has_duration = mt::AttrDouble(attrs, "DURATION", seg.duration_seconds);
            seg.has_init_section = pl.has_init_section;
            seg.init_uri = pl.init_uri;
            seg.container = ContainerFromUri(seg.uri);
            pl.low_latency = true;
            ++pl.partial_count;
            ++partial_index;
            pl.segments.push_back(std::move(seg));
        } else if (name == "#EXT-X-PRELOAD-HINT") {
            pl.low_latency = true;
        } else if (name == "#EXT-X-PART-INF") {
            const auto attrs = mt::ParseHlsAttributes(body);
            mt::AttrDouble(attrs, "PART-TARGET", pl.part_target_seconds);
            pl.low_latency = true;
        }
    }

    pl.total_duration_seconds = cursor;
    if (pending_discontinuity)
        pl.discontinuity_dangling = true;
}

// ---------------------------------------------------------------------------
// master playlist 正文
// ---------------------------------------------------------------------------

void ParseMasterBody(const std::vector<std::string>& lines, const std::string& base_dir,
                     model::StreamingPackageResult& out, const HlsManifestOptions& options) {
    (void)base_dir;
    bool pending_variant = false;
    int pending_variant_index = -1;

    auto add_playlist = [&out](const std::string& uri, const std::string& role, int variant_index,
                               const std::string& group_id) -> int {
        model::MediaPlaylistInfo pl;
        pl.index = static_cast<int>(out.playlists.size());
        pl.uri = uri;
        pl.role = role;
        pl.variant_index = variant_index;
        pl.group_id = group_id;
        out.playlists.push_back(std::move(pl));
        return static_cast<int>(out.playlists.size()) - 1;
    };

    for (const std::string& raw_line : lines) {
        const std::string raw = mt::Trim(raw_line);
        if (raw.empty())
            continue;

        if (raw[0] == '#') {
            const std::string name = TagName(raw);
            const std::string body = TagBody(raw);

            if (name == "#EXT-X-STREAM-INF") {
                if (out.variants.size() >= options.max_variants) {
                    out.truncated = true;
                    pending_variant = false;
                    continue;
                }
                const auto attrs = mt::ParseHlsAttributes(body);
                model::HlsVariantInfo v;
                v.index = static_cast<int>(out.variants.size());
                v.bandwidth_bps = mt::AttrInt64Or(attrs, "BANDWIDTH", 0);
                v.average_bandwidth_bps = mt::AttrInt64Or(attrs, "AVERAGE-BANDWIDTH", 0);
                v.codecs = mt::AttrString(attrs, "CODECS");
                v.resolution = mt::AttrString(attrs, "RESOLUTION");
                v.audio_group = mt::AttrString(attrs, "AUDIO");
                v.video_group = mt::AttrString(attrs, "VIDEO");
                v.subtitle_group = mt::AttrString(attrs, "SUBTITLES");
                mt::AttrDouble(attrs, "FRAME-RATE", v.frame_rate);
                ParseResolution(v.resolution, v.width, v.height);
                mt::SplitCodecs(v.codecs, v.video_codec, v.audio_codec);
                pending_variant_index = static_cast<int>(out.variants.size());
                out.variants.push_back(v);
                pending_variant = true;
                continue;
            }
            if (name == "#EXT-X-MEDIA") {
                const auto attrs = mt::ParseHlsAttributes(body);
                model::HlsRenditionInfo r;
                r.type = mt::ToLower(mt::AttrString(attrs, "TYPE"));
                r.group_id = mt::AttrString(attrs, "GROUP-ID");
                r.name = mt::AttrString(attrs, "NAME");
                r.language = mt::AttrString(attrs, "LANGUAGE");
                r.uri = mt::AttrString(attrs, "URI");
                r.is_default = mt::AttrBool(attrs, "DEFAULT");
                r.autoselect = mt::AttrBool(attrs, "AUTOSELECT");
                if (!r.uri.empty() && out.playlists.size() < options.max_playlists) {
                    r.playlist_index = add_playlist(r.uri, r.type, -1, r.group_id);
                }
                out.renditions.push_back(r);
                continue;
            }
            continue;
        }

        // 非 # 开头：如果上一个是 EXT-X-STREAM-INF，这行就是它的 URI
        if (pending_variant && pending_variant_index >= 0 &&
            static_cast<size_t>(pending_variant_index) < out.variants.size()) {
            out.variants[pending_variant_index].uri = raw;
            if (out.playlists.size() < options.max_playlists) {
                out.variants[pending_variant_index].playlist_index =
                    add_playlist(raw, "video", pending_variant_index, std::string());
            }
            pending_variant = false;
            pending_variant_index = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// 子播放列表加载 / 落盘标记
// ---------------------------------------------------------------------------

void LoadSubPlaylists(model::StreamingPackageResult& out, const HlsManifestOptions& options) {
    for (size_t i = 0; i < out.playlists.size(); ++i) {
        if (static_cast<uint32_t>(i) >= options.max_playlists) {
            out.truncated = true;
            break;
        }
        model::MediaPlaylistInfo& pl = out.playlists[i];
        pl.resolved_path = mt::ResolveLocalUri(out.manifest_dir, pl.uri);
        if (pl.resolved_path.empty()) {
            if (mt::IsRemoteUri(pl.uri))
                out.remote = true;
            continue;
        }
        std::string text;
        if (!ReadFileText(pl.resolved_path, text)) {
            pl.parse_failed = true;
            pl.error_message = "子播放列表无法读取: " + pl.uri;
            continue;
        }
        ParseMediaPlaylistBody(SplitLines(text), mt::DirOf(pl.resolved_path), pl, options);
        if (pl.has_init_section) {
            pl.init_resolved_path = mt::ResolveLocalUri(mt::DirOf(pl.resolved_path), pl.init_uri);
        }
    }
}

void MarkLocalFiles(model::StreamingPackageResult& out) {
    for (model::MediaPlaylistInfo& pl : out.playlists) {
        if (pl.has_init_section && !pl.init_resolved_path.empty()) {
            pl.init_exists = mt::FileSizeOf(pl.init_resolved_path, pl.init_file_size);
        }
        for (model::SegmentInfo& seg : pl.segments) {
            if (seg.resolved_path.empty()) {
                if (!seg.uri.empty())
                    out.remote = true;
                continue;
            }
            seg.exists = mt::FileSizeOf(seg.resolved_path, seg.file_size_bytes);
        }
    }
}

// ---------------------------------------------------------------------------
// Validate 用到的小工具
// ---------------------------------------------------------------------------

std::string FormatSeconds(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds);
    return std::string(buf);
}

std::string FormatBitrate(int64_t bps) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f Mbps", static_cast<double>(bps) / 1000000.0);
    return std::string(buf);
}

void PushIssue(model::StreamingPackageResult& out, const char* code, const char* title, const std::string& detail,
               const std::string& suggestion, model::IssueSeverity severity, double metric_value, double threshold,
               int occurrence_count, double at_seconds, int playlist_index, int variant_index) {
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
    issue.playlist_index = playlist_index;
    issue.variant_index = variant_index;
    out.issues.push_back(std::move(issue));
}

std::string PlaylistLabel(const model::MediaPlaylistInfo& pl, int index) {
    if (pl.role.empty())
        return "playlist #" + std::to_string(index);
    return pl.role + " playlist #" + std::to_string(index);
}

// 收集一个 playlist 里所有 discontinuity 的发生时间（秒）
std::vector<double> DiscontinuityTimes(const model::MediaPlaylistInfo& pl) {
    std::vector<double> times;
    for (const model::SegmentInfo& seg : pl.segments) {
        if (seg.discontinuity_before)
            times.push_back(seg.start_seconds);
    }
    return times;
}

bool SameTimes(const std::vector<double>& a, const std::vector<double>& b, double tolerance) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i] - b[i]) > tolerance)
            return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

HlsManifestAnalyzer::HlsManifestAnalyzer() = default;
HlsManifestAnalyzer::~HlsManifestAnalyzer() = default;

void HlsManifestAnalyzer::Reset() {
    options_ = HlsManifestOptions{};
}

bool HlsManifestAnalyzer::AnalyzeFile(const std::string& file_path, model::StreamingPackageResult& out,
                                      const HlsManifestOptions& options) {
    options_ = options;
    out = model::StreamingPackageResult{};
    out.manifest_path = file_path;
    out.manifest_dir = mt::DirOf(file_path);

    std::string text;
    if (!ReadFileText(file_path, text)) {
        out.valid = false;
        out.error_message = "无法读取清单文件: " + file_path;
        return false;
    }
    if (!ParseText(text, out.manifest_dir, out, options)) {
        if (out.error_message.empty())
            out.error_message = "不是有效的 HLS 清单";
        return false;
    }
    Validate(out, options);
    return true;
}

bool HlsManifestAnalyzer::ParseText(const std::string& text, const std::string& base_dir,
                                    model::StreamingPackageResult& out, const HlsManifestOptions& options) {
    out.manifest_dir = base_dir;
    out.kind = model::StreamingKind::Unknown;

    const std::vector<std::string> lines = SplitLines(text);

    // 第一行必须是 #EXTM3U
    bool has_magic = false;
    for (const std::string& l : lines) {
        const std::string t = mt::Trim(l);
        if (t.empty())
            continue;
        has_magic = (t == "#EXTM3U");
        break;
    }
    if (!has_magic) {
        out.valid = false;
        out.error_message = "缺少 #EXTM3U 头，不是 HLS 清单";
        return false;
    }

    bool master = false;
    for (const std::string& l : lines) {
        if (mt::StartsWith(mt::Trim(l), "#EXT-X-STREAM-INF")) {
            master = true;
            break;
        }
    }

    if (master) {
        out.kind = model::StreamingKind::HlsMaster;
        ParseMasterBody(lines, base_dir, out, options);
        if (options.load_sub_playlists)
            LoadSubPlaylists(out, options);
        out.valid = !out.variants.empty();
        if (!out.valid)
            out.error_message = "主播放列表里没有 EXT-X-STREAM-INF";
    } else {
        out.kind = model::StreamingKind::HlsMedia;
        model::MediaPlaylistInfo pl;
        pl.index = 0;
        pl.role = "main";
        pl.resolved_path = out.manifest_path;
        out.playlists.push_back(pl);
        ParseMediaPlaylistBody(lines, base_dir, out.playlists[0], options);
        if (out.playlists[0].has_init_section) {
            out.playlists[0].init_resolved_path = mt::ResolveLocalUri(base_dir, out.playlists[0].init_uri);
        }
        out.valid = !out.playlists[0].segments.empty();
        if (!out.valid)
            out.error_message = "媒体播放列表里没有分片";
    }

    MarkLocalFiles(out);
    return out.valid;
}

void HlsManifestAnalyzer::Validate(model::StreamingPackageResult& out, const HlsManifestOptions& options) {
    static const std::vector<const char*> kOwnCodes = {
        model::StreamingIssueCode::kHlsMissingTargetDuration, model::StreamingIssueCode::kHlsSegmentOverTarget,
        model::StreamingIssueCode::kHlsSegmentJitter,         model::StreamingIssueCode::kHlsDiscontinuityUnpaired,
        model::StreamingIssueCode::kHlsMissingInitSection,    model::StreamingIssueCode::kHlsEncryptionKey,
        model::StreamingIssueCode::kHlsPartialSegment,
    };
    model::RemoveIssuesByCode(out.issues, kOwnCodes);

    for (size_t pi = 0; pi < out.playlists.size(); ++pi) {
        const model::MediaPlaylistInfo& pl = out.playlists[pi];
        if (pl.parse_failed)
            continue;
        const int playlist_index = static_cast<int>(pi);
        const std::string label = PlaylistLabel(pl, playlist_index);

        // ---- 1) EXT-X-TARGETDURATION 缺失 ----
        if (!pl.has_target_duration && pl.SegmentCount() > 0) {
            PushIssue(out, model::StreamingIssueCode::kHlsMissingTargetDuration, "缺少目标分片时长",
                      label + " 没有 EXT-X-TARGETDURATION。",
                      "补上 #EXT-X-TARGETDURATION:<n>，其值必须不小于任一 EXTINF 四舍五入后的整数值。",
                      model::IssueSeverity::Warning, 0.0, 0.0, 1, -1.0, playlist_index, pl.variant_index);
        }

        // ---- 2) 分片时长超过目标时长 ----
        if (pl.has_target_duration && pl.target_duration_s > 0) {
            int over_count = 0;
            double worst = 0.0;
            double first_at = -1.0;
            for (const model::SegmentInfo& seg : pl.segments) {
                if (seg.partial || !seg.has_duration)
                    continue;
                if (seg.duration_seconds >
                    static_cast<double>(pl.target_duration_s) + options.target_duration_tolerance_s) {
                    ++over_count;
                    if (seg.duration_seconds > worst)
                        worst = seg.duration_seconds;
                    if (first_at < 0.0)
                        first_at = seg.start_seconds;
                }
            }
            if (over_count > 0) {
                PushIssue(out, model::StreamingIssueCode::kHlsSegmentOverTarget, "分片时长超过目标时长",
                          label + " 有 " + std::to_string(over_count) + " 个分片时长为 " + FormatSeconds(worst) +
                              " 秒，超过 EXT-X-TARGETDURATION 声明的 " + std::to_string(pl.target_duration_s) + " 秒。",
                          "把 EXT-X-TARGETDURATION 调到不小于最长分片（向上取整），"
                          "或重新切片让分片时长稳定；否则播放器可能按 target 预取不足而卡顿。",
                          model::IssueSeverity::Warning, worst, static_cast<double>(pl.target_duration_s), over_count,
                          first_at, playlist_index, pl.variant_index);
            }
        }

        // ---- 3) 分片时长抖动（末片允许更短，因此不计入）----
        {
            std::vector<double> durations;
            for (size_t i = 0; i < pl.segments.size(); ++i) {
                const model::SegmentInfo& seg = pl.segments[i];
                if (seg.partial || !seg.has_duration)
                    continue;
                // 最后一个完整分片常常天然更短，不参与抖动统计
                bool is_last_full = true;
                for (size_t j = i + 1; j < pl.segments.size(); ++j) {
                    if (!pl.segments[j].partial) {
                        is_last_full = false;
                        break;
                    }
                }
                if (is_last_full)
                    break;
                durations.push_back(seg.duration_seconds);
            }
            if (durations.size() >= 3) {
                const double mn = *std::min_element(durations.begin(), durations.end());
                const double mx = *std::max_element(durations.begin(), durations.end());
                const double sum = std::accumulate(durations.begin(), durations.end(), 0.0);
                const double mean = sum / static_cast<double>(durations.size());
                if (mean > 0.0) {
                    const double jitter = (mx - mn) / mean;
                    if (jitter > options.duration_jitter_ratio && mx > 0.0) {
                        PushIssue(out, model::StreamingIssueCode::kHlsSegmentJitter, "分片时长抖动过大",
                                  label + " 分片时长在 " + FormatSeconds(mn) + " ~ " + FormatSeconds(mx) +
                                      " 秒之间抖动（相对均值 " + FormatSeconds(mean) + " 的抖动率为 " +
                                      FormatSeconds(jitter) + "，阈值 " + FormatSeconds(options.duration_jitter_ratio) +
                                      "）。",
                                  "ABR 切换依赖稳定的分片边界，建议按固定 GOP 切片"
                                  "（-force_key_frames expr:gte(t,n_forced*N)），并让码率 ladder 的切片点一致。",
                                  model::IssueSeverity::Warning, jitter, options.duration_jitter_ratio, 1, -1.0,
                                  playlist_index, pl.variant_index);
                    }
                }
            }
        }

        // ---- 4) discontinuity 是否成对处理 ----
        if (pl.discontinuity_dangling) {
            PushIssue(out, model::StreamingIssueCode::kHlsDiscontinuityUnpaired, "discontinuity 未闭合",
                      label + " 末尾的 EXT-X-DISCONTINUITY 后面没有分片，该标记无意义。",
                      "删除多余的 EXT-X-DISCONTINUITY，或补上它后面真正发生不连续的分片。", model::IssueSeverity::Error,
                      static_cast<double>(pl.discontinuity_count), 0.0, 1, -1.0, playlist_index, pl.variant_index);
        } else if (pl.discontinuity_count > 0 && !pl.has_discontinuity_sequence) {
            PushIssue(out, model::StreamingIssueCode::kHlsDiscontinuityUnpaired, "discontinuity 未与序号配对",
                      label + " 有 " + std::to_string(pl.discontinuity_count) +
                          " 处 EXT-X-DISCONTINUITY，但没有 EXT-X-DISCONTINUITY-SEQUENCE，"
                          "多码率/多轨之间无法对齐同一处不连续。",
                      "在播放列表开头补 #EXT-X-DISCONTINUITY-SEQUENCE:<n>，"
                      "并让同一内容的所有 variant / 音轨使用一致的序号。",
                      model::IssueSeverity::Warning, static_cast<double>(pl.discontinuity_count), 0.0, 1, -1.0,
                      playlist_index, pl.variant_index);
        }

        // ---- 5) fMP4/CMAF 缺初始化段 ----
        {
            int fmp4_segments = 0;
            for (const model::SegmentInfo& seg : pl.segments) {
                if (!seg.partial && seg.container == model::SegmentContainer::FMP4)
                    ++fmp4_segments;
            }
            if (fmp4_segments > 0 && !pl.has_init_section) {
                PushIssue(out, model::StreamingIssueCode::kHlsMissingInitSection, "缺少初始化段",
                          label + " 有 " + std::to_string(fmp4_segments) +
                              " 个 fMP4/CMAF 分片，但没有 EXT-X-MAP 声明初始化段。",
                          "用 #EXT-X-MAP:URI=\"init.mp4\" 给出含 moov 的初始化段，"
                          "否则播放器拿不到 timescale / 编解码配置，整条流无法起播。",
                          model::IssueSeverity::Warning, static_cast<double>(fmp4_segments), 0.0, 1, -1.0,
                          playlist_index, pl.variant_index);
            }
        }

        // ---- 6) 加密标签（只是登记，不是问题）----
        if (pl.encrypted) {
            std::string detail = label + " 声明了 EXT-X-KEY，METHOD=" + pl.key_method;
            if (!pl.key_uri.empty())
                detail += "，URI=" + pl.key_uri;
            if (!pl.key_format.empty())
                detail += "，KEYFORMAT=" + pl.key_format;
            detail += "。";
            PushIssue(out, model::StreamingIssueCode::kHlsEncryptionKey, "分片已加密", detail,
                      "加密本身不是问题，但要确认密钥服务在目标网络可达；"
                      "SAMPLE-AES 与 fMP4 组合时还要确认播放器是否支持。",
                      model::IssueSeverity::Info, 0.0, 0.0, 1, -1.0, playlist_index, pl.variant_index);
        }

        // ---- 7) LL-HLS 部分分片 ----
        if (pl.partial_count > 0) {
            std::string detail =
                label + " 含 " + std::to_string(pl.partial_count) + " 个 EXT-X-PART 部分分片，属于 Low-Latency HLS";
            if (pl.part_target_seconds > 0.0) {
                detail += "（PART-TARGET=" + FormatSeconds(pl.part_target_seconds) + " 秒）";
            }
            detail += "。";
            PushIssue(out, model::StreamingIssueCode::kHlsPartialSegment, "启用 LL-HLS 部分分片", detail,
                      "LL-HLS 需要服务端支持阻塞式请求（EXT-X-PRELOAD-HINT / _HLS_msn 等参数），"
                      "并确认 CDN 不会缓存部分分片导致延迟反而升高。",
                      model::IssueSeverity::Info, static_cast<double>(pl.partial_count), 0.0, 1, -1.0, playlist_index,
                      pl.variant_index);
        }
    }

    // ---- 8) 跨播放列表的 discontinuity 对齐（同一个 variant 的视频轨与其音轨）----
    for (size_t vi = 0; vi < out.variants.size(); ++vi) {
        const model::HlsVariantInfo& variant = out.variants[vi];
        if (variant.playlist_index < 0 || variant.audio_group.empty())
            continue;

        std::vector<int> group;
        group.push_back(variant.playlist_index);
        for (const model::HlsRenditionInfo& r : out.renditions) {
            if (r.type == "audio" && r.group_id == variant.audio_group && r.playlist_index >= 0) {
                group.push_back(r.playlist_index);
            }
        }
        if (group.size() < 2)
            continue;

        int reference = -1;
        for (int idx : group) {
            if (idx >= 0 && static_cast<size_t>(idx) < out.playlists.size() &&
                out.playlists[idx].discontinuity_count > 0) {
                reference = idx;
                break;
            }
        }
        if (reference < 0)
            continue;

        const std::vector<double> ref_times = DiscontinuityTimes(out.playlists[reference]);
        int mismatch = 0;
        std::string worst_detail;
        for (int idx : group) {
            if (idx == reference)
                continue;
            if (idx < 0 || static_cast<size_t>(idx) >= out.playlists.size())
                continue;
            const std::vector<double> times = DiscontinuityTimes(out.playlists[idx]);
            if (SameTimes(ref_times, times, 0.05))
                continue;
            ++mismatch;
            if (worst_detail.empty()) {
                worst_detail = PlaylistLabel(out.playlists[idx], idx) + " 有 " + std::to_string(times.size()) +
                               " 处不连续，而参考轨有 " + std::to_string(ref_times.size()) + " 处";
            }
        }
        if (mismatch > 0) {
            PushIssue(out, model::StreamingIssueCode::kHlsDiscontinuityUnpaired, "音视频 discontinuity 不对齐",
                      "variant #" + std::to_string(static_cast<int>(vi)) + " 的 " + worst_detail +
                          "，位置或数量不一致。",
                      "让音频与视频在同一时间点插入 EXT-X-DISCONTINUITY，"
                      "并统一 EXT-X-DISCONTINUITY-SEQUENCE；否则切换码率时会出现音画错位。",
                      model::IssueSeverity::Warning, static_cast<double>(mismatch), 0.0, mismatch, -1.0, reference,
                      static_cast<int>(vi));
        }
    }
}

} // namespace analyzer
} // namespace videoeye
