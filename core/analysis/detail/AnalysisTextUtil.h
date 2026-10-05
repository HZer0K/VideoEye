#pragma once
//
// analysis 层内部的 Qt 替身集合。
//
// 为什么要有这个文件
// ------------------
// core/analysis 过去把 Qt 当 C++98 容器使（QString 当字符串、QByteArray 当字节缓冲、
// QFile+QDataStream 读文件、QMap/QVector 当容器），于是这一层被迫 PUBLIC 挂着 Qt6::Core
// —— 任何 include 它公开头文件的目标都被拖去链 Qt。而按分层方向，analysis 只该认识
// domain、media、FFmpeg 与 std；Qt 是 UI 侧的东西。
//
// scripts/audit_qt_analysis_border.py 会把 analysis 里再出现 Qt 当成违规拦下，
// 所以下面这些是**唯一**允许的过渡形态：名字说清楚替的是谁、语义与 Qt 版本对齐，
// 谁也别在别处另起炉灶写第二份。
//
// 语义对齐表（Qt 版 → 这里）
//   QByteArray::fromHex(s)                   → HexToBytes(s)
//   QByteArray::toHex()                      → BytesToHex(b)
//   QString::remove(QChar c)                 → RemoveAllCopy(s, c)
//   QString::trimmed()                       → TrimCopy(s)
//   QString::toUpper() / toLower()           → ToUpperCopy(s) / ToLowerCopy(s)
//   QString::number(v, 'f', n)               → Fixed(v, n)
//   QString("%1").arg(v, w, 16, QChar('0'))  → HexFill(v, w)  / HexFillLow(v, w)
//   QFileInfo(p).suffix().toLower()          → FileExtension(p)
//   QString("%a%b").arg(x).arg(y)            → StrCat("...", x, y)
//
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace videoeye {

/// QByteArray::fromHex：hex 文本 → 原始字节。
/// 输入全是代码里的定长常量，非 hex 字符按 Qt 的"忽略"处理即可，不另外引入错误分支；
/// 奇数长度时最后一个半字节被丢弃，与 Qt 一致。
inline std::string HexToBytes(const std::string& hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = -1;
    for (char c : hex) {
        if (c == '_') continue;  // Qt 的 fromHex 也跳过下划线
        const int v = nibble(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<char>(hi * 16 + v));
            hi = -1;
        }
    }
    return out;
}

/// QByteArray::toHex()：原始字节 → 小写 hex 文本（Qt 默认也是小写）。
inline std::string BytesToHex(const std::string& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0x0F]);
    }
    return out;
}

/// QString::remove(QChar c)：删掉**所有**等于 c 的字符（QString 的 remove 是全量删，
/// 只删第一个的写法是 removeFirst）。分析层只用来剥 UTF-16 里的结尾 NUL。
inline std::string RemoveAllCopy(const std::string& s, char c) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch != c) out.push_back(ch);
    }
    return out;
}

/// QString::trimmed()：去掉首尾空白（空格 / \t \n \r \f \v）。
inline std::string TrimCopy(const std::string& s) {
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

/// QString::toUpper()。分析层只用于四字符码与格式名，ASCII 转换即可。
inline std::string ToUpperCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

/// QString::toLower()（同上）。
inline std::string ToLowerCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/// QString("%1").arg(v, width, 16, QChar('0')) 的等价物之一：小写定宽十六进制。
/// Qt 的 base=16 默认就是小写数字，所以这是更贴原义的那个；大写版由它转出来。
inline std::string HexFillLow(uint64_t v, int width) {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%0*llx", width,
                                static_cast<unsigned long long>(v));
    return std::string(buf, static_cast<size_t>(n < 0 ? 0 : n));
}

/// 同上但大写。用法上对应原本在 .arg(...) 后面又 .toUpper() 的那些点。
inline std::string HexFill(uint64_t v, int width) { return ToUpperCopy(HexFillLow(v, width)); }

/// QString::number(v, 'f', digits) 的等价物：定宽小数，不切科学计数法。
inline std::string Fixed(double v, int digits) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return std::string(buf, static_cast<size_t>(n < 0 ? 0 : n));
}

// ---------------------------------------------------------------------------
// StrCat：QString("%1x%2").arg(w).arg(h) 的最小等价物
//
// 逐段走格式串：%1..%9 是**按出现顺序**的占位符 —— 第 N 个出现的 %N 吃第 N 个实参
// （所以 "%3 %1 %2" 配 (1,2,3) 得到 "1 2 3"，不是 "3 1 2"；想换序就把实参顺序换掉）。
// `%%` 输出一个 `%` 并且**不**吃实参（Qt 的 arg() 同理，它只按 "%N" 这个词面找占位符）。
// 实参用 ostream 的 << 落串，默认 6 位有效数字，与 QString::arg 的 `%1` 精度一致；
// 多余的 %N 会原样留下（Qt 也是这样），缺参数则编译期就被重载决议挡下
// —— 宁可编译不过，也不要悄悄拼错字符串。
// ---------------------------------------------------------------------------
/// 输出 p 到下一个 '%' 之前的字面量，并吃掉占位符或转义符。
/// 返回"这一步有没有真的吃掉一个 %N 占位符" —— 上层据此决定要不要落实参。
///
/// 返回 bool 是为了 `%%`：Qt 的 arg() 并不认识 `%%`（它只按 "%1" 这个词面找占位符，
/// 遇到 `%%` 当字面量跳过去），所以 `%%` 之后**不该**再落一个实参。早先的版本让
/// `%%` 之后照吃实参，于是 StrCat("%%1%%", 5) 拼出 "51%" 而不是 "1%" ——
/// 这种串只有把日志打出来才看得见，所以钉在这里。
inline bool EmitNext(std::ostringstream& os, const char*& p) {
    const char* s = p;
    while (*p && *p != '%') ++p;
    os.write(s, static_cast<std::streamsize>(p - s));
    if (*p == '\0') return false;
    ++p;                                    // 吃掉 %
    if (*p == '%') {
        ++p;
        os << '%';                          // `%%` → 一个 %（别把这个 % 吞掉）
        return false;                       // 且它不是占位符，不吃实参
    }
    if (*p >= '1' && *p <= '9') { ++p; return true; }
    return false;                           // 孤立的 '%'，原样留着
}

template <typename T>
inline void EmitNext(std::ostringstream& os, const char*& p, const T& v) {
    if (EmitNext(os, p)) os << v;
}

template <typename T, typename... Rest>
inline void EmitNext(std::ostringstream& os, const char*& p, const T& v, const Rest&... rest) {
    EmitNext(os, p, v);
    EmitNext(os, p, rest...);
}

template <typename... Args>
inline std::string StrCat(const char* fmt, const Args&... args) {
    std::ostringstream os;
    const char* p = fmt;
    EmitNext(os, p, args...);
    // 收尾：最后一个占位符之后到串尾。`%%` 在这里也要折成一个 %（没有实参可吃时
    // 会走到这条尾巴上，例如 StrCat("100%%")）。
    while (*p) {
        if (*p == '%' && *(p + 1) == '%') {
            os << '%';
            p += 2;
        } else {
            os << *p++;
        }
    }
    return os.str();
}

/// QString::startsWith(prefix)（默认大小写敏感）的等价物。
/// 用 prefix.size() 而不是 npos 作长度，这样 prefix 比被查串还长时返回 false，与 Qt 一致
/// （substr 那条路会因为越界抛 out_of_range）。
inline bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool StartsWith(const std::string& s, const char* prefix) {
    return StartsWith(s, std::string(prefix));
}

/// QFileInfo(path).suffix().toLower() 的等价物：最后一个点之后的部分，全小写。
/// 没有点、以点结尾、或点在最前面（".hidden"）时返回空串，与 QFileInfo 一致。
inline std::string FileExtension(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    // 三条边界：没有点（"clip"）、点在目录分隔符之前（"/a.b/clip" 里的那个点属
    // 上级目录名）、点在最前面（".hidden" 是隐藏文件不是扩展名）。
    //
    // 千万别写成 `dot <= slash`：slash 找不到时是 npos = (size_t)-1，任何真的 dot
    // 都 <= npos，于是**无目录分隔符的路径一律返回空** —— 也就是相对文件名
    // （"clip.mp4"）这条路会被判成"没有扩展名"。FormatDetector 正在用它做格式探测。
    if (dot == std::string::npos || dot == 0) return {};
    if (slash != std::string::npos && dot <= slash) return {};
    if (dot + 1 >= path.size()) return {};
    return ToLowerCopy(path.substr(dot + 1));
}

}  // namespace videoeye
