#include "core/ffmpeg/FfmpegCommandDraft.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace videoeye {
namespace ffmpeg {

CommandInsertion ComposeInsertion(const QString& text, int cursor, const QString& snippet) {
    CommandInsertion result;
    result.text = text;
    result.cursor = qBound(0, cursor, static_cast<int>(text.size()));
    if (snippet.isEmpty()) {
        return result;
    }

    const QString before = text.left(result.cursor);
    const QString after = text.mid(result.cursor);
    const QString insert = snippet;

    // 左边：紧贴着一个 token 就必须先隔开。用 isSpace() 而不是 == ' '，
    // 这样用户用 Tab 分隔（手写命令时很常见）也算已隔开。
    const bool need_leading_space = !before.isEmpty() && !before.back().isSpace() && !insert.front().isSpace();

    // 右边：插入点后面还有内容时同理；插入点在末尾同样补一个空格 ——
    // `-c:v` 后面补空格，用户接下去打的才是「值」，否则会拼成 `-c:vlibx264`。
    const bool need_trailing_space = !insert.back().isSpace() && (after.isEmpty() || !after.front().isSpace());

    QString prefix = need_leading_space ? QStringLiteral(" ") : QString();
    QString suffix = need_trailing_space ? QStringLiteral(" ") : QString();

    // 光标落点：默认在片段之后（也就是补的那个空格之前 —— 让用户接着输入值）。
    int inner = insert.size();
    if (insert.size() >= 2 && insert.endsWith(QLatin1String("\"\""))) {
        // `-i ""`：停在两个引号之间，用户一打就是引号内的路径
        inner = insert.size() - 1;
    }

    result.text = before + prefix + insert + suffix + after;
    result.cursor = before.size() + prefix.size() + inner;
    return result;
}

QString DefaultOutputPath(const QString& input_path, const QString& suffix, const QString& fallback_extension) {
    const QString trimmed = input_path.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1String("://"))) {
        // 网络流 / 设备（rtmp://、udp://、lavfi）没有"同目录"这个概念
        return QString();
    }
    const QFileInfo info(trimmed);
    const QString base = info.completeBaseName(); // a.b.mp4 → "a.b"，避免把 .b 当扩展名丢掉
    if (base.isEmpty()) {
        return QString();
    }

    QString extension = info.suffix();
    if (extension.isEmpty()) {
        extension = fallback_extension;
    }
    while (extension.startsWith(QLatin1Char('.'))) {
        extension.remove(0, 1);
    }
    if (extension.isEmpty()) {
        return QString();
    }

    const QString name = base + suffix + QLatin1Char('.') + extension;
    return info.dir().filePath(name);
}

} // namespace ffmpeg
} // namespace videoeye
