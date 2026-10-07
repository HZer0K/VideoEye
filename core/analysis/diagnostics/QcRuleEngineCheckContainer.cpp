#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <cctype>
#include <map>
#include <string>

namespace videoeye {
namespace diagnostics {
namespace detail {

namespace {

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
        // 流媒体清单：container_format 由 QtAnalysisController 直接写 "hls" / "dash"
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

// 容器级规则（container.* 中除 container.mp4.* 与流媒体清单外的全部 id）。
// 调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue> CheckContainerRules(const model::QcRule& rule,
                                                        const model::AnalysisResult& result,
                                                        const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;

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

    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye