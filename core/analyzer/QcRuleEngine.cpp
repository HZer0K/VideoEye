#include "core/analyzer/QcRuleEngine.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <map>
#include <numeric>
#include <string>

namespace videoeye {
namespace analyzer {
namespace {

std::string FormatValue(double value, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

double MeanOf(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double StdDevOf(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    const double mean = MeanOf(values);
    double acc = 0.0;
    for (const double v : values) {
        const double d = v - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(values.size() - 1));
}

bool Triggered(const model::QcRule& rule, double value) {
    switch (rule.op) {
        case model::QcRuleOp::MaxExceeded: return value > rule.threshold;
        case model::QcRuleOp::MinBelow:    return value < rule.threshold;
        case model::QcRuleOp::NonZero:     return std::abs(value) > 1e-9;
    }
    return false;
}

std::string ThresholdText(const model::QcRule& rule) {
    if (rule.op == model::QcRuleOp::NonZero) return "存在";
    const std::string op_text = (rule.op == model::QcRuleOp::MaxExceeded) ? ">" : "<";
    return op_text + " " + FormatValue(rule.threshold, 2) + rule.unit;
}

// ---- container.extension_mismatch 辅助 ----

// 常见"扩展名 -> 容器名 token"别名（iformat->name 中不含该扩展名本身时的等价映射）
const std::map<std::string, std::string>& ExtensionAliasMap() {
    static const std::map<std::string, std::string> kAlias = {
        {"ts", "mpegts"}, {"m2ts", "mpegts"}, {"mts", "mpegts"}, {"tts", "mpegts"},
        {"mkv", "matroska"}, {"mka", "matroska"}, {"mk3d", "matroska"},
        {"webm", "matroska"},
        {"wmv", "asf"}, {"wma", "asf"},
        {"mpg", "mpeg"}, {"mpe", "mpeg"}, {"vob", "mpeg"}, {"m2p", "mpeg"},
        {"m4v", "mov"},
        {"3gpp", "3gp"},
        // 流媒体清单：container_format 由 AnalysisCoordinator 直接写 "hls" / "dash"
        {"m3u8", "hls"}, {"m3u", "hls"}, {"mpd", "dash"},
    };
    return kAlias;
}

// 容器名 token -> 可读名称（用于问题说明）
std::string FriendlyContainerName(const std::string& token) {
    static const std::map<std::string, std::string> kNames = {
        {"mov", "QuickTime/MOV"}, {"mp4", "MP4"}, {"m4a", "M4A"},
        {"3gp", "3GP"}, {"3g2", "3G2"}, {"mj2", "Motion JPEG 2000"},
        {"mpegts", "MPEG-TS"}, {"matroska", "Matroska/MKV"},
        {"avi", "AVI"}, {"flv", "Flash Video (FLV)"}, {"asf", "ASF/WMV"},
        {"ogg", "Ogg"}, {"mpeg", "MPEG-PS"}, {"wav", "WAV"}, {"mp3", "MP3"},
        {"aac", "AAC (ADTS)"}, {"flac", "FLAC"}, {"opus", "Opus (Ogg)"},
        {"rawvideo", "原始视频流"}, {"webm", "WebM"},
        {"hls", "HLS 清单"}, {"dash", "DASH 清单"},
    };
    auto it = kNames.find(token);
    return (it != kNames.end()) ? it->second : token;
}

// 判断扩展名是否与 FFmpeg 探测到的容器一致（container_format 为 iformat->name，
// 如 "mov,mp4,m4a,3gp,3g2,mj2" / "mpegts"）
bool ExtensionMatchesContainer(const std::string& ext, const std::string& container_format) {
    const auto& alias = ExtensionAliasMap();
    const auto alias_it = alias.find(ext);
    size_t start = 0;
    while (start <= container_format.size()) {
        const size_t comma = container_format.find(',', start);
        const size_t end = (comma == std::string::npos) ? container_format.size() : comma;
        std::string token = container_format.substr(start, end - start);
        // 去空格 + 小写
        std::string trimmed;
        for (const char c : token) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                trimmed += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        if (!trimmed.empty()) {
            if (trimmed == ext) return true;
            if (alias_it != alias.end() && trimmed == alias_it->second) return true;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

}  // namespace

QcRuleEngine::QcRuleEngine() : rules_(model::DefaultQcRules()) {}

QcRuleEngine::QcRuleEngine(std::vector<model::QcRule> rules) : rules_(std::move(rules)) {}

void QcRuleEngine::SetRules(std::vector<model::QcRule> rules) {
    rules_ = std::move(rules);
}

model::QcReport QcRuleEngine::Evaluate(const AnalysisResult& result) const {
    model::QcReport report;
    report.file_path = result.file_path;
    const size_t slash = result.file_path.find_last_of("/\\");
    report.file_name = (slash == std::string::npos) ? result.file_path
                                                    : result.file_path.substr(slash + 1);
    report.container_format = result.container_format;
    report.duration_seconds = result.duration_seconds;
    report.file_size_bytes = result.file_size_bytes;
    report.overall_bitrate_bps = result.overall_bitrate_bps;
    report.video_stream_count = result.VideoStreamCount();
    report.audio_stream_count = result.AudioStreamCount();
    report.completed = result.completed;
    report.rules = rules_;
    report.color_hdr = result.color_hdr;
    report.generated_at = model::CurrentTimestampString();

    for (const auto& rule : rules_) {
        if (!rule.enabled) continue;
        auto issues = CheckRule(rule, result);
        for (auto& issue : issues) {
            report.issues.push_back(std::move(issue));
        }
    }

    // 并入时间轴与同步诊断问题（category=Timing，统一计分与展示，避免与上面规则重复）
    for (const auto& tl_issue : result.timeline.issues) {
        report.issues.push_back(tl_issue);
    }

    // 严重度排序: Critical > Error > Warning > Info
    std::stable_sort(report.issues.begin(), report.issues.end(),
                     [](const model::DiagnosticIssue& a, const model::DiagnosticIssue& b) {
                         return static_cast<int>(a.severity) > static_cast<int>(b.severity);
                     });

    report.score = model::ComputeQcScore(report.issues);
    report.verdict = model::ComputeQcVerdict(report.score);
    return report;
}

std::vector<model::DiagnosticIssue> QcRuleEngine::CheckRule(const model::QcRule& rule,
                                                            const AnalysisResult& result) const {
    std::vector<model::DiagnosticIssue> issues;

    auto make_issue = [&rule](double value, const std::string& detail,
                              model::TimeRange range = model::TimeRange::Global(),
                              int stream_index = -1, int occurrences = 1) {
        model::DiagnosticIssue issue;
        issue.rule_id = rule.id;
        issue.title = rule.name;
        issue.detail = detail;
        issue.suggestion = rule.suggestion;
        issue.severity = rule.severity;
        issue.category = rule.category;
        issue.range = range;
        issue.stream_index = stream_index;
        issue.metric_value = value;
        issue.threshold = rule.threshold;
        issue.occurrence_count = occurrences;
        return issue;
    };

    // ---------- 容器 ----------
    if (rule.id == "container.duration_invalid") {
        if (Triggered(rule, result.duration_seconds)) {
            issues.push_back(make_issue(result.duration_seconds,
                "容器时长为 " + FormatValue(result.duration_seconds, 3) + " 秒（阈值 " +
                ThresholdText(rule) + "）。"));
        }
        return issues;
    }
    if (rule.id == "container.unseekable") {
        if (Triggered(rule, result.seekable ? 0.0 : 1.0)) {
            issues.push_back(make_issue(1.0, "底层 IO 不支持随机访问，只能线性播放。"));
        }
        return issues;
    }
    if (rule.id == "container.moov_after_mdat") {
        if (Triggered(rule, result.moov_after_mdat ? 1.0 : 0.0)) {
            issues.push_back(make_issue(1.0,
                "MP4 索引盒 moov 位于 mdat 之后，点播需下载完整个文件才能起播。"));
        }
        return issues;
    }
    if (rule.id == "container.missing_video") {
        if (result.VideoStreamCount() == 0) {
            issues.push_back(make_issue(1.0, "未检测到视频流。"));
        }
        return issues;
    }
    if (rule.id == "container.missing_audio") {
        if (result.AudioStreamCount() == 0) {
            issues.push_back(make_issue(1.0, "未检测到音频流。"));
        }
        return issues;
    }
    if (rule.id == "container.extension_mismatch") {
        // 仅本地文件参与比对；无扩展名、裸流（rawvideo 等）不判
        static const char* kRawExts[] = {"yuv", "nv12", "rgb", "bgr", "yuy2", "raw", "pcm"};
        const std::string& ext = result.file_extension;
        const std::string& fmt_name = result.container_format;
        bool is_raw_ext = false;
        for (const char* raw : kRawExts) {
            if (ext == raw) { is_raw_ext = true; break; }
        }
        if (!ext.empty() && !fmt_name.empty() && !is_raw_ext &&
            result.file_path.find("://") == std::string::npos) {
            if (!ExtensionMatchesContainer(ext, fmt_name)) {
                // 取第一个 token 作为实际容器的主要可读名
                const size_t comma = fmt_name.find(',');
                const std::string primary = (comma == std::string::npos)
                                                ? fmt_name
                                                : fmt_name.substr(0, comma);
                issues.push_back(make_issue(1.0,
                    "文件扩展名为 ." + ext + "，但按内容探测的实际容器为 " +
                    FriendlyContainerName(primary) + "（" + fmt_name + "）。"
                    "FFmpeg 等按内容探测的工具可正常打开，但按扩展名选择解析器的"
                    "播放器/剪辑工具/上传平台可能打开失败或识别错误。"));
            }
        }
        return issues;
    }

    // ---------- MP4/fMP4 容器一致性 ----------
    // 问题由 Mp4SampleTableAnalyzer 产出（含具体样本号 / 偏移 / 分片号），
    // 规则只做三件事: 决定要不要报、按什么级别报、阈值类再核一次阈值。
    if (rule.id.rfind("container.mp4.", 0) == 0) {
        if (!result.mp4_samples_analyzed || !result.mp4_samples.valid) return issues;

        for (const auto& finding : result.mp4_samples.issues) {
            if (finding.code != rule.id) continue;
            // 阈值类规则（elst 首帧偏移 / 音视频起点差）：分析器用的是自己的默认阈值，
            // 这里按用户在规则表里改过的阈值再核一次
            if (rule.op == model::QcRuleOp::MaxExceeded && rule.threshold > 0.0 &&
                finding.metric_value > 0.0 && finding.metric_value <= rule.threshold) {
                continue;
            }
            if (rule.op == model::QcRuleOp::MinBelow && rule.threshold != 0.0 &&
                finding.metric_value != 0.0 && finding.metric_value >= rule.threshold) {
                continue;
            }

            // 尽量把问题定位到时间点（样本级发现才有意义）
            double seconds = -1.0;
            if (finding.has_sample_index && finding.track_id >= 0) {
                const model::Mp4TrackSampleTable* track =
                    result.mp4_samples.FindTrack(static_cast<uint32_t>(finding.track_id));
                const model::Mp4Sample* sample =
                    track ? track->FindSample(finding.sample_index) : nullptr;
                if (sample && track->media_timescale > 0) {
                    seconds = sample->CtsSeconds(track->media_timescale);
                }
            }

            model::DiagnosticIssue issue = finding.ToDiagnosticIssue(seconds);
            issue.rule_id = rule.id;
            issue.title = rule.name;
            issue.category = rule.category;
            issue.suggestion = rule.suggestion.empty() ? finding.suggestion : rule.suggestion;
            issue.threshold = rule.threshold;
            // 级别取「规则」与「分析器」中更严重的一侧：用户在规则表调到 Error 可以抬高，
            // 调到 Info 也不会把 Error 级发现（如分片序号回退）降没。
            if (static_cast<int>(finding.severity) > static_cast<int>(rule.severity)) {
                issue.severity = finding.severity;
            } else {
                issue.severity = rule.severity;
            }
            issues.push_back(std::move(issue));
        }
        return issues;
    }

    // ---------- HLS / DASH 流媒体包 ----------
    // 与 container.mp4.* 完全同构：问题（含 variant 号 / 分片序号 / 时间点）由
    // HlsManifestAnalyzer / DashManifestAnalyzer / SegmentQcAnalyzer 产出，
    // 规则只决定"要不要报、以什么级别报、阈值类再核一次"。
    if (rule.id.rfind("container.hls.", 0) == 0 || rule.id.rfind("container.dash.", 0) == 0 ||
        rule.id.rfind("container.streaming.", 0) == 0) {
        if (!result.streaming_analyzed || !result.streaming_package.valid) return issues;

        for (const auto& finding : result.streaming_package.issues) {
            if (finding.code != rule.id) continue;
            if (rule.op == model::QcRuleOp::MaxExceeded && rule.threshold > 0.0 &&
                finding.metric_value > 0.0 && finding.metric_value <= rule.threshold) {
                continue;
            }
            if (rule.op == model::QcRuleOp::MinBelow && rule.threshold != 0.0 &&
                finding.metric_value != 0.0 && finding.metric_value >= rule.threshold) {
                continue;
            }

            model::DiagnosticIssue issue = finding.ToDiagnosticIssue();
            issue.rule_id = rule.id;
            issue.title = rule.name;
            issue.category = rule.category;
            issue.suggestion = rule.suggestion.empty() ? finding.suggestion : rule.suggestion;
            issue.threshold = rule.threshold;
            // 级别取「规则」与「分析器」中更严重的一侧
            if (static_cast<int>(finding.severity) > static_cast<int>(rule.severity)) {
                issue.severity = finding.severity;
            } else {
                issue.severity = rule.severity;
            }
            issues.push_back(std::move(issue));
        }
        return issues;
    }

    // ---------- 码率 ----------
    if (rule.id == "video.bitrate.peak_ratio") {
        const auto& series = result.video_bitrate_kbps.IsEmpty() ? result.total_bitrate_kbps
                                                                 : result.video_bitrate_kbps;
        if (series.Size() >= 2) {
            const double ratio = series.PeakToMeanRatio();
            if (Triggered(rule, ratio)) {
                issues.push_back(make_issue(ratio,
                    "峰值/均值码率比为 " + FormatValue(ratio) + "（峰值 " +
                    FormatValue(series.Max(), 0) + " kbps，均值 " +
                    FormatValue(series.Mean(), 0) + " kbps，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }
    if (rule.id == "video.bitrate.low_bpp") {
        const StreamDigest* video = result.FirstVideoStream();
        if (video && video->width > 0 && video->height > 0) {
            double fps = video->avg_fps;
            if (fps <= 0.0) fps = result.video_fps.Mean();
            if (fps > 0.0) {
                const double bpp = static_cast<double>(video->bitrate_bps) /
                                   (static_cast<double>(video->width) *
                                    static_cast<double>(video->height) * fps);
                if (Triggered(rule, bpp)) {
                    issues.push_back(make_issue(bpp,
                        "每像素每帧比特数为 " + FormatValue(bpp, 4) + "（码率 " +
                        FormatValue(video->bitrate_bps / 1000.0, 0) + " kbps，" +
                        std::to_string(video->width) + "x" + std::to_string(video->height) +
                        "@" + FormatValue(fps, 2) + "fps，阈值 " + ThresholdText(rule) + "）。",
                        model::TimeRange::Global(), video->index));
                }
            }
        }
        return issues;
    }

    // ---------- GOP ----------
    if (rule.id == "video.gop.max_seconds") {
        if (result.max_gop_interval_seconds > 0.0 &&
            Triggered(rule, result.max_gop_interval_seconds)) {
            issues.push_back(make_issue(result.max_gop_interval_seconds,
                "最长关键帧间隔 " + FormatValue(result.max_gop_interval_seconds, 2) + " 秒（" +
                std::to_string(result.max_gop_frames) + " 帧，阈值 " + ThresholdText(rule) + "）。"));
        }
        return issues;
    }
    if (rule.id == "video.gop.irregular") {
        if (result.gop_intervals_seconds.size() >= 3) {
            const double mean = MeanOf(result.gop_intervals_seconds);
            const double stddev = StdDevOf(result.gop_intervals_seconds);
            const double ratio = (mean > 0.0) ? (stddev / mean) : 0.0;
            if (Triggered(rule, ratio)) {
                issues.push_back(make_issue(ratio,
                    "关键帧间隔标准差/均值为 " + FormatValue(ratio) + "（均值 " +
                    FormatValue(mean, 2) + " 秒，标准差 " + FormatValue(stddev, 2) +
                    " 秒，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }

    // ---------- 码率与 GOP 深度分析（BitrateGopAnalyzer）----------
    // 这些规则逐条展开 anomalies，让诊断表能直接给出时间区间并支持"跳转"。
    if (rule.id == "video.gop.long_count" ||
        rule.id == "video.gop.scene_without_keyframe" ||
        rule.id == "video.gop.sparse_keyframes" ||
        rule.id == "video.bitrate.peak_overshoot" ||
        rule.id == "video.frame.oversized" ||
        rule.id == "video.frame.oversized_i") {
        using AnomalyType = BitrateAnomalyType;
        const AnomalyType type =
            (rule.id == "video.gop.long_count")               ? AnomalyType::LongGop
            : (rule.id == "video.gop.scene_without_keyframe") ? AnomalyType::SceneChangeWithoutKeyframe
            : (rule.id == "video.gop.sparse_keyframes")       ? AnomalyType::SparseKeyframes
            : (rule.id == "video.bitrate.peak_overshoot")     ? AnomalyType::PeakOvershoot
            : (rule.id == "video.frame.oversized")            ? AnomalyType::OversizedFrame
                                                              : AnomalyType::OversizedIFrame;

        auto anomalies = result.bitrate_gop.AnomaliesOf(type);
        if (!anomalies.empty()) {
            // 单条规则最多展开 50 条，避免长视频把诊断表刷爆
            constexpr size_t kMaxExpanded = 50;
            const size_t count = std::min(anomalies.size(), kMaxExpanded);
            for (size_t i = 0; i < count; ++i) {
                const BitrateAnomaly& a = *anomalies[i];
                issues.push_back(make_issue(a.value, a.detail,
                                            model::TimeRange::Between(a.start_seconds,
                                                                      a.end_seconds),
                                            -1, 1));
            }
            if (anomalies.size() > count) {
                issues.push_back(make_issue(static_cast<double>(anomalies.size()),
                    "另有 " + std::to_string(anomalies.size() - count) + " 条「" + rule.name +
                    "」未展开，完整列表见「码率与 GOP」页。"));
            }
        }
        return issues;
    }

    // ---------- 视频 ----------
    if (rule.id == "video.fps.unstable") {
        if (result.video_fps.Size() >= 3) {
            const double stddev = result.video_fps.StdDev();
            if (Triggered(rule, stddev)) {
                issues.push_back(make_issue(stddev,
                    "逐秒帧率标准差为 " + FormatValue(stddev, 2) + " fps（均值 " +
                    FormatValue(result.video_fps.Mean(), 2) + " fps，最低 " +
                    FormatValue(result.video_fps.Min(), 2) + " fps，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }
    if (rule.id == "video.resolution.odd") {
        const StreamDigest* video = result.FirstVideoStream();
        if (video && (video->width % 2 != 0 || video->height % 2 != 0)) {
            issues.push_back(make_issue(1.0,
                "分辨率为 " + std::to_string(video->width) + "x" + std::to_string(video->height) +
                "，存在奇数边。",
                model::TimeRange::Global(), video->index));
        }
        return issues;
    }

    // ---------- 时间戳（PTS 非单调 / 时间戳跳变已由 TimelineAnalyzer 统一产出，并入诊断报告） ----------
    if (rule.id == "timing.dts_missing") {
        if (result.total_packets > 0) {
            const double ratio = 100.0 * static_cast<double>(result.packets_missing_dts) /
                                 static_cast<double>(result.total_packets);
            if (Triggered(rule, ratio)) {
                issues.push_back(make_issue(ratio,
                    "缺失 DTS 的包占比 " + FormatValue(ratio, 2) + "%（" +
                    std::to_string(result.packets_missing_dts) + "/" +
                    std::to_string(result.total_packets) + "，阈值 " + ThresholdText(rule) + "）。"));
            }
        }
        return issues;
    }
    // 时间戳跳变（gap）已由 TimelineAnalyzer 统一产出，不再重复规则。

    // ---------- 音频 ----------
    if (rule.id == "audio.sample_rate_low") {
        int min_rate = 0;
        int stream_index = -1;
        for (const auto& stream : result.streams) {
            if (!stream.IsAudio()) continue;
            if (min_rate == 0 || stream.sample_rate < min_rate) {
                min_rate = stream.sample_rate;
                stream_index = stream.index;
            }
        }
        if (min_rate > 0 && Triggered(rule, static_cast<double>(min_rate))) {
            issues.push_back(make_issue(static_cast<double>(min_rate),
                "最低音频采样率为 " + std::to_string(min_rate) + " Hz（阈值 " +
                ThresholdText(rule) + "）。",
                model::TimeRange::Global(), stream_index));
        }
        return issues;
    }
    // ---------- 音频 QC（core/analyzer/AudioQcAnalyzer，需解码音频）----------
    // 未跑过音频 QC（analyzed=false）时一律不判定，避免因为"没有数据"而误报。
    if (rule.id == "audio.loudness.target_high" || rule.id == "audio.loudness.target_low" ||
        rule.id == "audio.loudness.range" || rule.id == "audio.true_peak" ||
        rule.id == "audio.clipping" || rule.id == "audio.silence.longest" ||
        rule.id == "audio.silence.ratio" || rule.id == "audio.dc_offset" ||
        rule.id == "audio.phase_correlation" || rule.id == "audio.metadata.layout" ||
        rule.id == "audio.metadata.duration_mismatch") {
        const auto& qc = result.audio_qc;
        if (!qc.analyzed) return issues;

        const StreamDigest* audio = result.FirstAudioStream();
        const int audio_stream = (audio != nullptr) ? audio->index : -1;
        const bool has_loudness = qc.integrated_lufs > model::kSilenceLufs + 1.0;

        // 在响度曲线里定位指标最值出现的时刻（供"跳转到问题位置"）
        auto extreme_time = [&qc](auto getter, bool maximum) {
            double best = maximum ? -1e30 : 1e30;
            double ts = 0.0;
            for (const auto& p : qc.loudness_points) {
                const double v = getter(p);
                if ((maximum && v > best) || (!maximum && v < best)) {
                    best = v;
                    ts = p.timestamp_seconds;
                }
            }
            return ts;
        };

        if (rule.id == "audio.loudness.target_high") {
            if (has_loudness && Triggered(rule, qc.integrated_lufs)) {
                issues.push_back(make_issue(qc.integrated_lufs,
                    "积分响度为 " + FormatValue(qc.integrated_lufs, 2) + " LUFS，高于上限 " +
                    ThresholdText(rule) + "（短期最大 " + FormatValue(qc.short_term_max_lufs, 2) +
                    " LUFS，瞬时最大 " + FormatValue(qc.momentary_max_lufs, 2) + " LUFS）。",
                    model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.loudness.target_low") {
            if (has_loudness && Triggered(rule, qc.integrated_lufs)) {
                issues.push_back(make_issue(qc.integrated_lufs,
                    "积分响度为 " + FormatValue(qc.integrated_lufs, 2) + " LUFS，低于下限 " +
                    ThresholdText(rule) + "（短期最大 " + FormatValue(qc.short_term_max_lufs, 2) +
                    " LUFS）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.loudness.range") {
            if (qc.loudness_range_lu > 0.0 && Triggered(rule, qc.loudness_range_lu)) {
                issues.push_back(make_issue(qc.loudness_range_lu,
                    "响度动态范围 LRA 为 " + FormatValue(qc.loudness_range_lu, 1) + " LU（P10 " +
                    FormatValue(qc.loudness_range_low_lufs, 1) + " LUFS，P95 " +
                    FormatValue(qc.loudness_range_high_lufs, 1) + " LUFS，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.true_peak") {
            if (Triggered(rule, qc.true_peak_dbtp)) {
                const double ts = extreme_time(
                    [](const model::LoudnessPoint& p) { return p.true_peak_dbtp; }, true);
                issues.push_back(make_issue(qc.true_peak_dbtp,
                    "4× 过采样真峰值为 " + FormatValue(qc.true_peak_dbtp, 2) + " dBTP（采样峰值 " +
                    FormatValue(qc.sample_peak_dbfs, 2) + " dBFS，阈值 " + ThresholdText(rule) +
                    "）。", model::TimeRange::At(ts), audio_stream));
            }
        } else if (rule.id == "audio.clipping") {
            if (qc.clipping_sample_count > 0) {
                const double ts = qc.clipping_events.empty()
                                      ? 0.0
                                      : qc.clipping_events.front().start_seconds;
                issues.push_back(make_issue(static_cast<double>(qc.clipping_sample_count),
                    std::to_string(qc.clipping_sample_count) + " 个样本达到满刻度（" +
                    std::to_string(qc.clipping_event_count) + " 段，阈值 " +
                    ThresholdText(rule) + "），首次出现于 " + FormatValue(ts, 2) + " 秒。",
                    model::TimeRange::At(ts), audio_stream,
                    static_cast<int>(qc.clipping_event_count)));
            }
        } else if (rule.id == "audio.silence.longest") {
            double longest = 0.0;
            model::TimeRange longest_range = model::TimeRange::Global();
            for (const auto& range : qc.silence_ranges) {
                if (range.duration_seconds > longest) {
                    longest = range.duration_seconds;
                    longest_range = model::TimeRange::Between(range.start_seconds, range.end_seconds);
                }
            }
            if (longest > 0.0 && Triggered(rule, longest)) {
                issues.push_back(make_issue(longest,
                    "最长静音段 " + FormatValue(longest, 2) + " 秒（共 " +
                    std::to_string(qc.silence_ranges.size()) + " 段，阈值 " +
                    ThresholdText(rule) + "）。", longest_range, audio_stream,
                    static_cast<int>(qc.silence_ranges.size())));
            }
        } else if (rule.id == "audio.silence.ratio") {
            const double percent = qc.silence_ratio * 100.0;
            if (Triggered(rule, percent)) {
                issues.push_back(make_issue(percent,
                    "静音时长占比 " + FormatValue(percent, 1) + "%（" +
                    std::to_string(qc.silence_ranges.size()) + " 段，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.dc_offset") {
            if (Triggered(rule, qc.max_dc_offset)) {
                issues.push_back(make_issue(qc.max_dc_offset,
                    "最大直流偏移为 " + FormatValue(qc.max_dc_offset, 5) + "（" +
                    FormatValue(qc.max_dc_offset_dbfs, 1) + " dBFS，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.phase_correlation") {
            if (qc.correlation_available && Triggered(rule, qc.correlation_min)) {
                const double ts = extreme_time(
                    [](const model::LoudnessPoint& p) { return p.correlation; }, false);
                issues.push_back(make_issue(qc.correlation_min,
                    "最差声道相关性为 " + FormatValue(qc.correlation_min, 3) + "（反相块占比 " +
                    FormatValue(qc.out_of_phase_ratio * 100.0, 1) + "%，阈值 " +
                    ThresholdText(rule) + "）。", model::TimeRange::At(ts), audio_stream));
            }
        } else if (rule.id == "audio.metadata.layout") {
            if (!qc.metadata.layout_confirmed && qc.metadata.channels > 2) {
                issues.push_back(make_issue(static_cast<double>(qc.metadata.channels),
                    "声道布局为 " + qc.metadata.channel_layout +
                    "，但容器未明确标注声道位置，响度加权按通用顺序估算。",
                    model::TimeRange::Global(), audio_stream));
            }
        } else if (rule.id == "audio.metadata.duration_mismatch") {
            const bool container_mismatch =
                qc.metadata.container_duration_seconds > 0.0 &&
                std::abs(qc.metadata.container_delta_seconds) > 0.5;
            const bool video_mismatch =
                qc.metadata.has_video && qc.metadata.video_duration_seconds > 0.0 &&
                std::abs(qc.metadata.video_delta_seconds) > 0.5;
            if (container_mismatch || video_mismatch) {
                std::string detail;
                for (const auto& text : qc.metadata.inconsistencies) {
                    if (text.find("时长") == std::string::npos) continue;
                    if (!detail.empty()) detail += "；";
                    detail += text;
                }
                if (detail.empty()) detail = "音频流与容器/视频时长不一致。";
                issues.push_back(make_issue(
                    std::max(std::abs(qc.metadata.container_delta_seconds),
                             std::abs(qc.metadata.video_delta_seconds)),
                    detail + "音频 " + FormatValue(qc.metadata.stream_duration_seconds, 2) +
                        " 秒。", model::TimeRange::Global(), audio_stream));
            }
        }
        return issues;
    }

    // ---------- 色彩与 HDR ----------
    if (rule.id == "video.color.hdr_missing_mastering" ||
        rule.id == "video.color.hdr_missing_light_level" ||
        rule.id == "video.color.hdr_low_bitdepth" ||
        rule.id == "video.color.wide_gamut_sdr_transfer" ||
        rule.id == "video.color.matrix_mismatch" ||
        rule.id == "video.color.range_conflict" ||
        rule.id == "video.color.unspecified" ||
        rule.id == "video.color.dv_no_compatibility") {
        const analyzer::ColorHdrAnalysis& analysis = result.color_hdr;
        if (!analysis.analyzed) return issues;

        const model::ColorInfo& color = analysis.color;
        const model::HdrMetadataInfo& hdr = analysis.hdr;
        const int stream = color.stream_index;

        if (rule.id == "video.color.hdr_missing_mastering") {
            // HLG 不依赖静态元数据（广播电视场景本就没有），只对 PQ 要求 SMPTE ST 2086
            if (color.IsPqTransfer() && !hdr.mastering_display.Complete() &&
                !hdr.dolby_vision.present) {
                std::string missing = "母版显示信息";
                missing += hdr.mastering_display.present ? "不完整" : "缺失";
                issues.push_back(make_issue(1.0,
                    "传递函数为 PQ，但 SMPTE ST 2086 " + missing +
                    "，播放器只能按默认色彩体量做色调映射。",
                    model::TimeRange::Global(), stream));
            }
        } else if (rule.id == "video.color.hdr_missing_light_level") {
            if (color.IsPqTransfer() && !hdr.content_light.Complete() &&
                !hdr.dolby_vision.present) {
                issues.push_back(make_issue(1.0,
                    "传递函数为 PQ，但 MaxCLL/MaxFALL " +
                    (hdr.content_light.present ? std::string("不完整（只有其中一项）")
                                               : std::string("缺失")) +
                    "，接收端无法预判内容亮度。",
                    model::TimeRange::Global(), stream));
            }
        } else if (rule.id == "video.color.hdr_low_bitdepth") {
            const int depth = color.EffectiveBitDepth();
            if (color.IsHdrTransfer() && depth > 0 && depth < 10) {
                issues.push_back(make_issue(static_cast<double>(depth),
                    "传递函数为 " + color.transfer_name + "，但像素格式 " +
                    (color.pixel_format.name.empty() ? std::string("未知")
                                                     : color.pixel_format.name) +
                    " 只有 " + std::to_string(depth) + " bit，HDR 需要 ≥ 10 bit。",
                    model::TimeRange::Global(), stream));
            }
        } else if (rule.id == "video.color.wide_gamut_sdr_transfer") {
            if (color.IsWideGamutPrimaries() && color.transfer == model::TransferKind::Sdr) {
                issues.push_back(make_issue(1.0,
                    "色彩原色为 " + color.primaries_name + "，但传递函数为 " +
                    color.transfer_name +
                    "：广色域 + SDR gamma 的组合容易被播放端误判，"
                    "要么补 HDR 元数据并按 HDR 分发，要么回到 BT.709 SDR。",
                    model::TimeRange::Global(), stream));
            }
        } else if (rule.id == "video.color.matrix_mismatch") {
            // 同代标准组合才是常态：709+709 / 2020+2020NCL / SDR 601+601
            bool mismatch = false;
            std::string detail;
            if (color.primaries == model::ColorPrimariesKind::Bt2020 &&
                color.matrix == model::MatrixKind::Bt709) {
                mismatch = true;
                detail = "BT.2020 原色配 BT.709 矩阵（应为 BT.2020 NCL）";
            } else if (color.primaries == model::ColorPrimariesKind::Bt709 &&
                       (color.matrix == model::MatrixKind::Bt2020Ncl ||
                        color.matrix == model::MatrixKind::Bt2020Cl)) {
                mismatch = true;
                detail = "BT.709 原色配 BT.2020 矩阵（应为 BT.709）";
            } else if (color.primaries == model::ColorPrimariesKind::Bt709 &&
                       color.matrix == model::MatrixKind::Bt601) {
                mismatch = true;
                detail = "BT.709 原色配 BT.601 矩阵（常见于 SD/HD 转换漏标）";
            } else if (color.primaries == model::ColorPrimariesKind::Bt601 &&
                       color.matrix == model::MatrixKind::Bt709) {
                mismatch = true;
                detail = "BT.601 原色配 BT.709 矩阵（SD 素材按 HD 矩阵还原会偏色）";
            }
            if (mismatch) {
                issues.push_back(make_issue(1.0,
                    detail + "：YUV→RGB 按错误矩阵还原，画面整体偏色。",
                    model::TimeRange::Global(), stream));
            }
        } else if (rule.id == "video.color.range_conflict") {
            std::string detail;
            if (color.pixel_format.ImpliesFullRange() &&
                color.range == model::ColorRangeKind::Limited) {
                detail = "像素格式 " + color.pixel_format.name +
                         " 隐含 full range，色彩范围却被标注为 Limited";
            } else if (color.pixel_format.rgb && color.range == model::ColorRangeKind::Limited) {
                detail = "RGB 数据不存在 limited range，色彩范围却被标注为 Limited";
            } else if (color.pixel_format.rgb && !color.pixel_format.chroma_subsampling.empty()) {
                detail = "RGB 像素格式却标注了色度下采样 " + color.pixel_format.chroma_subsampling;
            }
            if (!detail.empty()) {
                issues.push_back(make_issue(1.0, detail + "：灰阶会被整体抬高或压低。",
                                            model::TimeRange::Global(), stream));
            }
        } else if (rule.id == "video.color.unspecified") {
            std::vector<std::string> missing_items;
            if (!color.PrimariesSpecified()) missing_items.push_back("primaries");
            if (!color.TransferSpecified()) missing_items.push_back("transfer");
            if (!color.MatrixSpecified()) missing_items.push_back("matrix");
            if (!color.RangeSpecified()) missing_items.push_back("range");
            if (!missing_items.empty()) {
                std::string list;
                for (size_t i = 0; i < missing_items.size(); ++i) {
                    if (i > 0) list += " / ";
                    list += missing_items[i];
                }
                issues.push_back(make_issue(static_cast<double>(missing_items.size()),
                    "未标注的色彩项: " + list + "。缺失时播放器按默认值猜测，"
                    "广色域/HDR 内容几乎必然颜色错误。",
                    model::TimeRange::Global(), stream,
                    static_cast<int>(missing_items.size())));
            }
        } else if (rule.id == "video.color.dv_no_compatibility") {
            if (hdr.dolby_vision.present && hdr.dolby_vision.compatibility_id == 0) {
                issues.push_back(make_issue(1.0,
                    "Dolby Vision profile " + std::to_string(hdr.dolby_vision.profile) +
                    " 的 bl_signal_compatibility_id 为 0（" + hdr.dolby_vision.CompatibilityText() +
                    "），非 DV 设备回放颜色会明显错误。",
                    model::TimeRange::Global(), stream));
            }
        }
        return issues;
    }

    // ---------- 字幕（core/analyzer/SubtitleAnalyzer.h）----------
    // 具体阈值（最短/最长停留、阅读速度）由 SubtitleOptions 决定，规则只决定
    // "这类问题要不要进报告、按什么级别进"，所以全部用计数型判定。
    if (rule.id.rfind("subtitle.", 0) == 0) {
        static const std::pair<const char*, model::SubtitleIssueType> kMap[] = {
            {"subtitle.cue_empty", model::SubtitleIssueType::EmptyText},
            {"subtitle.cue_overlap", model::SubtitleIssueType::Overlap},
            {"subtitle.cue_too_short", model::SubtitleIssueType::TooShort},
            {"subtitle.cue_too_long", model::SubtitleIssueType::TooLong},
            {"subtitle.cue_order", model::SubtitleIssueType::NonMonotonic},
            {"subtitle.cue_invalid_duration", model::SubtitleIssueType::InvalidDuration},
            {"subtitle.cue_too_fast", model::SubtitleIssueType::TooFast},
            {"subtitle.cue_out_of_range", model::SubtitleIssueType::OutOfRange},
            {"subtitle.missing_language", model::SubtitleIssueType::MissingLanguage},
            {"subtitle.missing_handler", model::SubtitleIssueType::MissingHandler},
        };
        bool matched = false;
        model::SubtitleIssueType target = model::SubtitleIssueType::EmptyText;
        for (const auto& entry : kMap) {
            if (rule.id == entry.first) {
                matched = true;
                target = entry.second;
                break;
            }
        }
        if (!matched || !result.subtitle_analyzed) return issues;

        const int count = result.subtitle.CountIssues(target);
        if (Triggered(rule, static_cast<double>(count))) {
            const model::SubtitleIssue* first = nullptr;
            for (const model::SubtitleIssue& issue : result.subtitle.issues) {
                if (issue.type == target) {
                    first = &issue;
                    break;
                }
            }
            std::string detail = "共 " + std::to_string(count) + " 处";
            if (first != nullptr && !first->detail.empty()) detail += "，例如：" + first->detail;
            model::TimeRange range = model::TimeRange::Global();
            int stream_index = -1;
            if (first != nullptr && !first->IsStreamLevel() && first->start_seconds >= 0.0) {
                range = model::TimeRange::At(first->start_seconds);
                stream_index = first->stream_index;
            } else if (first != nullptr) {
                stream_index = first->stream_index;
            }
            issues.push_back(make_issue(static_cast<double>(count), detail, range, stream_index,
                                        count > 0 ? count : 1));
        }
        return issues;
    }

    // ---------- 时码与章节（core/analyzer/TimecodeAnalyzer.h）----------
    if (rule.id == "timecode.missing") {
        if (!result.timecode_analyzed) return issues;
        if (Triggered(rule, result.timecode.has_primary ? 0.0 : 1.0)) {
            issues.push_back(make_issue(1.0,
                "未找到时码：既没有 MOV/MP4 的 tmcd 时码轨，也没有 metadata 里的 timecode tag。"));
        }
        return issues;
    }
    if (rule.id == "timecode.drop_frame_mismatch") {
        if (!result.timecode_analyzed || !result.timecode.has_primary) return issues;
        const double fps = result.timecode.primary_frame_rate;
        if (fps <= 0.0) return issues;
        const bool rate_is_ntsc = model::IsDropFrameRate(fps);
        const bool mismatch = (rate_is_ntsc && !result.timecode.primary_drop_frame) ||
                              (!rate_is_ntsc && result.timecode.primary_drop_frame);
        if (Triggered(rule, mismatch ? 1.0 : 0.0)) {
            issues.push_back(make_issue(1.0,
                "帧率 " + FormatValue(fps, 3) + " fps 与时码标记 " +
                (result.timecode.primary_drop_frame ? "drop-frame" : "non-drop-frame") +
                " 不一致；29.97 素材用 non-drop 时码，一小时会累积约 3.6 秒偏差。",
                model::TimeRange::At(0.0)));
        }
        return issues;
    }
    if (rule.id == "timecode.invalid_frame") {
        if (!result.timecode_analyzed || !result.timecode.has_primary) return issues;
        const double fps = result.timecode.primary_frame_rate;
        if (fps <= 0.0) return issues;
        if (Triggered(rule, model::IsValidTimecode(result.timecode.primary, fps) ? 0.0 : 1.0)) {
            issues.push_back(make_issue(1.0,
                "首帧时码 " + result.timecode.primary.ToString() + " 在 " +
                FormatValue(fps, 3) + " fps 下不合法（帧号越界或 drop-frame 跳帧位置错误）。",
                model::TimeRange::At(0.0)));
        }
        return issues;
    }
    if (rule.id.rfind("chapter.", 0) == 0) {
        static const std::pair<const char*, model::ChapterIssueType> kMap[] = {
            {"chapter.overlap", model::ChapterIssueType::Overlap},
            {"chapter.out_of_range", model::ChapterIssueType::OutOfRange},
            {"chapter.non_monotonic", model::ChapterIssueType::NonMonotonic},
            {"chapter.zero_duration", model::ChapterIssueType::ZeroDuration},
            {"chapter.missing_title", model::ChapterIssueType::MissingTitle},
        };
        bool matched = false;
        model::ChapterIssueType target = model::ChapterIssueType::Overlap;
        for (const auto& entry : kMap) {
            if (rule.id == entry.first) {
                matched = true;
                target = entry.second;
                break;
            }
        }
        if (!matched || !result.timecode_analyzed) return issues;

        const int count = result.timecode.CountChapterIssues(target);
        if (Triggered(rule, static_cast<double>(count))) {
            const model::ChapterIssue* first = nullptr;
            for (const model::ChapterIssue& issue : result.timecode.chapter_issues) {
                if (issue.type == target) {
                    first = &issue;
                    break;
                }
            }
            std::string detail = "共 " + std::to_string(count) + " 处";
            if (first != nullptr && !first->detail.empty()) detail += "，例如：" + first->detail;
            model::TimeRange range = model::TimeRange::Global();
            if (first != nullptr && first->start_seconds >= 0.0) {
                range = model::TimeRange::At(first->start_seconds);
            }
            issues.push_back(make_issue(static_cast<double>(count), detail, range, -1,
                                        count > 0 ? count : 1));
        }
        return issues;
    }

    // ---------- SCTE-35（core/analyzer/Scte35Analyzer.h）----------
    if (rule.id == "scte35.parse_error") {
        if (!result.aux_data_analyzed) return issues;
        const int count = result.aux_data.parse_error_count;
        if (Triggered(rule, static_cast<double>(count))) {
            issues.push_back(make_issue(static_cast<double>(count),
                "有 " + std::to_string(count) +
                " 个 SCTE-35 载荷解析失败（table_id / section_length 或命令体异常）。"));
        }
        return issues;
    }
    if (rule.id == "scte35.crc_invalid") {
        if (!result.aux_data_analyzed) return issues;
        const int count = result.aux_data.crc_invalid_count;
        if (Triggered(rule, static_cast<double>(count))) {
            issues.push_back(make_issue(static_cast<double>(count),
                "有 " + std::to_string(count) +
                " 个 SCTE-35 section 的 CRC_32 校验不通过，可能是传输损坏或被拼接过。"));
        }
        return issues;
    }
    if (rule.id == "scte35.duration_missing") {
        if (!result.aux_data_analyzed) return issues;
        int count = 0;
        for (const model::Scte35Cue& cue : result.aux_data.cues) {
            // splice_insert 没带 break_duration、segmentation 也没给时长 -> 下游不知道插多长
            if (cue.valid && !cue.cancel_indicator && !cue.has_duration &&
                cue.command == model::Scte35Command::SpliceInsert) {
                bool seg_duration = false;
                for (const model::Scte35Segmentation& seg : cue.segmentation) {
                    if (seg.has_duration) seg_duration = true;
                }
                if (!seg_duration) ++count;
            }
        }
        if (Triggered(rule, static_cast<double>(count))) {
            issues.push_back(make_issue(static_cast<double>(count),
                "有 " + std::to_string(count) +
                " 条 splice_insert cue 没有给 duration，下游无法判断广告插入多长。"));
        }
        return issues;
    }

    if (rule.id == "audio.channel_missing") {
        int missing = 0;
        int stream_index = -1;
        for (const auto& stream : result.streams) {
            if (stream.IsAudio() && stream.channels <= 0) {
                ++missing;
                if (stream_index < 0) stream_index = stream.index;
            }
        }
        if (Triggered(rule, static_cast<double>(missing))) {
            issues.push_back(make_issue(static_cast<double>(missing),
                std::to_string(missing) + " 条音频流未标注声道数。",
                model::TimeRange::Global(), stream_index, missing));
        }
        return issues;
    }

    return issues;
}

} // namespace analyzer
} // namespace videoeye
