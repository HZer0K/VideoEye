#include "core/ffmpeg/FfmpegRunOutputs.h"

#include <QDir>
#include <QFileInfo>

#include "core/ffmpeg/FfmpegCommandParser.h"

namespace videoeye {
namespace ffmpeg {
namespace {

/// 输出目标是"本地文件"吗。pipe:/stdout/网络地址都不是。
bool LooksLikeLocalFileTarget(const QString& target) {
    if (target.isEmpty() || target == QLatin1String("-")) {
        return false;
    }
    if (target.contains(QLatin1String("://"))) {
        return false; // rtmp:// http:// udp:// 之类
    }
    if (target.startsWith(QLatin1String("pipe:"), Qt::CaseInsensitive) ||
        target.startsWith(QLatin1String("concat:"), Qt::CaseInsensitive)) {
        return false;
    }
    return true;
}

} // namespace

bool HasSequencePattern(const QString& path) {
    // ffmpeg 的 image2 语法：%d / %3d / %03d。这里只看有没有，不解析宽度。
    for (int i = 0; i + 1 < path.size(); ++i) {
        if (path.at(i) != QLatin1Char('%')) {
            continue;
        }
        int j = i + 1;
        while (j < path.size() && path.at(j).isDigit()) {
            ++j;
        }
        if (j < path.size() && path.at(j) == QLatin1Char('d')) {
            return true;
        }
    }
    return false;
}

QString ExpandSequencePattern(const QString& path, int index) {
    if (index < 0 || !HasSequencePattern(path)) {
        return path;
    }

    QString out;
    out.reserve(path.size() + 4);
    for (int i = 0; i < path.size(); ++i) {
        const QChar ch = path.at(i);
        if (ch != QLatin1Char('%')) {
            out.append(ch);
            continue;
        }
        int j = i + 1;
        while (j < path.size() && path.at(j).isDigit()) {
            ++j;
        }
        if (j >= path.size() || path.at(j) != QLatin1Char('d')) {
            out.append(ch); // 不是序号模式（例如 %s），原样留着
            continue;
        }

        const QString digits = path.mid(i + 1, j - i - 1);
        QString number = QString::number(index);
        if (!digits.isEmpty()) {
            // %03d 的宽度 = 数字部分的数值（"03" → 3），不足左补 0
            const int width = digits.toInt();
            while (number.size() < width) {
                number.prepend(QLatin1Char('0'));
            }
        }
        out.append(number);
        i = j; // 跳过模式本身（循环末尾还会 ++i，正好落到 'd' 之后）
    }
    return out;
}

FfmpegRunOutputs CollectRunOutputs(const QStringList& arguments) {
    FfmpegRunOutputs outputs;
    const FfmpegCommandStructure structure = AnalyzeCommandStructure(arguments);

    for (const FfmpegArgPlacement& placement : structure.args) {
        if (placement.role != FfmpegArgRole::OutputUrl || placement.index < 0 || placement.index >= arguments.size()) {
            continue;
        }
        const QString target = arguments.at(placement.index);
        if (!LooksLikeLocalFileTarget(target)) {
            continue;
        }

        if (HasSequencePattern(target)) {
            outputs.has_sequence = true;
            const QString first_frame = ExpandSequencePattern(target, 1);
            if (QFileInfo::exists(first_frame)) {
                outputs.files.append(first_frame);
            } else {
                // 报模板原文：用户要对照的是自己写的那个模式，不是我们展开出来的第 1 帧
                outputs.missing.append(target);
            }
            continue;
        }

        if (QFileInfo(target).isFile()) {
            outputs.files.append(target);
        } else {
            outputs.missing.append(target);
        }
    }
    return outputs;
}

QString RevealDirectory(const FfmpegRunOutputs& outputs) {
    if (!outputs.files.isEmpty()) {
        const QFileInfo info(outputs.files.front());
        const QDir dir = info.absoluteDir();
        if (dir.exists()) {
            return dir.absolutePath();
        }
    }
    // 没产出时也尽量给个位置：用户多半想去看看"为什么没有"
    if (!outputs.missing.isEmpty()) {
        const QFileInfo info(outputs.missing.front());
        const QDir dir = info.absoluteDir();
        if (dir.exists()) {
            return dir.absolutePath();
        }
    }
    return QString();
}

} // namespace ffmpeg
} // namespace videoeye
