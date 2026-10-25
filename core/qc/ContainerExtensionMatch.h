#pragma once

// container.extension_mismatch 的「扩展名 ↔ 解复用器名」匹配口径（header-only）。
//
// 单独成头的理由：这张别名表是**结论性**的 —— 判错方向不是"少一条提示"，
// 而是对着一个完全正常的文件报「扩展名与实际容器不符」。误报比缺功能更伤信任，
// 所以它必须能被单测直接钉住。留在 QcRuleEngineCheckContainer.cpp 的匿名
// namespace 里就测不到（只能造 AnalysisResult 走整条规则链，覆盖不到表本身）。
//
// 口径：container_format 是 FFmpeg 的 iformat->name，可能是逗号分隔的别名列表
// （如 MOV 解复用器叫 "mov,mp4,m4a,3gp,3g2,mj2"）。只要任一 token 等于扩展名，
// 或等于该扩展名的别名，就算一致。
//
// 依赖红线：本头只吃 <string>，不碰 Qt / FFmpeg / analysis —— VideoEyeQcRules
// 是纯 std:: + domain 的判定层，链上 Qt 或 FFmpeg 会毁掉这条边界。

#include <cctype>
#include <map>
#include <string>

namespace videoeye {
namespace qc {
namespace detail {

// 扩展名 -> 解复用器 token 的等价映射。
//
// 只登记「解复用器名里不含该扩展名本身」的情况；能直接 token 命中的
// （mp4 / mov / avi / flv / asf / ogg / mpeg / mxf / wav / flac / mp3 / aac /
// ac3 / eac3 / dts / ape / amr / h264 / hevc / vvc / obu / mj2 / 3gp / 3g2）
// 一律不写进来 —— 冗余条目会让这张表看起来像"需要逐条穷举"，进而在新增
// 格式时被抄漏。
inline const std::map<std::string, std::string>& ExtensionAliasMap() {
    static const std::map<std::string, std::string> kAlias = {
        // --- MPEG-TS 家族：192 字节包长的 m2ts/mts、以及广播圈的 .tts / .m2t ---
        {"ts", "mpegts"},
        {"m2ts", "mpegts"},
        {"mts", "mpegts"},
        {"tts", "mpegts"},
        {"m2t", "mpegts"},
        // --- Matroska 家族：解复用器名是 "matroska,webm" ---
        {"mkv", "matroska"},
        {"mka", "matroska"},
        {"mk3d", "matroska"},
        {"webm", "matroska"},
        // --- MPEG-PS 家族：解复用器名是 "mpeg" ---
        {"mpg", "mpeg"},
        {"mpe", "mpeg"},
        {"m2p", "mpeg"},
        {"ps", "mpeg"},
        {"vob", "mpeg"},
        // --- MPEG 视频裸流：解复用器名是 "mpegvideo"（与 PS 的 "mpeg" 不同）---
        // .m2v 是 MPEG-2 video elementary stream，没有 pack header。
        {"m2v", "mpegvideo"},
        {"m1v", "mpegvideo"},
        {"mpv", "mpegvideo"},
        // --- ISOBMFF 家族：解复用器名是 "mov,mp4,m4a,3gp,3g2,mj2" ---
        // 该 token 列表里没有 m4v，也没有 f4v（fMP4）/ m4b（有声书）/ m4p（DRM）。
        {"m4v", "mov"},
        {"m4b", "mov"},
        {"m4p", "mov"},
        {"m4r", "mov"},
        {"f4v", "mov"},
        // --- ASF 家族 ---
        {"wmv", "asf"},
        {"wma", "asf"},
        // --- Ogg 家族：解复用器名统一是 "ogg"，音频/视频/字幕共用同一套封装 ---
        // .opus / .spx 是编解码器名当扩展名，.ogv / .oga / .ogx 是 Ogg 的
        // 视频/音频/通用后缀 —— 这六个以前全部漏登记，是误报的主要来源。
        {"oga", "ogg"},
        {"ogv", "ogg"},
        {"ogx", "ogg"},
        {"spx", "ogg"},
        {"opus", "ogg"},
        // --- 裸流：解复用器名带 h 前缀，用户常写成短名 ---
        {"264", "h264"},
        {"avc", "h264"},
        {"265", "hevc"},
        {"h265", "hevc"},
        {"hvc", "hevc"},
        {"266", "vvc"},
        {"h266", "vvc"},
        {"av1", "obu"},
        // --- MPEG 音频：mp1/mp2/mp3 共用一个解复用器，iformat 名是 "mp3" ---
        {"mp2", "mp3"},
        {"mp1", "mp3"},
        // --- 其它常见别名 ---
        {"aif", "aiff"},
        // --- 流媒体清单：container_format 由 QtAnalysisController 直接写 "hls" / "dash" ---
        {"m3u8", "hls"},
        {"m3u", "hls"},
        {"mpd", "dash"},
    };
    return kAlias;
}

// 判断扩展名与 FFmpeg 探测到的容器是否一致。
//
// ext 允许带前导点、大小写混写（AnalysisEngine 已统一小写，这里兜住其它调用方
// 和单测）；container_format 的 token 同样会去空白 + 小写后比较。
// ext 归一化后为空 = 无法判断，返回 true（宁可不报，也不误报）。
inline bool ExtensionMatchesContainer(const std::string& raw_ext, const std::string& container_format) {
    std::string ext;
    for (const char c : raw_ext) {
        if (c == '.') {
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(c))) {
            ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (ext.empty()) {
        return true;
    }

    const auto& alias = ExtensionAliasMap();
    const auto alias_it = alias.find(ext);

    size_t start = 0;
    while (start <= container_format.size()) {
        const size_t comma = container_format.find(',', start);
        const size_t end = (comma == std::string::npos) ? container_format.size() : comma;
        std::string trimmed;
        for (size_t i = start; i < end; ++i) {
            const char c = container_format[i];
            if (!std::isspace(static_cast<unsigned char>(c))) {
                trimmed += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        if (!trimmed.empty()) {
            if (trimmed == ext)
                return true;
            if (alias_it != alias.end() && trimmed == alias_it->second)
                return true;
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return false;
}

} // namespace detail
} // namespace qc
} // namespace videoeye
