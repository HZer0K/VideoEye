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
namespace {

namespace mt = videoeye::manifest;

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
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
        return 0;
    return a * b;
}

// 时间戳加减前先判上界，避免回绕成天文数字把整条时间轴带歪。
bool AddU64WillOverflow(uint64_t a, uint64_t b) {
    return a > std::numeric_limits<uint64_t>::max() - b;
}

uint64_t SubU64Clamped(uint64_t a, uint64_t b) {
    return a >= b ? a - b : 0;
}

// ---------------------------------------------------------------------------
// DRM / 描述符小工具
// ---------------------------------------------------------------------------

// 属性名带命名空间前缀是常态（cenc:default_KID、cenc:pssh），而我们的标签扫描器
// 只剥掉标签名的前缀、不动属性名。所以按"忽略前缀"的方式查一次。
std::string AttrIgnoreNs(const std::map<std::string, std::string>& attrs, const std::string& key) {
    const auto it = attrs.find(key);
    if (it != attrs.end())
        return it->second;
    for (const auto& kv : attrs) {
        const size_t colon = kv.first.find(':');
        if (colon != std::string::npos && kv.first.substr(colon + 1) == key)
            return kv.second;
    }
    return std::string();
}

// schemeIdUri -> 可读的 DRM 系统名。认不出来就返回空串（调用方按"未知方案"报）。
std::string DrmSystemName(const std::string& scheme) {
    const std::string s = mt::ToLower(mt::Trim(scheme));
    if (s == "urn:mpeg:dash:mp4protection:2011")
        return "CENC (mp4protection)";
    if (s == "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed")
        return "Widevine";
    if (s == "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95")
        return "PlayReady";
    if (s == "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e")
        return "ClearKey";
    if (s == "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2")
        return "FairPlay";
    if (s == "urn:uuid:1077efec-c0b2-4d02-ace3-3c1e52e2fb4b")
        return "W3C Common PSSH";
    if (s == "urn:uuid:279fe473-512c-48fe-ade8-d176fee6b40f")
        return "Adobe Primetime";
    return std::string();
}

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
        seg.start_seconds =
            timescale > 0.0 ? static_cast<double>(SubU64Clamped(decode_time, rep.presentation_time_offset)) / timescale
                            : 0.0;
        seg.has_init_section = !rep.initialization_template.empty();
        seg.init_uri = rep.initialization_template;
        seg.container = ContainerFromMime(rep.mime_type, seg.uri);
        seg.starts_with_keyframe = (seg.container == model::SegmentContainer::MPEG_TS);
        seg.keyframe_evidence =
            seg.starts_with_keyframe ? model::KeyframeEvidence::Declared : model::KeyframeEvidence::Unknown;
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
                if (mul_overflow)
                    break;
                const uint64_t offset = e.d * static_cast<uint64_t>(k);
                if (AddU64WillOverflow(start, offset))
                    break; // 加法溢出
                push_segment(number, start + offset, e.d);
                ++number;
            }
            const uint64_t entry_span = MulU64Checked(e.d, static_cast<uint64_t>(e.r) + 1);
            cursor = AddU64WillOverflow(start, entry_span) ? std::numeric_limits<uint64_t>::max() : start + entry_span;
        }
    } else if (rep.segment_duration > 0 && rep.timescale > 0) {
        // 窗口长度：先看 Period@duration / mediaPresentationDuration，两者都没有时
        // 退到 timeShiftBufferDepth —— 它描述服务端保留多久的历史分片，
        // 对 dynamic MPD 来说就是"这份快照大致覆盖了多长"的唯一线索。
        // 以前这里只看 Period 时长，直播包因此一步就落到"分片数未知"分支，
        // 结果是 0 分片、0 告警，用户完全不知道为什么什么都分析不出来。
        double window_seconds = rep.total_duration_seconds;
        if (window_seconds <= 0.0)
            window_seconds = out.EffectiveWindowSeconds();
        if (window_seconds <= 0.0) {
            rep.segment_count_unknown = true;
            return;
        }
        const uint64_t d = rep.segment_duration;
        const double seg_seconds = static_cast<double>(d) / static_cast<double>(rep.timescale);
        const double raw_count = seg_seconds > 0.0 ? std::ceil(window_seconds / seg_seconds) : 0.0;
        // duration=1 / timescale=1e9 配一段极长 Period，算出来的分片数能到 1e15
        // （阶段 3 语料 dash_extreme_count.mpd），甚至可能溢出成 inf/NaN ——
        // ceil 的结果直接 static_cast<uint64_t> 是 UB。先夹到分片上限 +1：
        // 超出上限的精确值没有任何用途，循环会在上限处截断退出。
        const uint64_t max_segments = options.max_segments_per_representation;
        uint64_t count = 0;
        if (raw_count > 0.0) {
            count =
                (raw_count >= static_cast<double>(max_segments)) ? max_segments + 1 : static_cast<uint64_t>(raw_count);
        }
        uint64_t cursor = rep.presentation_time_offset;
        for (uint64_t i = 0; i < count; ++i) {
            // 分片上限硬约束：不能依赖 push_segment 内部那一道判 —— 它不阻止
            // 循环继续推进（阶段 3 前这里就是"解析一份清单永不返回"的来源）。
            if (rep.segments.size() >= max_segments) {
                rep.truncated = true;
                break;
            }
            if (ShouldCheckStreamingCancel(i) && IsStreamingCancelled(cancel)) {
                rep.truncated = true;
                break;
            }
            push_segment(rep.start_number + i, cursor, d);
            if (AddU64WillOverflow(cursor, d)) {
                rep.truncated = true;
                break;
            }
            cursor += d;
        }
    }

    if (!rep.initialization_template.empty()) {
        rep.init_resolved_path =
            remote_base ? std::string()
                        : mt::ResolveLocalUri(rep_dir, ExpandTemplate(rep.initialization_template, rep, 0, 0));
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
        out.truncated = (status == mt::ManifestReadStatus::TooLarge || status == mt::ManifestReadStatus::Cancelled);
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
    int cur_event_stream = -1;
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

    // ContentProtection / Role / EssentialProperty 的宿主与上面完全一致
    // （Representation 优先，否则 AdaptationSet），复用同一套下标判定。
    auto with_descriptor_owner = [&](auto&& fn) { with_template_owner(fn); };

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
                } else if (cur_period >= 0 && cur_as >= 0 && static_cast<size_t>(cur_period) < out.periods.size() &&
                           static_cast<size_t>(cur_as) < out.periods[cur_period].adaptation_sets.size()) {
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

        // <Event> 的正文（<EventStream><Event><messageData>...</messageData>）
        // 只用来判定"这条事件有没有带载荷"，内容本身不解析（base64 的 SCTE-35 命令
        // 属于业务侧，硬解出来对 QC 结论没有增量）。
        if (!stack.empty() && stack.back() == "messageData" && cur_event_stream >= 0 &&
            static_cast<size_t>(cur_event_stream) < out.event_streams.size()) {
            const std::string value = mt::Trim(text.substr(pos, lt - pos));
            if (!value.empty()) {
                model::DashEventStreamInfo& es = out.event_streams[static_cast<size_t>(cur_event_stream)];
                if (!es.events.empty())
                    es.events.back().has_message_data = true;
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
            if (tag.name == "EventStream")
                cur_event_stream = -1;
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

            // ---- 直播（dynamic）语义三件套 ----
            // availabilityStartTime / minimumUpdatePeriod / timeShiftBufferDepth 都在 MPD 元素上，
            // 以前一个都没解析：dynamic MPD 因此既不展开分片、也不报任何 issue，
            // 界面上就是"打开了一份直播包，结果空空如也"。
            const std::string ast = mt::AttrString(tag.attrs, "availabilityStartTime");
            if (!ast.empty()) {
                out.availability_start_time = ast;
                out.has_availability_start_time = true;
            }
            out.publish_time = mt::AttrString(tag.attrs, "publishTime");
            const std::string mup = mt::AttrString(tag.attrs, "minimumUpdatePeriod");
            if (!mup.empty()) {
                // MUP=0 是合法值（表示分片一可用就立刻更新策略），要靠"属性是否存在"
                // 而不是"值是否为 0"来判缺失。
                out.minimum_update_period_s = mt::ParseIso8601Duration(mup);
                out.has_minimum_update_period = true;
            }
            const std::string tsbd = mt::AttrString(tag.attrs, "timeShiftBufferDepth");
            if (!tsbd.empty()) {
                out.time_shift_buffer_depth_s = mt::ParseIso8601Duration(tsbd);
                out.has_time_shift_buffer_depth = true;
            }
            const std::string spd = mt::AttrString(tag.attrs, "suggestedPresentationDelay");
            if (!spd.empty()) {
                out.suggested_presentation_delay_s = mt::ParseIso8601Duration(spd);
                out.has_suggested_presentation_delay = true;
            }
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
                as.start_with_sap = static_cast<int>(mt::AttrInt64Or(tag.attrs, "startWithSAP", -1));
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
                // Representation 上也可以覆盖 startWithSAP（比 AdaptationSet 更具体）
                rep.start_with_sap = static_cast<int>(mt::AttrInt64Or(tag.attrs, "startWithSAP", -1));
                mt::SplitCodecs(rep.codecs, rep.video_codec, rep.audio_codec);
                // 这里读的是 Period / AdaptationSet 的内部内容，两个下标都要连上界一起查，
                // 和上面 <Period> / <AdaptationSet> 两处 push 时的判据保持一致。
                const model::DashPeriodInfo& cur_period_ref = out.periods[cur_period];
                const model::DashAdaptationSetInfo& cur_as_ref = cur_period_ref.adaptation_sets[cur_as];
                rep.content_type = InferContentType(cur_as_ref.content_type, rep.mime_type, rep);
                // 时长：Period@duration 优先，其次整个 MPD 的 mediaPresentationDuration
                rep.total_duration_seconds =
                    cur_period_ref.has_duration ? cur_period_ref.duration_seconds : out.media_presentation_duration_s;
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
                // 负 t 直接 static_cast 到 uint64 会回绕成天文数字起点，后续
                // 段起点全部错位。规范里 t 只允许非负，负值按 0 处理。
                e.t = t > 0 ? static_cast<uint64_t>(t) : 0;
                e.has_t = true;
            }
            const int64_t d_raw = mt::AttrInt64Or(tag.attrs, "d", 0);
            e.d = d_raw > 0 ? static_cast<uint64_t>(d_raw) : 0;
            // r 是"额外重复次数"，规范只允许 >= 0。负值 static_cast 到 uint32
            // 会回绕成 43 亿，时间轴要展开到分片上限才停（阶段 3 语料
            // dash_negative_r.mpd）。夹到 [0, UINT32_MAX]，负值等价于不重复。
            const int64_t r_raw = mt::AttrInt64Or(tag.attrs, "r", 0);
            e.r = r_raw > 0 ? static_cast<uint32_t>(std::min<int64_t>(r_raw, 0xFFFFFFFFLL)) : 0;
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
        } else if (tag.name == "ContentProtection") {
            // DRM：以前这个元素零匹配，加密流在报告里就是一个"看起来正常"的空壳。
            model::DashContentProtectionInfo cp;
            cp.scheme_id_uri = AttrIgnoreNs(tag.attrs, "schemeIdUri");
            cp.value = AttrIgnoreNs(tag.attrs, "value");
            cp.default_kid = AttrIgnoreNs(tag.attrs, "default_KID");
            cp.robustness = AttrIgnoreNs(tag.attrs, "robustness");
            with_descriptor_owner([&](auto& owner) {
                if (owner.content_protections.size() >= options.max_descriptors) {
                    out.truncated = true;
                    return;
                }
                owner.content_protections.push_back(cp);
                owner.has_content_protection = true;
            });
        } else if (tag.name == "pssh") {
            // <cenc:pssh> 是 ContentProtection 的子元素，标记它挂在最后一条上
            with_descriptor_owner([](auto& owner) {
                if (!owner.content_protections.empty())
                    owner.content_protections.back().has_pssh = true;
            });
        } else if (tag.name == "Role") {
            model::DashDescriptor d;
            d.scheme_id_uri = AttrIgnoreNs(tag.attrs, "schemeIdUri");
            d.value = AttrIgnoreNs(tag.attrs, "value");
            d.id = AttrIgnoreNs(tag.attrs, "id");
            with_descriptor_owner([&](auto& owner) {
                if (owner.roles.size() >= options.max_descriptors) {
                    out.truncated = true;
                    return;
                }
                owner.roles.push_back(d);
            });
        } else if (tag.name == "EssentialProperty") {
            model::DashDescriptor d;
            d.scheme_id_uri = AttrIgnoreNs(tag.attrs, "schemeIdUri");
            d.value = AttrIgnoreNs(tag.attrs, "value");
            d.id = AttrIgnoreNs(tag.attrs, "id");
            with_descriptor_owner([&](auto& owner) {
                if (owner.essential_properties.size() >= options.max_descriptors) {
                    out.truncated = true;
                    return;
                }
                owner.essential_properties.push_back(d);
            });
        } else if (tag.name == "UTCTiming") {
            if (out.utc_timings.size() < options.max_descriptors) {
                model::DashUtcTimingInfo t;
                t.scheme_id_uri = AttrIgnoreNs(tag.attrs, "schemeIdUri");
                t.value = AttrIgnoreNs(tag.attrs, "value");
                out.utc_timings.push_back(std::move(t));
            } else {
                out.truncated = true;
            }
        } else if (tag.name == "EventStream") {
            if (cur_period < 0 || out.event_streams.size() >= options.max_event_streams) {
                out.event_streams_truncated = true;
                out.truncated = true;
                cur_event_stream = -1;
            } else {
                model::DashEventStreamInfo es;
                es.period_index = cur_period;
                es.scheme_id_uri = AttrIgnoreNs(tag.attrs, "schemeIdUri");
                es.value = AttrIgnoreNs(tag.attrs, "value");
                const int64_t ts = mt::AttrInt64Or(tag.attrs, "timescale", 1);
                es.timescale = ts > 0 ? static_cast<uint32_t>(ts) : 0;
                out.event_streams.push_back(std::move(es));
                cur_event_stream = static_cast<int>(out.event_streams.size()) - 1;
            }
        } else if (tag.name == "Event") {
            if (cur_event_stream >= 0 && static_cast<size_t>(cur_event_stream) < out.event_streams.size()) {
                model::DashEventStreamInfo& es = out.event_streams[static_cast<size_t>(cur_event_stream)];
                if (es.events.size() < options.max_events_per_stream) {
                    model::DashEventInfo ev;
                    int64_t pt = 0;
                    if (mt::AttrInt64(tag.attrs, "presentationTime", pt)) {
                        ev.has_presentation_time = true;
                        ev.presentation_time_s =
                            es.timescale > 0 ? static_cast<double>(pt) / static_cast<double>(es.timescale) : 0.0;
                    }
                    int64_t dur = 0;
                    if (mt::AttrInt64(tag.attrs, "duration", dur)) {
                        ev.has_duration = true;
                        ev.duration_s =
                            es.timescale > 0 ? static_cast<double>(dur) / static_cast<double>(es.timescale) : 0.0;
                    }
                    ev.id = AttrIgnoreNs(tag.attrs, "id");
                    es.events.push_back(std::move(ev));
                } else {
                    es.truncated = true;
                    out.truncated = true;
                }
            }
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
                        : mt::ResolveLocalUri(mt::JoinPath(out.manifest_dir, out.representations[cur_rep].base_url),
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
        if (rep.period_index < 0 || rep.adaptation_index < 0)
            continue;
        const model::DashAdaptationSetInfo& as = out.periods[rep.period_index].adaptation_sets[rep.adaptation_index];

        // 描述符继承（ContentProtection / Role / EssentialProperty）。
        // 规范允许三者都写在 AdaptationSet 上让下属 Representation 共享；只解析
        // Representation 上的那几份，加密流就会因为"加密声明写在外层"而被漏判。
        if (rep.content_protections.empty() && !as.content_protections.empty()) {
            rep.content_protections = as.content_protections;
            rep.has_content_protection = as.has_content_protection;
        }
        if (rep.roles.empty() && !as.roles.empty())
            rep.roles = as.roles;
        if (rep.essential_properties.empty() && !as.essential_properties.empty())
            rep.essential_properties = as.essential_properties;
        // startWithSAP 的继承：Representation 没写就沿用 AdaptationSet 的值，
        // 沿用后仍是 -1 才叫"未声明"。这一位决定「清单有没有承诺分片可独立切换」，
        // 漏继承会让写在 AdaptationSet 上的声明被当成没写。
        if (rep.start_with_sap < 0)
            rep.start_with_sap = as.start_with_sap;

        if (rep.has_template)
            continue;
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
        model::StreamingIssueCode::kDashLiveMissingAvailabilityStartTime,
        model::StreamingIssueCode::kDashLiveMissingMinimumUpdatePeriod,
        model::StreamingIssueCode::kDashLiveUpdatePeriodTooLong,
        model::StreamingIssueCode::kDashLiveMissingTimeShiftBuffer,
        model::StreamingIssueCode::kDashLiveSegmentCountUnknown,
        model::StreamingIssueCode::kDashDrmContentProtection,
        model::StreamingIssueCode::kDashContentProtectionMissingKeyId,
        model::StreamingIssueCode::kDashContentProtectionUnknownScheme,
        model::StreamingIssueCode::kDashLiveMissingUtcTiming,
        model::StreamingIssueCode::kDashUtcTimingInvalid,
        model::StreamingIssueCode::kDashEventStream,
        model::StreamingIssueCode::kDashEventStreamInvalid,
        model::StreamingIssueCode::kDashMissingRole,
        model::StreamingIssueCode::kDashEssentialProperty,
        model::StreamingIssueCode::kDashEssentialPropertyUnknown,
    };
    model::RemoveIssuesByCode(out.issues, kOwnCodes);

    const bool live = out.IsLive();

    for (size_t ri = 0; ri < out.representations.size(); ++ri) {
        // SegmentTimeline 的缺口/重叠比对是"每个表示扫一遍时间轴"，表示多时要能中断
        if (IsStreamingCancelled(cancel))
            return;
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
                cursor =
                    AddU64WillOverflow(start, entry_span) ? std::numeric_limits<uint64_t>::max() : start + entry_span;
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

    // ---- 4) 直播（dynamic MPD）语义：一条清单级别的检查 ----
    // 这几样都在 MPD 元素上，每个 Representation 报一遍只是刷屏，所以放在循环外。
    if (live) {
        // availabilityStartTime：dynamic MPD 的必填项
        if (!out.has_availability_start_time) {
            PushIssue(out, model::StreamingIssueCode::kDashLiveMissingAvailabilityStartTime,
                      "直播 MPD 缺少 availabilityStartTime",
                      "MPD 的 type=\"dynamic\" 表示这是一个直播包，但没有给出 availabilityStartTime，"
                      "客户端无法由分片起点算出\"这一片现在是否可用\"。",
                      "在 <MPD> 上补 availabilityStartTime=\"<UTC 时间>\"（ISO 8601，如 2026-01-01T00:00:00Z）；"
                      "规范对 dynamic MPD 强制要求该属性。",
                      model::IssueSeverity::Error, 0.0, 0.0, 1, -1.0, -1);
        }

        // minimumUpdatePeriod：重载间隔。缺失则客户端只能自己猜（常见做法是退回
        // minBufferTime 或直接用分片时长，各家实现不一，行为不可预期）。
        if (!out.has_minimum_update_period) {
            PushIssue(out, model::StreamingIssueCode::kDashLiveMissingMinimumUpdatePeriod,
                      "直播 MPD 缺少 minimumUpdatePeriod",
                      "直播 MPD 没有声明 minimumUpdatePeriod，客户端不知道该隔多久重载一次清单，"
                      "重载慢了会停在旧窗口、快了则白白打 CDN。",
                      "在 <MPD> 上补 minimumUpdatePeriod=\"PT2S\"（典型值等于或略小于一个分片时长）。",
                      model::IssueSeverity::Warning, 0.0, 0.0, 1, -1.0, -1);
        } else {
            double seg_seconds = 0.0;
            for (const model::DashRepresentationInfo& rep : out.representations) {
                seg_seconds = std::max(seg_seconds, rep.SegmentDurationSeconds());
            }
            if (seg_seconds > 0.0 && out.minimum_update_period_s > seg_seconds + 0.001) {
                PushIssue(
                    out, model::StreamingIssueCode::kDashLiveUpdatePeriodTooLong, "清单重载间隔过长",
                    "minimumUpdatePeriod=" + FormatSeconds(out.minimum_update_period_s) + " 秒，比一个分片的时长（" +
                        FormatSeconds(seg_seconds) +
                        " 秒）还长；客户端两次重载之间可能整段新分片都不可见，表现为直播起播慢或卡在旧画面。",
                    "把 minimumUpdatePeriod 调整到不大于分片时长；LL-DASH 场景通常与 SegmentTemplate@duration 等值。",
                    model::IssueSeverity::Warning, out.minimum_update_period_s, seg_seconds, 1, -1.0, -1);
            }
        }

        // timeShiftBufferDepth：既是服务端的历史保留窗口，也是快照能覆盖多长的唯一线索
        if (!out.has_time_shift_buffer_depth) {
            PushIssue(out, model::StreamingIssueCode::kDashLiveMissingTimeShiftBuffer,
                      "直播 MPD 未声明 timeShiftBufferDepth",
                      "MPD 没有 timeShiftBufferDepth，按规范意味着服务端必须一直保留全部历史分片"
                      "（可回退窗口无上界）；同时离线分析也失去了估计窗口长度的依据。",
                      "按业务需要的时移时长显式声明 timeShiftBufferDepth（如 PT30S），"
                      "既能限制服务端留存成本，也让这份快照的分片数可以被推算。",
                      model::IssueSeverity::Info, 0.0, 0.0, 1, -1.0, -1);
        }

        // 既没有可用时长线索又不是 SegmentTimeline → 分片数推不出来。
        // 以前这种情况是"静默产出 0 分片"，界面上一片空白，用户只会以为软件坏了。
        int unknown_reps = 0;
        for (const model::DashRepresentationInfo& rep : out.representations) {
            if (rep.segment_count_unknown)
                ++unknown_reps;
        }
        if (unknown_reps > 0) {
            PushIssue(out, model::StreamingIssueCode::kDashLiveSegmentCountUnknown, "直播包的分片数无法从快照推算",
                      std::to_string(unknown_reps) + "/" + std::to_string(out.representations.size()) +
                          " 个 Representation 使用 SegmentTemplate@duration 寻址，"
                          "而这份清单既没有 Period@duration / mediaPresentationDuration，也没有 timeShiftBufferDepth，"
                          "因此枚举不出具体有哪些分片。",
                      "这是离线单张快照的固有边界，不是清单缺陷："
                      "补齐 timeShiftBufferDepth 后即可按窗口枚举；也可改用 SegmentTimeline 显式列出每个分片的起点。",
                      model::IssueSeverity::Info, static_cast<double>(unknown_reps), 0.0, unknown_reps, -1.0, -1);
        }
    }

    // ---- 5) 加密（ContentProtection）----
    // 三种结论分开报：有 DRM 本身只是登记（Info），"声明了 CENC 却给不出 default_KID"
    // 与"scheme 完全认不出来"才是缺陷 —— 后者客户端拿不到密钥，整条流放不了。
    {
        int encrypted_reps = 0;
        int missing_kid = 0;
        std::string missing_kid_label;
        std::vector<std::string> systems;
        std::vector<std::string> unknown_schemes;
        for (size_t ri = 0; ri < out.representations.size(); ++ri) {
            if (IsStreamingCancelled(cancel))
                return;
            const model::DashRepresentationInfo& rep = out.representations[ri];
            if (!rep.has_content_protection)
                continue;
            ++encrypted_reps;
            bool has_cenc_declaration = false;
            bool has_kid = false;
            for (const model::DashContentProtectionInfo& cp : rep.content_protections) {
                if (cp.IsCencDeclaration())
                    has_cenc_declaration = true;
                if (!cp.default_kid.empty())
                    has_kid = true;
                const std::string name = DrmSystemName(cp.scheme_id_uri);
                if (name.empty()) {
                    if (std::find(unknown_schemes.begin(), unknown_schemes.end(), cp.scheme_id_uri) ==
                        unknown_schemes.end()) {
                        unknown_schemes.push_back(cp.scheme_id_uri);
                    }
                } else if (std::find(systems.begin(), systems.end(), name) == systems.end()) {
                    systems.push_back(name);
                }
            }
            // mp4protection 是"这是 CENC 加密"的通用声明，它必须带 default_KID；
            // 只写 Widevine/PlayReady 的 PSSH 反而不强制（KID 常在 pssh 里带）。
            if (has_cenc_declaration && !has_kid) {
                ++missing_kid;
                if (missing_kid_label.empty())
                    missing_kid_label = "Representation " + (rep.id.empty() ? std::to_string(ri) : rep.id);
            }
        }

        if (encrypted_reps > 0) {
            std::string list;
            for (size_t i = 0; i < systems.size(); ++i) {
                if (i)
                    list += "、";
                list += systems[i];
            }
            if (list.empty())
                list = "无法识别的方案";
            PushIssue(out, model::StreamingIssueCode::kDashDrmContentProtection, "DASH 流已加密",
                      std::to_string(encrypted_reps) + "/" + std::to_string(out.representations.size()) +
                          " 个 Representation 声明了 ContentProtection（" + list +
                          "）。加密本身不是缺陷，分片载荷不可读，码流类结论仅供参考。",
                      "确认密钥服务（License 服务器）在目标网络可达，"
                      "并核对 default_KID 与内容实际使用的密钥一致。",
                      model::IssueSeverity::Info, static_cast<double>(encrypted_reps), 0.0, encrypted_reps, -1.0, -1);
        }
        if (missing_kid > 0) {
            PushIssue(out, model::StreamingIssueCode::kDashContentProtectionMissingKeyId, "加密声明缺少 default_KID",
                      missing_kid_label +
                          " 声明了 mp4protection（CENC），但没有 cenc:default_KID；"
                          "共 " +
                          std::to_string(missing_kid) +
                          " 个表示受影响。"
                          "客户端无法在多个密钥里挑出该用哪一把。",
                      "在 <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\"> 上补 "
                      "cenc:default_KID=\"<16 字节 UUID>\"，与封装时 cenc 使用的 KID 保持一致。",
                      model::IssueSeverity::Warning, static_cast<double>(missing_kid), 0.0, missing_kid, -1.0, -1);
        }
        if (!unknown_schemes.empty()) {
            std::string list;
            for (size_t i = 0; i < unknown_schemes.size(); ++i) {
                if (i)
                    list += "、";
                list += "\"" + unknown_schemes[i] + "\"";
            }
            PushIssue(out, model::StreamingIssueCode::kDashContentProtectionUnknownScheme, "无法识别的加密方案",
                      "ContentProtection 的 schemeIdUri 不在已知 DRM 系统里：" + list + "。",
                      "确认 schemeIdUri 是否拼错（Widevine / PlayReady / ClearKey / FairPlay 都是 urn:uuid:...）；"
                      "自研 DRM 需要在播放器里注册对应的解密模块。",
                      model::IssueSeverity::Warning, static_cast<double>(unknown_schemes.size()), 0.0,
                      static_cast<int>(unknown_schemes.size()), -1.0, -1);
        }
    }

    // ---- 6) UTCTiming（直播对时）----
    // 直播分片的可用性由 availabilityStartTime + 分片起点推算，而"现在几点"只能靠
    // UTCTiming 从服务端拿。客户端信本地时钟的话，机顶盒时间不准就会整段缓冲。
    {
        int invalid = 0;
        std::string first_invalid;
        for (const model::DashUtcTimingInfo& t : out.utc_timings) {
            if (mt::Trim(t.scheme_id_uri).empty() || mt::Trim(t.value).empty()) {
                ++invalid;
                if (first_invalid.empty())
                    first_invalid = "schemeIdUri=\"" + t.scheme_id_uri + "\" value=\"" + t.value + "\"";
            }
        }
        if (live && out.utc_timings.empty()) {
            PushIssue(out, model::StreamingIssueCode::kDashLiveMissingUtcTiming, "直播 MPD 没有 UTCTiming",
                      "MPD 是 type=\"dynamic\"，但没有声明 UTCTiming；客户端只能相信本地时钟，"
                      "机顶盒/DVR 时间偏差会直接表现成起播慢、卡在旧画面或跳到未来分片。",
                      "在 <MPD> 下加 <UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:http-head:2014\" "
                      "value=\"https://example.com/time\"/>（或 direct / ntp 方案）。",
                      model::IssueSeverity::Warning, 0.0, 0.0, 1, -1.0, -1);
        }
        if (invalid > 0) {
            PushIssue(out, model::StreamingIssueCode::kDashUtcTimingInvalid, "UTCTiming 声明不完整",
                      std::to_string(invalid) + " 条 UTCTiming 缺少 schemeIdUri 或 value（首条 " + first_invalid +
                          "），客户端无从取时。",
                      "schemeIdUri 与 value 都是 UTCTiming 的必填属性：scheme 说明取时方式，"
                      "value 是时间串或取时 URL。",
                      model::IssueSeverity::Warning, static_cast<double>(invalid), 0.0, invalid, -1.0, -1);
        }
    }

    // ---- 7) EventStream（广告/章节，SCTE-35 over DASH 走这条）----
    if (!out.event_streams.empty()) {
        size_t event_total = 0;
        int invalid = 0;
        std::string first_invalid;
        std::vector<std::string> schemes;
        for (const model::DashEventStreamInfo& es : out.event_streams) {
            if (IsStreamingCancelled(cancel))
                return;
            event_total += es.events.size();
            if (!es.scheme_id_uri.empty() &&
                std::find(schemes.begin(), schemes.end(), es.scheme_id_uri) == schemes.end()) {
                schemes.push_back(es.scheme_id_uri);
            }
            if (es.timescale == 0) {
                ++invalid;
                if (first_invalid.empty())
                    first_invalid = "有一条 EventStream 的 timescale=0，事件时间无法换算成秒";
                continue;
            }
            for (const model::DashEventInfo& ev : es.events) {
                // presentationTime 在 <Event> 上是必填项，缺了就无法定位事件
                if (!ev.has_presentation_time) {
                    ++invalid;
                    if (first_invalid.empty())
                        first_invalid = "事件 id=\"" + ev.id + "\" 没有 presentationTime";
                    break;
                }
            }
        }
        std::string scheme_list;
        for (size_t i = 0; i < schemes.size(); ++i) {
            if (i)
                scheme_list += "、";
            scheme_list += schemes[i];
        }
        if (scheme_list.empty())
            scheme_list = "未声明 schemeIdUri";
        PushIssue(out, model::StreamingIssueCode::kDashEventStream, "含事件流（广告/章节标记）",
                  std::to_string(out.event_streams.size()) + " 条 EventStream、共 " + std::to_string(event_total) +
                      " 个事件（" + scheme_list + "）。",
                  "事件流是业务侧插播/章节的载体；确认播放器认识对应的 schemeIdUri，"
                  "否则这些标记会被静默忽略（画面上看不出问题）。",
                  model::IssueSeverity::Info, static_cast<double>(event_total), 0.0,
                  static_cast<int>(out.event_streams.size()), -1.0, -1);
        if (invalid > 0) {
            PushIssue(out, model::StreamingIssueCode::kDashEventStreamInvalid, "EventStream 声明不完整",
                      std::to_string(invalid) + " 处事件流/事件不自洽：" + first_invalid + "。",
                      "EventStream@timescale 必须是正整数，<Event> 必须带 presentationTime（单位同 timescale）。",
                      model::IssueSeverity::Warning, static_cast<double>(invalid), 0.0, invalid, -1.0, -1);
        }
    }

    // ---- 8) Role：多条同类型流里靠它区分主轨/备轨 ----
    // 一个 Period 里有两条 audio AdaptationSet 而都不写 Role 时，客户端只能按顺序取第一条；
    // 不同播放器取法不同，多语种/解说轨场景下会"打开就是错的音轨"。
    for (const model::DashPeriodInfo& period : out.periods) {
        if (IsStreamingCancelled(cancel))
            return;
        std::map<std::string, std::vector<const model::DashAdaptationSetInfo*>> by_type;
        for (const model::DashAdaptationSetInfo& as : period.adaptation_sets) {
            std::string type = mt::ToLower(as.content_type);
            if (type.empty() && !as.representation_indices.empty()) {
                const int ri = as.representation_indices.front();
                if (ri >= 0 && static_cast<size_t>(ri) < out.representations.size())
                    type = out.representations[static_cast<size_t>(ri)].content_type;
            }
            if (type.empty())
                continue;
            by_type[type].push_back(&as);
        }
        for (const auto& kv : by_type) {
            if (kv.second.size() < 2)
                continue;
            int without = 0;
            for (const model::DashAdaptationSetInfo* as : kv.second) {
                if (as->roles.empty())
                    ++without;
            }
            if (without == 0)
                continue;
            PushIssue(out, model::StreamingIssueCode::kDashMissingRole, "多条同类型流缺少 Role 标注",
                      "Period " + (period.id.empty() ? std::to_string(period.index) : period.id) + " 有 " +
                          std::to_string(kv.second.size()) + " 条 " + kv.first + " 流，其中 " +
                          std::to_string(without) + " 条没有 <Role>。客户端无法判定哪条是主轨（main）。",
                      "给主轨加 <Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"main\"/>，"
                      "备轨用 alternate / dub / description；字幕轨用 subtitle / caption。",
                      model::IssueSeverity::Info, static_cast<double>(without), 0.0, without, -1.0, -1);
        }
    }

    // ---- 9) EssentialProperty：客户端"必须理解"的扩展 ----
    {
        std::vector<std::string> schemes;
        int empty_scheme = 0;
        for (const model::DashRepresentationInfo& rep : out.representations) {
            for (const model::DashDescriptor& d : rep.essential_properties) {
                if (mt::Trim(d.scheme_id_uri).empty()) {
                    ++empty_scheme;
                    continue;
                }
                if (std::find(schemes.begin(), schemes.end(), d.scheme_id_uri) == schemes.end())
                    schemes.push_back(d.scheme_id_uri);
            }
        }
        if (empty_scheme > 0) {
            PushIssue(out, model::StreamingIssueCode::kDashEssentialPropertyUnknown,
                      "EssentialProperty 缺少 schemeIdUri",
                      std::to_string(empty_scheme) + " 条 EssentialProperty 没有 schemeIdUri。"
                                                     "按规范这是“必须理解”的扩展，没有标识就无法判断客户端能不能处理。",
                      "补上 schemeIdUri；若该扩展其实可忽略，改用 SupplementalProperty。",
                      model::IssueSeverity::Warning, static_cast<double>(empty_scheme), 0.0, empty_scheme, -1.0, -1);
        }
        if (!schemes.empty()) {
            std::string list;
            for (size_t i = 0; i < schemes.size(); ++i) {
                if (i)
                    list += "、";
                list += schemes[i];
            }
            PushIssue(out, model::StreamingIssueCode::kDashEssentialProperty, "声明了强制扩展属性",
                      "清单里有 " + std::to_string(schemes.size()) + " 种 EssentialProperty：" + list +
                          "。播放器若不认识这些 scheme，按规范应当拒绝播放该表示。",
                      "确认目标播放器支持这些扩展（如 CEA-608 闭包、SSR、tile 排布等）；"
                      "不支持时降级方案要考虑把该表示从 MPD 里去掉。",
                      model::IssueSeverity::Info, static_cast<double>(schemes.size()), 0.0,
                      static_cast<int>(schemes.size()), -1.0, -1);
        }
    }
}

} // namespace videoeye
