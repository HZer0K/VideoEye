#include "core/ffmpeg/FfmpegToolKind.h"

#include <QFileInfo>

namespace videoeye {
namespace ffmpeg {
namespace {

/// 文件名去掉 `.exe`。与 FfmpegCommandParser::LooksLikeFfmpegProgram 原先的写法一致：
/// 只认 `.exe` 后缀（Linux/macOS 上的可执行文件本来就不带后缀）。
QString ProgramBaseName(const QString& token) {
    if (token.isEmpty()) {
        return QString();
    }
    QString name = QFileInfo(token).fileName();
    if (name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    return name;
}

} // namespace

FfmpegToolKind ClassifyProgramToken(const QString& token) {
    const QString name = ProgramBaseName(token);
    // 选项不是程序：`-i` / `-show_streams` 取 fileName 之后还是自己，得先挡掉。
    if (name.isEmpty() || name.startsWith(QLatin1Char('-'))) {
        return FfmpegToolKind::Unknown;
    }
    if (name.compare(QLatin1String("ffmpeg"), Qt::CaseInsensitive) == 0) {
        return FfmpegToolKind::Ffmpeg;
    }
    if (name.compare(QLatin1String("ffprobe"), Qt::CaseInsensitive) == 0) {
        return FfmpegToolKind::Ffprobe;
    }
    if (name.compare(QLatin1String("ffplay"), Qt::CaseInsensitive) == 0) {
        return FfmpegToolKind::Ffplay;
    }
    return FfmpegToolKind::Unknown;
}

unsigned ToolMaskOf(FfmpegToolKind kind) {
    switch (kind) {
    case FfmpegToolKind::Ffmpeg:
        return kToolFfmpeg;
    case FfmpegToolKind::Ffprobe:
        return kToolFfprobe;
    case FfmpegToolKind::Ffplay:
        return kToolFfplay;
    case FfmpegToolKind::Unknown:
    default:
        return kToolNone;
    }
}

QString ToolProgramName(FfmpegToolKind kind) {
    switch (kind) {
    case FfmpegToolKind::Ffmpeg:
        return QStringLiteral("ffmpeg");
    case FfmpegToolKind::Ffprobe:
        return QStringLiteral("ffprobe");
    case FfmpegToolKind::Ffplay:
        return QStringLiteral("ffplay");
    case FfmpegToolKind::Unknown:
    default:
        return QString();
    }
}

QString ToolPurposeText(FfmpegToolKind kind) {
    switch (kind) {
    case FfmpegToolKind::Ffmpeg:
        return QStringLiteral("转码 / 处理：读入媒体，按参数重新编码后写出文件");
    case FfmpegToolKind::Ffprobe:
        return QStringLiteral("查询媒体信息：读入媒体，把流、格式、帧的信息打印出来（不产出文件）");
    case FfmpegToolKind::Ffplay:
        return QStringLiteral("简易播放器：读入媒体，在独立窗口里播放（不产出文件）");
    case FfmpegToolKind::Unknown:
    default:
        return QString();
    }
}

bool ToolTakesBareInput(FfmpegToolKind kind) {
    // ffmpeg 唯一的例外是信息查询命令（-version 之类），那由 IsInformationCommand 单独判。
    return kind == FfmpegToolKind::Ffprobe || kind == FfmpegToolKind::Ffplay;
}

bool ToolProducesOutputFile(FfmpegToolKind kind) {
    return kind == FfmpegToolKind::Ffmpeg;
}

} // namespace ffmpeg
} // namespace videoeye
