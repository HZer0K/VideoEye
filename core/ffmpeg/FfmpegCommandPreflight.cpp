#include "core/ffmpeg/FfmpegCommandPreflight.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include "core/ffmpeg/FfmpegCommandParser.h"

namespace videoeye {
namespace ffmpeg {
namespace {

/// 协议前缀判断。
///
/// `http://…`（已由 "://" 挡掉）之外还有一批**带冒号但不是 URL** 的写法：
/// `pipe:1`、`concat:a.txt|b.txt`、`subfile:`、`cache:…`。它们都不是磁盘路径。
/// 同时必须放过 Windows 盘符 `C:\` / `C:/` —— 冒号后面紧跟分隔符的一律当盘符，
/// 否则整台机器上所有 Windows 路径都会被误判成"未知协议"。
bool HasProtocolPrefix(const QString& text) {
    const int colon = text.indexOf(QLatin1Char(':'));
    if (colon <= 0 || colon + 1 >= text.size()) {
        return false;
    }
    const QChar next = text.at(colon + 1);
    if (next == QLatin1Char('\\') || next == QLatin1Char('/')) {
        return false; // C:\ / C:/
    }
    for (int i = 0; i < colon; ++i) {
        const QChar c = text.at(i);
        const bool ok = c.isLetterOrNumber() || c == QLatin1Char('+') || c == QLatin1Char('-') || c == QLatin1Char('.');
        if (!ok) {
            return false; // 前导有非协议字符（例如中文路径里的冒号）→ 当普通路径
        }
    }
    return true;
}

/// 这个 token 看起来像不像"一个文件"。
///
/// 只用来挡 `-f lavfi -i testsrc2`、`-f lavfi -i "color=c=red:s=320x240"` 这类**虚拟输入**：
/// 它们没有扩展名也没有分隔符，但确实不是磁盘上的文件，报"文件不存在"是纯误报。
/// 代价是"不带扩展名的真实文件"也会被跳过 —— 宁可少报一次。
bool LooksLikePathToken(const QString& text) {
    return text.contains(QLatin1Char('/')) || text.contains(QLatin1Char('\\')) || text.contains(QLatin1Char('.'));
}

/// 比较两个路径是否指向同一个文件。
///
/// 存在的一方用 canonical（解析符号链接、Windows 上拿到真实大小写），不存在的一方用
/// absolute；Windows 下再折叠大小写 —— `A.mp4` 和 `a.mp4` 在 NTFS 上是同一个文件。
QString NormalizePathForCompare(const QString& path) {
    const QFileInfo info(path);
    QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty()) {
        resolved = info.absoluteFilePath();
    }
#ifdef Q_OS_WIN
    return resolved.toCaseFolded();
#else
    return resolved;
#endif
}

void PushIssue(std::vector<PreflightIssue>& issues, QSet<QString>& seen, PreflightSeverity severity, const QString& id,
               const QString& path, const QString& message) {
    // 同一路径只报一次：输入被多个输出引用、或者同一个坏路径写了两遍都很常见。
    const QString key = id + QLatin1Char('|') + NormalizePathForCompare(path);
    if (seen.contains(key)) {
        return;
    }
    seen.insert(key);
    issues.push_back(PreflightIssue{severity, id, message, path});
}

} // namespace

bool IsLocalPathTarget(const QString& target) {
    const QString text = target.trimmed();
    if (text.isEmpty()) {
        return false;
    }
    if (text == QLatin1String("-")) {
        return false; // stdout / stdin
    }
    if (text.contains(QLatin1String("://"))) {
        return false; // http / rtmp / rtsp / srt / udp …
    }
    return !HasProtocolPrefix(text);
}

std::vector<PreflightIssue> PreflightCommand(const QStringList& arguments) {
    std::vector<PreflightIssue> issues;
    if (IsInformationCommand(arguments)) {
        return issues; // -version / -encoders：既没输入也没输出
    }

    const FfmpegCommandStructure structure = AnalyzeCommandStructure(arguments);
    if (!structure.complete) {
        // 有 token 归属不明（字典没收录的选项，后面跟的那个值到底是值还是文件说不清）。
        // 这时候连"哪个是输出"都不确定，宁可不检查。
        return issues;
    }

    QSet<QString> seen;
    QStringList local_inputs;

    // 先过一遍输入：输出要与它们比对，所以必须全部收齐再判输出。
    for (const FfmpegArgPlacement& place : structure.args) {
        if (place.role != FfmpegArgRole::InputUrl) {
            continue;
        }
        if (place.index < 0 || place.index >= arguments.size()) {
            continue;
        }
        const QString token = arguments.at(place.index);
        if (!IsLocalPathTarget(token) || !LooksLikePathToken(token)) {
            continue;
        }
        local_inputs.append(token);

        const QFileInfo info(token);
        if (!info.exists()) {
            PushIssue(issues, seen, PreflightSeverity::Error, QStringLiteral("input-missing"), token,
                      QStringLiteral("输入文件不存在：%1").arg(token));
        } else if (info.isDir()) {
            PushIssue(issues, seen, PreflightSeverity::Error, QStringLiteral("input-is-directory"), token,
                      QStringLiteral("输入是一个目录，不是媒体文件：%1").arg(token));
        }
    }

    for (const FfmpegArgPlacement& place : structure.args) {
        if (place.role != FfmpegArgRole::OutputUrl) {
            continue;
        }
        if (place.index < 0 || place.index >= arguments.size()) {
            continue;
        }
        const QString token = arguments.at(place.index);
        if (!IsLocalPathTarget(token)) {
            continue; // 管道 / URL：不落磁盘
        }

        const QFileInfo info(token);
        if (info.exists() && info.isDir()) {
            PushIssue(issues, seen, PreflightSeverity::Error, QStringLiteral("output-is-directory"), token,
                      QStringLiteral("输出路径是一个目录，ffmpeg 要的是文件名：%1").arg(token));
            continue;
        }

        // 输出到一个还不存在的子目录：ffmpeg 不会替你建目录，最后只报一句
        // "No such file or directory"，指的还是那个输出文件，很容易被当成输入的问题。
        const QString parent = info.absolutePath();
        if (!parent.isEmpty() && !QFileInfo(parent).isDir()) {
            PushIssue(issues, seen, PreflightSeverity::Error, QStringLiteral("output-dir-missing"), parent,
                      QStringLiteral("输出目录不存在（ffmpeg 不会自动建目录）：%1").arg(parent));
        }

        for (const QString& input : local_inputs) {
            if (NormalizePathForCompare(input) == NormalizePathForCompare(token)) {
                PushIssue(issues, seen, PreflightSeverity::Error, QStringLiteral("same-as-input"), token,
                          QStringLiteral("输出和输入是同一个文件：%1").arg(token));
                break;
            }
        }
    }

    return issues;
}

} // namespace ffmpeg
} // namespace videoeye
