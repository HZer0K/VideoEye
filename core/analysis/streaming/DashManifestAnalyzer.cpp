#include "core/analysis/streaming/DashManifestAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/analysis/streaming/StreamingCancel.h"
#include "core/media/streaming/ManifestReader.h"
#include "core/media/streaming/ManifestText.h"

namespace videoeye {
namespace analyzer {
namespace {

namespace mt = videoeye::utils::manifest;

// ---------------------------------------------------------------------------
// 模板占位符展开
// ---------------------------------------------------------------------------

// DASH 的 $Number%05d$ / $Bandwidth$ / $RepresentationID$ / $Time$
std::string FormatWith(const std::string& fmt, uint64_t value) {
    char buf[64];
    if (fmt.empty()) {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
        return std::string(buf);
    }
    int width = 0;
    size_t i = 1; // 跳过 '%'
    while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
        width = width * 10 + (fmt[i] - '0');
        ++i;
    }
    if (width > 0) {
        std::snprintf(buf, sizeof(buf), "%0*llu", width, static_cast<unsigned long long>(value));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    }
    return std::string(buf);
}

std::string ExpandTemplate(const std::string& tpl, const model::DashRepresentationInfo& rep, uint64_t number,
                           uint64_t time) {
    std::string out;
    size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] == '$' && i + 1 < tpl.size()) {
            if (tpl[i + 1] == '$') {
                out += '$';
                i += 2;
                continue;
            }
            const size_t close = tpl.find('$', i + 1);
            if (close == std::string::npos) {
                out += tpl.substr(i);
                break;
            }
            const std::string ident = tpl.substr(i + 1, close - i - 1);
            std::string name = ident;
            std::string fmt;
            const size_t pct = ident.find('%');
            if (pct != std::string::npos) {
                name = ident.substr(0, pct);
                fmt = ident.substr(pct);
            }
            std::string value;
            if (name == "RepresentationID") {
                value = rep.id;
            } else if (name == "Number") {
                value = FormatWith(fmt, number);
            } else if (name == "Bandwidth") {
                value = FormatWith(fmt, static_cast<uint64_t>(rep.bandwidth_bps));
            } else if (name == "Time") {
                value = FormatWith(fmt, time);
            }
            out += value;
            i = close + 1;
            continue;
        }
        out += tpl[i++];
    }
    return out;
}

// 分片 URI 的解析基址（<BaseURL> 相对清单目录的位置）。
//
// 远端 BaseURL 必须原样返回：JoinPath / NormalizePath 是按本地路径写的 —— 它会把
// "http://a/b/" 里的 "//" 压成 "/"、把 ".." 当目录穿越吃掉，URL 当场就坏了。
std::string BaseUrlDir(const std::string& manifest_dir, const std::string& base_url) {
    return mt::IsRemoteUri(base_url) ? base_url : mt::JoinPath(manifest_dir, base_url);
}

// 无符号乘法，溢出时返回 0（调用方按"这条时间轴推不动了"处理，不能再展开）。
uint64_t MulU64Checked(uint64_t a, uint64_t b) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return 0;
    return a * b;
}

// 时间戳加减前先判上界，避免回绕成天文数字把整条时间轴带歪。
bool AddU64WillOverflow(uint64_t a, uint64_t b) { return a > std::numeric_limits<uint64_t>::max() - b; }

uint64_t SubU64Clamped(uint64_t a, uint64_t b) { return a >= b ? a - b : 0; }

model::SegmentContainer ContainerFromMime(const std::string& mime, const std::string& uri) {
    const std::string m = mt::ToLower(mime);
    if (mt::Contains(m, "webm"))
        return model::SegmentContainer::WebM;
    if (mt::Contains(m, "mp4") || mt::Contains(m, "iso"))
        return model::SegmentContainer::FMP4;
    if (mt::Contains(m, "mpeg") || mt::Contains(m, "ts"))
        return model::SegmentContainer::MPEG_TS;
    const std::string ext = mt::ExtensionOf(uri);
    if (ext == "ts" || ext == "m2ts")
        return model::SegmentContainer::MPEG_TS;
    if (ext == "m4s" || ext == "mp4")
        return model::SegmentContainer::FMP4;
    if (ext == "webm")
        return model::SegmentContainer::WebM;
    return model::SegmentContainer::Unknown;
}

std::string InferContentType(const std::string& adaptation_content_type, const std::string& mime,
                             const model::DashRepresentationInfo& rep) {
    if (!adaptation_content_type.empty())
        return mt::ToLower(adaptation_content_type);
    const std::string m = mt::ToLower(mime);
    if (mt::StartsWith(m, "video"))
        return "video";
    if (mt::StartsWith(m, "audio"))
        return "audio";
    if (mt::StartsWith(m, "text") || mt::StartsWith(m, "application"))
        return "text";
    if (rep.width > 0 && rep.height > 0)
        return "video";
    if (!rep.audio_codec.empty() && rep.video_codec.empty())
        return "audio";
    return std::string();
}

// ---------------------------------------------------------------------------
// 极简 XML 标签扫描
// ---------------------------------------------------------------------------

struct XmlTag {
    std::string name;
    std::map<std::string, std::string> attrs;
    bool closing = false;
    bool self_closing = false;
};

// 从 text[lt] 处的 '<' 开始读一个标签。返回 false 表示文件截断（直接停止扫描）。
bool ReadTag(const std::string& text, size_t lt, XmlTag& tag, size_t& end_pos) {
    size_t gt = lt + 1;
    char quote = 0;
    while (gt < text.size()) {
        const char c = text[gt];
        if (quote != 0) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            break;
        }
        ++gt;
    }
    if (gt >= text.size())
        return false;
    end_pos = gt + 1;

    std::string body = text.substr(lt + 1, gt - lt - 1);
    if (!body.empty() && body[0] == '/') {
        tag.closing = true;
        body.erase(0, 1);
    }
    if (!body.empty() && body.back() == '/') {
        tag.self_closing = true;
        body.pop_back();
    }
    const size_t sp = body.find_first_of(" \t\r\n");
    if (sp == std::string::npos) {
        tag.name = body;
    } else {
        tag.name = body.substr(0, sp);
        tag.attrs = mt::ParseXmlAttributes(body.substr(sp + 1));
    }
    // 去掉命名空间前缀（MPD 里偶尔会出现 <dash:Representation>）
    const size_t colon = tag.name.find(':');
    if (colon != std::string::npos)
        tag.name = tag.name.substr(colon + 1);
    tag.name = mt::Trim(tag.name);
    return true;
}

// ---------------------------------------------------------------------------
// 分片展开
// ---------------------------------------------------------------------------

void BuildSegments(model::StreamingPackageResult& out, model::DashRepresentationInfo& rep,
                   const DashManifestOptions& options, const std::atomic<bool>* cancel) {
    if (!rep.has_template || rep.media_template.empty())
        return;

    // SegmentTimeline 展开是这一层最大的放大点：每条 <S r=N> 会展开成 N 个分片，
    // 上限 max_segments_per_representation（默认 5000）也只是"每个表示各自的上限"。
    if (IsStreamingCancelled(cancel)) {
        rep.truncated = true;
        return;
    }

    const double timescale = rep.timescale > 0 ? static_cast<double>(rep.timescale) : 0.0;
    const std::string rep_dir = BaseUrlDir(out.manifest_dir, rep.base_url);
    // 远端 BaseURL 的分片没有本地路径可算：硬拼一个出来只会拿去 stat，必然全部"缺失"。
    // 这里直接留空 resolved_path，外层按远端分片处理（见 ParseText 末尾的 out.remote）。
    const bool remote_base = mt::IsRemoteUri(rep.base_url);

    auto push_segment = [&](uint64_t number, uint64_t decode_time, uint64_t duration) {
        if (rep.segments.size() >= options.max_segments_per_representation) {
            rep.truncated = true;
            return;
        }
        model::SegmentInfo seg;
        seg.uri = ExpandTemplate(rep.media_template, rep, number, decode_time);
        seg.resolved_path = remote_base ? std::string() : mt::ResolveLocalUri(rep_dir, seg.uri);
        seg.sequence = number;
        seg.duration_seconds = timescale > 0.0 ? static_cast<double>(duration) / timescale : 0.0;
        seg.has_duration = timescale > 0.0 && duration > 0;
        // PTO 是整条时间轴的全局起点，可能大于本分片的 decode_time。无符号相减前
        // 先比大小，否则直接下溢成一个天文数字（start_seconds 不再是"秒"而是巨大值）。
        seg.start_seconds = timescale > 0.0
                                ? static_cast<double>(
                                      SubU64Clamped(decode_time, rep.presentation_time_offset)) /
                                      timescale
                                : 0.0;
        seg.has_init_section = !rep.initialization_template.empty();
        seg.init_uri = rep.initialization_template;
        seg.container = ContainerFromMime(rep.mime_type, seg.uri);
        seg.starts_with_keyframe = (seg.container == model::SegmentContainer::MPEG_TS);
        rep.segments.push_back(std::move(seg));
    };

    if (rep.has_segment_timeline) {
        uint64_t cursor = rep.presentation_time_offset;
        uint64_t number = rep.start_number;
        for (size_t ti = 0; ti < rep.timeline.size(); ++ti) {
            if (IsStreamingCancelled(cancel)) {
                rep.truncated = true;
                break;
            }
            const model::DashTimelineEntry& e = rep.timeline[ti];
            const uint64_t start = e.has_t ? e.t : cursor;
            for (uint32_t k = 0; k <= e.r; ++k) {
                // 到达该表示的分片上限就停。<S r=N> 的 N 可以大到让这个循环跑几分钟，
                // 不能只靠 push_segment 内部那一道判（它得先算出 number/起点才判得出来）。
                if (rep.segments.size() >= options.max_segments_per_representation) {
                    rep.truncated = true;
                    break;
                }
                // r 展开的第 k 段起点是 t + d*k。注意不能直接拿
                // "MulU64Checked 返回 0" 当溢出判据 —— k==0 时 d*0 本来就是 0，
                // 那是**第一个分片**的合法起点。以前这里写成 offset==0 就 break，
                // 结果每条 <S> 都在 k=0 处退出，整条时间轴一个分片都展开不出来
                // (StreamingPackageTests 的两条 DASH 用例就是这么红的)。
                // 所以溢出单独判，乘法在无溢出的前提下才做。
                const bool mul_overflow =
                    e.d != 0 && static_cast<uint64_t>(k) > std::numeric_limits<uint64_t>::max() / e.d;
                if (mul_overflow) break;
                const uint64_t offset = e.d * static_cast<uint64_t>(k);
                if (AddU64WillOverflow(start, offset)) break;             // 加法溢出
                push_segment(number, start + offset, e.d);
                ++number;
            }
            const uint64_t entry_span = MulU64Checked(e.d, static_cast<uint64_t>(e.r) + 1);
            cursor = AddU64WillOverflow(start, entry_span) ? std::numeric_limits<uint64_t>::max()
                                                           : start + entry_span;
        }
    } else if (rep.segment_duration > 0 && rep.timescale > 0) {
        // 直播（dynamic）MPD 无法预知分片数，只能标记为未知
        const double period_duration = rep.total_duration_seconds;
        if (period_duration <= 0.0) {
            rep.segment_count_unknown = true;
            return;
        }
        const uint64_t d = rep.segment_duration;
        const uint64_t count = static_cast<uint64_t>(
            std::ceil(period_duration / (static_cast<double>(d) / static_cast<double>(rep.timescale))));
        uint64_t cursor = rep.presentation_time_offset;
        for (uint64_t i = 0; i < count; ++i) {
            if (ShouldCheckStreamingCancel(i) && IsStreamingCancelled(cancel)) {
                rep.truncated = true;
                break;
            }
            push_segment(rep.start_number + i, cursor, d);
            cursor += d;
        }
    }

    if (!rep.initialization_template.empty()) {
        rep.init_resolved_path = remote_base
                                     ? std::string()
                                     : mt::ResolveLocalUri(
                                           rep_dir, ExpandTemplate(rep.initialization_template, rep, 0, 0));
        rep.init_exists = mt::FileSizeOf(rep.init_resolved_path, rep.init_file_size);
    }

    double total = 0.0;
    size_t stat_count = 0;
    for (model::SegmentInfo& seg : rep.segments) {
        // 每个分片都是一次磁盘 stat，和 HLS 的 MarkLocalFiles 同一个量级
        if (ShouldCheckStreamingCancel(stat_count++) && IsStreamingCancelled(cancel)) {
            rep.truncated = true;
            break;
        }
        seg.exists = mt::FileSizeOf(seg.resolved_path, seg.file_size_bytes);
        total += seg.duration_seconds;
    }
    rep.total_duration_seconds = total;
}

std::string FormatSeconds(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds);
    return std::string(buf);
}

void PushIssue(model::StreamingPackageResult& out, const char* code, const char* title, const std::string& detail,
               const std::string& suggestion, model::IssueSeverity severity, double metric_value, double threshold,
               int occurrence_count, double at_seconds, int representation_index) {
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
    issue.playlist_index = representation_index;
    out.issues.push_back(std::move(issue));
}

} // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

DashManifestAnalyzer::DashManifestAnalyzer() = default;
DashManifestAnalyzer::~DashManifestAnalyzer() = default;

void DashManifestAnalyzer::Reset() {
    options_ = DashManifestOptions{};
}

bool DashManifestAnalyzer::AnalyzeFile(const std::string& file_path, model::StreamingPackageResult& out,
                                       const DashManifestOptions& options, const std::atomic<bool>* cancel) {
    options_ = options;
    out = model::StreamingPackageResult{};
    out.manifest_path = file_path;
    out.manifest_dir = mt::DirOf(file_path);

    // MPD 是 XML，扫描器按字符前进，只能整块读；但照样要"分块读 + 每块检查取消
    // + 体积上限"这三件套，否则一个大 MPD 会把取消按钮卡死在读盘阶段。
    mt::ManifestReadOptions read_options;
    read_options.max_bytes = options.max_manifest_bytes;
    std::string text;
    const mt::ManifestReadStatus status = mt::ReadManifestText(file_path, read_options, cancel, text);
    if (status != mt::ManifestReadStatus::Ok) {
        out.valid = false;
        out.truncated = (status == mt::ManifestReadStatus::TooLarge ||
                         status == mt::ManifestReadStatus::Cancelled);
        out.error_message = mt::ManifestReadErrorMessage(status, file_path, options.max_manifest_bytes);
        return false;
    }

    if (!ParseText(text, out.manifest_dir, out, options, cancel)) {
        if (out.error_message.empty())
            out.error_message = "不是有效的 DASH MPD";
        return false;
    }
    Validate(out, options, cancel);
    return true;
}

bool DashManifestAnalyzer::ParseText(const std::string& text, const std::string& base_dir,
                                     model::StreamingPackageResult& out, const DashManifestOptions& options,
                                     const std::atomic<bool>* cancel) {
    out.manifest_dir = base_dir;
    out.kind = model::StreamingKind::Dash;

    int cur_period = -1;
    int cur_as = -1;
    int cur_rep = -1;
    std::vector<std::string> stack;

    // 泛型 lambda：把 SegmentTemplate 的属性同时写进 Representation 或 AdaptationSet
    auto with_template_owner = [&](auto&& fn) {
        if (cur_rep >= 0 && static_cast<size_t>(cur_rep) < out.representations.size()) {
            fn(out.representations[cur_rep]);
        } else if (cur_period >= 0 && cur_as >= 0 && static_cast<size_t>(cur_period) < out.periods.size() &&
                   static_cast<size_t>(cur_as) < out.periods[cur_period].adaptation_sets.size()) {
            fn(out.periods[cur_period].adaptation_sets[cur_as]);
        }
    };

    size_t pos = 0;
    size_t tag_count = 0;
    while (pos < text.size()) {
        // MPD 的解析是标签扫描 + 逐标签建表示，超大 MPD 同样要能被中断
        if (ShouldCheckStreamingCancel(tag_count++) && IsStreamingCancelled(cancel)) {
            out.truncated = true;
            break;
        }
        const size_t lt = text.find('<', pos);
        if (lt == std::string::npos)
            break;

        // 文本节点：只有 <BaseURL> 的内容对我们有用
        if (!stack.empty() && stack.back() == "BaseURL") {
            const std::string value = mt::Trim(text.substr(pos, lt - pos));
            if (!value.empty()) {
                // 下标一并查上界：cur_rep / cur_as 只保证 >=0，region 上限是被
                // max_* 选项卡住的，两层判据不一致时这里会越界。
                if (cur_rep >= 0 && static_cast<size_t>(cur_rep) < out.representations.size()) {
                    out.representations[cur_rep].base_url = value;
                } else if (cur_period >= 0 && cur_as >= 0 &&
                           static_cast<size_t>(cur_period) < out.periods.size() &&
                           static_cast<size_t>(cur_as) <
                               out.periods[cur_period].adaptation_sets.size()) {
                    // AdaptationSet 级 BaseURL：下属 Representation 继承
                    const std::vector<int>& inherited =
                        out.periods[cur_period].adaptation_sets[cur_as].representation_indices;
                    for (int ri : inherited) {
                        if (ri >= 0 && static_cast<size_t>(ri) < out.representations.size()) {
                            out.representations[ri].base_url = value;
                        }
                    }
                }
            }
        }

        if (text.compare(lt, 4, "<!--") == 0) {
            const size_t e = text.find("-->", lt);
            pos = (e == std::string::npos) ? text.size() : e + 3;
            continue;
        }
        if (text.compare(lt, 2, "<?") == 0) {
            const size_t e = text.find("?>", lt);
            pos = (e == std::string::npos) ? text.size() : e + 2;
            continue;
        }

        XmlTag tag;
        size_t end_pos = lt + 1;
        if (!ReadTag(text, lt, tag, end_pos))
            break;
        pos = end_pos;

        if (tag.closing) {
            if (!stack.empty() && stack.back() == tag.name)
                stack.pop_back();
            if (tag.name == "Representation")
                cur_rep = -1;
            if (tag.name == "AdaptationSet")
                cur_as = -1;
            if (tag.name == "Period")
                cur_period = -1;
            continue;
        }

        if (tag.name == "MPD") {
            out.mpd_type = mt::ToLower(mt::AttrString(tag.attrs, "type"));
            if (out.mpd_type.empty())
                out.mpd_type = "static"; // DASH 规范默认值
            out.mpd_profiles = mt::AttrString(tag.attrs, "profiles");
            out.media_presentation_duration_s =
                mt::ParseIso8601Duration(mt::AttrString(tag.attrs, "mediaPresentationDuration"));
            out.min_buffer_time_s = mt::ParseIso8601Duration(mt::AttrString(tag.attrs, "minBufferTime"));
            const std::string max_seg = mt::AttrString(tag.attrs, "maxSegmentDuration");
            out.max_segment_duration_s = mt::ParseIso8601Duration(max_seg);
        } else if (tag.name == "Period") {
            if (out.periods.size() >= options.max_periods) {
                out.truncated = true;
                cur_period = -1;
            } else {
                model::DashPeriodInfo p;
                p.index = static_cast<int>(out.periods.size());
                p.id = mt::AttrString(tag.attrs, "id");
                p.start_seconds = mt::ParseIso8601Duration(mt::AttrString(tag.attrs, "start"));
                const std::string dur = mt::AttrString(tag.attrs, "duration");
                if (!dur.empty()) {
                    p.duration_seconds = mt::ParseIso8601Duration(dur);
                    p.has_duration = true;
                }
                out.periods.push_back(p);
                cur_period = p.index;
                cur_as = -1;
                cur_rep = -1;
            }
        } else if (tag.name == "AdaptationSet") {
            if (cur_period < 0 ||
                out.periods[cur_period].adaptation_sets.size() >= options.max_adaptation_sets_per_period) {
                out.truncated = true;
                cur_as = -1;
            } else {
                model::DashAdaptationSetInfo as;
                as.index = static_cast<int>(out.periods[cur_period].adaptation_sets.size());
                as.content_type = mt::ToLower(mt::AttrString(tag.attrs, "contentType"));
                as.mime_type = mt::AttrString(tag.attrs, "mimeType");
                as.lang = mt::AttrString(tag.attrs, "lang");
                as.par = mt::AttrString(tag.attrs, "par");
                out.periods[cur_period].adaptation_sets.push_back(as);
                cur_as = as.index;
                cur_rep = -1;
            }
        } else if (tag.name == "Representation") {
            if (cur_period < 0 || cur_as < 0 || out.representations.size() >= options.max_representations) {
                out.truncated = true;
                cur_rep = -1;
            } else {
                model::DashRepresentationInfo rep;
                rep.index = static_cast<int>(out.representations.size());
                rep.period_index = cur_period;
                rep.adaptation_index = cur_as;
                rep.id = mt::AttrString(tag.attrs, "id");
                rep.bandwidth_bps = mt::AttrInt64Or(tag.attrs, "bandwidth", 0);
                rep.width = static_cast<int>(mt::AttrInt64Or(tag.attrs, "width", 0));
                rep.height = static_cast<int>(mt::AttrInt64Or(tag.attrs, "height", 0));
                rep.codecs = mt::AttrString(tag.attrs, "codecs");
                rep.mime_type = mt::AttrString(tag.attrs, "mimeType");
                mt::AttrDouble(tag.attrs, "frameRate", rep.frame_rate);
                mt::SplitCodecs(rep.codecs, rep.video_codec, rep.audio_codec);
                // 这里读的是 Period / AdaptationSet 的内部内容，两个下标都要连上界一起查，
                // 和上面 <Period> / <AdaptationSet> 两处 push 时的判据保持一致。
                const model::DashPeriodInfo& cur_period_ref = out.periods[cur_period];
                const model::DashAdaptationSetInfo& cur_as_ref =
                    cur_period_ref.adaptation_sets[cur_as];
                rep.content_type = InferContentType(cur_as_ref.content_type, rep.mime_type, rep);
                // 时长：Period@duration 优先，其次整个 MPD 的 mediaPresentationDuration
                rep.total_duration_seconds =
                    cur_period_ref.has_duration ? cur_period_ref.duration_seconds
                                                : out.media_presentation_duration_s;
                out.representations.push_back(rep);
                cur_rep = rep.index;
                out.periods[cur_period].adaptation_sets[cur_as].representation_indices.push_back(cur_rep);
            }
        } else if (tag.name == "SegmentTemplate") {
            with_template_owner([&](auto& owner) {
                owner.has_template = true;
                owner.timescale = static_cast<uint32_t>(mt::AttrInt64Or(tag.attrs, "timescale", 1));
                owner.segment_duration = static_cast<uint64_t>(mt::AttrInt64Or(tag.attrs, "duration", 0));
                owner.start_number = static_cast<uint64_t>(mt::AttrInt64Or(tag.attrs, "startNumber", 1));
                owner.presentation_time_offset =
                    static_cast<uint64_t>(mt::AttrInt64Or(tag.attrs, "presentationTimeOffset", 0));
                owner.initialization_template = mt::AttrString(tag.attrs, "initialization");
                owner.media_template = mt::AttrString(tag.attrs, "media");
            });
        } else if (tag.name == "SegmentTimeline") {
            with_template_owner([](auto& owner) { owner.has_segment_timeline = true; });
        } else if (tag.name == "S") {
            if (cur_rep < 0 && !(cur_period >= 0 && cur_as >= 0))
                continue;
            model::DashTimelineEntry e;
            int64_t t = 0;
            if (mt::AttrInt64(tag.attrs, "t", t)) {
                e.t = static_cast<uint64_t>(t);
                e.has_t = true;
            }
            e.d = static_cast<uint64_t>(mt::AttrInt64Or(tag.attrs, "d", 0));
            e.r = static_cast<uint32_t>(mt::AttrInt64Or(tag.attrs, "r", 0));
            with_template_owner([&](auto& owner) {
                if (owner.timeline.size() < options.max_timeline_entries) {
                    owner.timeline.push_back(e);
                } else {
                    out.truncated = true;
                }
            });
        } else if (tag.name == "SegmentBase") {
            if (cur_rep >= 0)
                out.representations[cur_rep].has_segment_base = true;
        } else if (tag.name == "SegmentList") {
            if (cur_rep >= 0)
                out.representations[cur_rep].has_segment_list = true;
        } else if (tag.name == "SegmentURL") {
            if (cur_rep >= 0 &&
                out.representations[cur_rep].segments.size() < options.max_segments_per_representation) {
                model::SegmentInfo seg;
                seg.uri = mt::AttrString(tag.attrs, "media");
                // 远端 BaseURL 不进本地路径拼接（JoinPath 会把 URL 拆坏），
                // resolved_path 直接留空，按远端分片处理。
                seg.resolved_path =
                    mt::IsRemoteUri(out.representations[cur_rep].base_url)
                        ? std::string()
                        : mt::ResolveLocalUri(
                              mt::JoinPath(out.manifest_dir, out.representations[cur_rep].base_url),
                              seg.uri);
                seg.container = ContainerFromMime(out.representations[cur_rep].mime_type, seg.uri);
                out.representations[cur_rep].segments.push_back(std::move(seg));
            }
        }

        if (!tag.self_closing)
            stack.push_back(tag.name);
    }

    out.valid = !out.representations.empty();
    if (!out.valid) {
        out.error_message = "MPD 里没有 Representation";
        return false;
    }

    // Representation 继承 AdaptationSet 级别的 SegmentTemplate
    for (model::DashRepresentationInfo& rep : out.representations) {
        if (rep.has_template)
            continue;
        if (rep.period_index < 0 || rep.adaptation_index < 0)
            continue;
        const model::DashAdaptationSetInfo& as = out.periods[rep.period_index].adaptation_sets[rep.adaptation_index];
        if (!as.has_template)
            continue;
        rep.has_template = true;
        rep.timescale = as.timescale;
        rep.segment_duration = as.segment_duration;
        rep.start_number = as.start_number;
        rep.presentation_time_offset = as.presentation_time_offset;
        rep.initialization_template = as.initialization_template;
        rep.media_template = as.media_template;
        rep.has_segment_timeline = as.has_segment_timeline;
        rep.timeline = as.timeline;
    }

    if (options.expand_segments) {
        for (model::DashRepresentationInfo& rep : out.representations) {
            BuildSegments(out, rep, options, cancel);
        }
    }

    for (const model::DashRepresentationInfo& rep : out.representations) {
        for (const model::SegmentInfo& seg : rep.segments) {
            if (seg.resolved_path.empty() && !seg.uri.empty())
                out.remote = true;
        }
    }

    return true;
}

void DashManifestAnalyzer::Validate(model::StreamingPackageResult& out, const DashManifestOptions& options,
                                    const std::atomic<bool>* cancel) {
    static const std::vector<const char*> kOwnCodes = {
        model::StreamingIssueCode::kDashSegmentTimelineGap,
        model::StreamingIssueCode::kDashSegmentTimelineOverlap,
        model::StreamingIssueCode::kDashMissingSegmentInfo,
    };
    model::RemoveIssuesByCode(out.issues, kOwnCodes);

    for (size_t ri = 0; ri < out.representations.size(); ++ri) {
        // SegmentTimeline 的缺口/重叠比对是"每个表示扫一遍时间轴"，表示多时要能中断
        if (IsStreamingCancelled(cancel)) return;
        const model::DashRepresentationInfo& rep = out.representations[ri];
        const int rep_index = static_cast<int>(ri);
        const std::string label = "Representation " + (rep.id.empty() ? std::to_string(ri) : rep.id);

        // ---- 1) 完全没有分片定位信息 ----
        if (!rep.has_template && !rep.has_segment_list && !rep.has_segment_base) {
            PushIssue(out, model::StreamingIssueCode::kDashMissingSegmentInfo, "缺少分片定位信息",
                      label + " 既没有 SegmentTemplate，也没有 SegmentList / SegmentBase，"
                              "客户端无法构造分片 URL。",
                      "补 SegmentTemplate（推荐，配合 $Number$ 或 SegmentTimeline），"
                      "或至少给出 SegmentBase + indexRange 以支持 onDemand 单文件播放。",
                      model::IssueSeverity::Error, 0.0, 0.0, 1, -1.0, rep_index);
        }

        // ---- 2) SegmentTimeline 缺口 / 重叠 ----
        if (rep.has_segment_timeline && rep.timeline.size() >= 2 && rep.timescale > 0) {
            uint64_t cursor = rep.presentation_time_offset;
            int gaps = 0;
            int overlaps = 0;
            double worst_gap = 0.0;
            double worst_overlap = 0.0;
            double gap_at = -1.0;
            double overlap_at = -1.0;

            for (const model::DashTimelineEntry& e : rep.timeline) {
                const uint64_t start = e.has_t ? e.t : cursor;
                if (e.has_t) {
                    if (start > cursor + options.timeline_tolerance) {
                        const double gap_seconds =
                            static_cast<double>(start - cursor) / static_cast<double>(rep.timescale);
                        ++gaps;
                        if (gap_seconds > worst_gap)
                            worst_gap = gap_seconds;
                        if (gap_at < 0.0)
                            gap_at = static_cast<double>(cursor) / rep.timescale;
                    } else if (start + options.timeline_tolerance < cursor) {
                        const double overlap_seconds =
                            static_cast<double>(cursor - start) / static_cast<double>(rep.timescale);
                        ++overlaps;
                        if (overlap_seconds > worst_overlap)
                            worst_overlap = overlap_seconds;
                        if (overlap_at < 0.0)
                            overlap_at = static_cast<double>(start) / rep.timescale;
                    }
                }
                // 先判再乘/再加：d 与 r 都来自外部 MPD，乘出来回绕会把 cursor 推到
                // 一个小数上，后面每一处"缺口/重叠"全部误判。
                const uint64_t entry_span = MulU64Checked(e.d, static_cast<uint64_t>(e.r) + 1);
                cursor = AddU64WillOverflow(start, entry_span) ? std::numeric_limits<uint64_t>::max()
                                                              : start + entry_span;
            }

            if (gaps > 0) {
                PushIssue(out, model::StreamingIssueCode::kDashSegmentTimelineGap, "SegmentTimeline 存在时间缺口",
                          label + " 的 SegmentTimeline 有 " + std::to_string(gaps) + " 处时间不连续，最大缺口 " +
                              FormatSeconds(worst_gap) + " 秒。",
                          "补齐缺失的分片，或用 <S t=...> 显式声明每个区段的起点。"
                          "缺口会让播放器缓冲失败或跳播。",
                          model::IssueSeverity::Error, worst_gap, 0.0, gaps, gap_at, rep_index);
            }
            if (overlaps > 0) {
                PushIssue(out, model::StreamingIssueCode::kDashSegmentTimelineOverlap, "SegmentTimeline 存在时间重叠",
                          label + " 的 SegmentTimeline 有 " + std::to_string(overlaps) + " 处时间倒退，最大重叠 " +
                              FormatSeconds(worst_overlap) + " 秒。",
                          "修正 <S> 的 t / d / r，让各区段首尾相接；重叠会导致重复播放与音画错位。",
                          model::IssueSeverity::Error, worst_overlap, 0.0, overlaps, overlap_at, rep_index);
            }
        }
    }
}

} // namespace analyzer
} // namespace videoeye
