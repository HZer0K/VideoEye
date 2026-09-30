#pragma once

// 清单文本解析的公共小工具（HLS m3u8 与 DASH MPD 共用）
//
// 之所以存在这个文件：
//   HLS 的属性串（EXT-X-STREAM-INF 后那一串 KEY=VALUE）与 DASH 的 XML 属性，
//   语法不同但都要"按 KEY 取值、支持引号、把字符串转数字"。两边各写一遍必然走样，
//   统一放这里，header-only（inline），不额外产生编译单元。
//
// 依赖边界：只用 C++17 标准库，不碰 Qt / FFmpeg，纯逻辑可单测。

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace videoeye {
namespace utils {
namespace manifest {

inline std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

inline std::string ToLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool Contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// 是否网络 URI。只看 "://" 是否出现在第一个 '/' 之前，避免把本地路径误判。
inline bool IsRemoteUri(const std::string& uri) {
    const size_t scheme = uri.find("://");
    if (scheme == std::string::npos)
        return false;
    const size_t slash = uri.find('/');
    return slash == std::string::npos || scheme < slash;
}

// 路径分隔符统一成 '/'，并返回所在目录（不带尾斜杠）。找不到分隔符时返回 "."。
inline std::string DirOf(const std::string& path) {
    std::string p = path;
    for (char& c : p) {
        if (c == '\\')
            c = '/';
    }
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return p.substr(0, slash);
}

// 把 "a/b/../c" 归一化成 "a/c"（只处理 '.' 与 '..'，够清单里的相对路径用）
inline std::string NormalizePath(std::string path) {
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    const bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        const size_t next = path.find('/', i);
        const std::string seg = (next == std::string::npos) ? path.substr(i) : path.substr(i, next - i);
        if (!seg.empty() && seg != ".") {
            if (seg == ".." && !parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else {
                parts.push_back(seg);
            }
        }
        if (next == std::string::npos)
            break;
        i = next + 1;
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k)
            out += "/";
        out += parts[k];
    }
    return out.empty() ? "." : out;
}

inline std::string JoinPath(const std::string& dir, const std::string& name) {
    if (name.empty())
        return dir;
    if (!name.empty() && (name[0] == '/' || name[0] == '\\'))
        return NormalizePath(name);
    std::string d = dir;
    for (char& c : d) {
        if (c == '\\')
            c = '/';
    }
    while (!d.empty() && d.back() == '/')
        d.pop_back();
    return NormalizePath(d.empty() ? name : d + "/" + name);
}

// 把清单里的 URI 解析成本地路径。
// 返回空串表示"不是本地文件"（网络 URI 或绝对路径不可用）——第一阶段只处理本地。
inline std::string ResolveLocalUri(const std::string& base_dir, const std::string& uri) {
    if (uri.empty() || IsRemoteUri(uri))
        return std::string();
    return JoinPath(base_dir, uri);
}

inline std::string ExtensionOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t q = path.find('?');
    size_t end = (q == std::string::npos) ? path.size() : q;
    const size_t dot = path.find_last_of('.', end == 0 ? std::string::npos : end - 1);
    if (dot == std::string::npos)
        return std::string();
    if (slash != std::string::npos && dot < slash)
        return std::string();
    return ToLower(path.substr(dot + 1, (end > dot + 1) ? (end - dot - 1) : std::string::npos));
}

// 取文件大小，同时兼作"是否存在"判定。用 ifstream 而非 std::filesystem，
// 免得在老一些的 GCC 上额外链 -lstdc++fs。
inline bool FileSizeOf(const std::string& path, int64_t& size_out) {
    size_out = 0;
    if (path.empty())
        return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const std::streamoff sz = f.tellg();
    if (sz < 0)
        return false;
    size_out = static_cast<int64_t>(sz);
    return true;
}

// 解析 KEY=VALUE 属性串（逗号分隔，值可用双引号包裹，引号内允许逗号）。
// HLS: BANDWIDTH=1280000,RESOLUTION=1920x1080,CODECS="avc1.64001f,mp4a.40.2"
// DASH XML: 同一套，只是分隔符是空白 —— 调用方先按空白切好再传进来。
inline std::map<std::string, std::string> ParseAttributes(const std::string& text, char separator) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == separator || text[i] == ' ' || text[i] == '\t'))
            ++i;
        if (i >= text.size())
            break;

        const size_t eq = text.find('=', i);
        if (eq == std::string::npos)
            break;
        std::string key = Trim(text.substr(i, eq - i));
        i = eq + 1;
        if (i >= text.size())
            break;

        std::string value;
        if (text[i] == '"') {
            ++i;
            while (i < text.size() && text[i] != '"')
                value += text[i++];
            if (i < text.size())
                ++i; // 吃掉右引号
        } else {
            while (i < text.size() && text[i] != separator)
                value += text[i++];
            value = Trim(value);
        }
        if (!key.empty())
            out[key] = value;
        // 跳到下一个分隔符之后（引号串后面可能直接跟逗号）
        while (i < text.size() && text[i] != separator)
            ++i;
        if (i < text.size() && text[i] == separator)
            ++i;
    }
    return out;
}

inline std::map<std::string, std::string> ParseHlsAttributes(const std::string& text) {
    return ParseAttributes(text, ',');
}

// XML 标签的属性（空白分隔，值用单引号或双引号）
inline std::map<std::string, std::string> ParseXmlAttributes(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i >= text.size())
            break;
        size_t start = i;
        while (i < text.size() && text[i] != '=' && !std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i >= text.size() || text[i] != '=')
            continue;
        std::string key = text.substr(start, i - start);
        ++i;
        if (i >= text.size())
            break;
        const char quote = text[i];
        std::string value;
        if (quote == '"' || quote == '\'') {
            ++i;
            while (i < text.size() && text[i] != quote)
                value += text[i++];
            if (i < text.size())
                ++i;
        } else {
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
                value += text[i++];
        }
        if (!key.empty())
            out[key] = value;
    }
    return out;
}

inline std::string AttrString(const std::map<std::string, std::string>& attrs, const std::string& key) {
    auto it = attrs.find(key);
    return it == attrs.end() ? std::string() : it->second;
}

inline bool AttrBool(const std::map<std::string, std::string>& attrs, const std::string& key,
                     bool default_value = false) {
    auto it = attrs.find(key);
    if (it == attrs.end())
        return default_value;
    const std::string v = ToLower(Trim(it->second));
    return v == "true" || v == "1" || v == "yes";
}

// 支持十进制与 HLS 常见的 0x 十六进制（如 EXT-X-MEDIA-SEQUENCE 不会用，但 KEY 的 IV 会）
inline bool AttrInt64(const std::map<std::string, std::string>& attrs, const std::string& key, int64_t& out) {
    auto it = attrs.find(key);
    if (it == attrs.end())
        return false;
    const std::string v = Trim(it->second);
    if (v.empty())
        return false;
    try {
        size_t pos = 0;
        int base = 10;
        std::string body = v;
        if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
            base = 16;
            body = body.substr(2);
        }
        const long long parsed = std::stoll(body, &pos, base);
        if (pos != body.size())
            return false;
        out = static_cast<int64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

inline int64_t AttrInt64Or(const std::map<std::string, std::string>& attrs, const std::string& key,
                           int64_t default_value) {
    int64_t v = 0;
    return AttrInt64(attrs, key, v) ? v : default_value;
}

inline bool AttrDouble(const std::map<std::string, std::string>& attrs, const std::string& key, double& out) {
    auto it = attrs.find(key);
    if (it == attrs.end())
        return false;
    const std::string v = Trim(it->second);
    if (v.empty())
        return false;
    try {
        size_t pos = 0;
        const double parsed = std::stod(v, &pos);
        if (pos != v.size())
            return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

inline double AttrDoubleOr(const std::map<std::string, std::string>& attrs, const std::string& key,
                           double default_value) {
    double v = 0.0;
    return AttrDouble(attrs, key, v) ? v : default_value;
}

// ISO 8601 时长 -> 秒。DASH 的 mediaPresentationDuration / Period@duration /
// SegmentTimeline 之外的时间都用这个格式：PnYnMnDTnHnMnS，常见形如 PT0H1M30.000S。
inline double ParseIso8601Duration(const std::string& text) {
    if (text.empty() || text[0] != 'P')
        return 0.0;
    double seconds = 0.0;
    size_t i = 1;
    bool in_time = false;
    // 单位倍率：年/月按常用近似（DASH 里几乎不会用）
    double value = 0.0;
    bool has_value = false;
    auto apply = [&](char unit) {
        switch (unit) {
        case 'Y':
            seconds += value * 365.0 * 24.0 * 3600.0;
            break;
        case 'M':
            seconds += value * (in_time ? 60.0 : 30.0 * 24.0 * 3600.0);
            break;
        case 'W':
            seconds += value * 7.0 * 24.0 * 3600.0;
            break;
        case 'D':
            seconds += value * 24.0 * 3600.0;
            break;
        case 'H':
            seconds += value * 3600.0;
            break;
        case 'S':
            seconds += value;
            break;
        default:
            break;
        }
        value = 0.0;
        has_value = false;
    };
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == 'T') {
            in_time = true;
        } else if ((c >= '0' && c <= '9') || c == '.' || c == ',') {
            if (c == ',') {
                value += 0.0;
            } else {
                if (!has_value) {
                    value = 0.0;
                    has_value = true;
                }
                if (c == '.') {
                    // 小数部分单独累加
                    size_t j = i + 1;
                    double frac = 0.0;
                    double scale = 0.1;
                    while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
                        frac += (text[j] - '0') * scale;
                        scale *= 0.1;
                        ++j;
                    }
                    value += frac;
                    i = j - 1;
                } else {
                    value = value * 10.0 + (c - '0');
                }
            }
        } else {
            apply(c);
        }
    }
    return seconds;
}

// 从 CODECS 串里挑出视频/音频编码四字符码。
// HLS 的 CODECS 可能是 "avc1.64001f,mp4a.40.2"；DASH 的 codecs 可能是
// "avc1.640028" 或 "vp09.00.10.08" 或 "mp4a.40.2"。
inline void SplitCodecs(const std::string& codecs, std::string& video_codec, std::string& audio_codec) {
    video_codec.clear();
    audio_codec.clear();
    size_t i = 0;
    while (i <= codecs.size()) {
        size_t comma = codecs.find(',', i);
        std::string item = (comma == std::string::npos) ? codecs.substr(i) : codecs.substr(i, comma - i);
        item = Trim(item);
        if (!item.empty()) {
            // 去掉 ".profile" 之类的后缀，只留四字符码
            const size_t dot = item.find('.');
            const std::string fourcc = ToLower((dot == std::string::npos) ? item : item.substr(0, dot));
            const bool is_audio =
                StartsWith(fourcc, "mp4a") || StartsWith(fourcc, "ac-3") || StartsWith(fourcc, "ec-3") ||
                StartsWith(fourcc, "ac-4") || StartsWith(fourcc, "opus") || StartsWith(fourcc, "alac") ||
                StartsWith(fourcc, "flac") || StartsWith(fourcc, "dtsc") || StartsWith(fourcc, "mlpa");
            if (is_audio) {
                if (audio_codec.empty())
                    audio_codec = fourcc;
            } else if (video_codec.empty()) {
                video_codec = fourcc;
            }
        }
        if (comma == std::string::npos)
            break;
        i = comma + 1;
    }
}

} // namespace manifest
} // namespace utils
} // namespace videoeye
