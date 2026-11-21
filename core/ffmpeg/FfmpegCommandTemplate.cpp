#include "core/ffmpeg/FfmpegCommandTemplate.h"

#include <QDir>
#include <QFileInfo>

namespace videoeye {
namespace ffmpeg {
namespace {

const char* const kInputSlot = "{input}";
const char* const kOutputSlot = "{output}";

/// 路径里出现空格就必须加引号：ParseCommandLine 只按空白切 token，
/// 不加引号的 `C:/a b/x.mp4` 会被切成两个参数。
QString QuotePathIfNeeded(const QString& path) {
    if (path.isEmpty()) {
        return QString();
    }
    if (!path.contains(QLatin1Char(' ')) && !path.contains(QLatin1Char('\t'))) {
        return path;
    }
    return QLatin1Char('"') + path + QLatin1Char('"');
}

FfmpegCommandTemplate MakeTemplate(const QString& id, const QString& title, const QString& summary,
                                   const QStringList& arguments, const QString& output_extension,
                                   const QString& focus_literal = QString(), bool output_is_sequence = false) {
    FfmpegCommandTemplate tmpl;
    tmpl.id = id;
    tmpl.title = title;
    tmpl.summary = summary;
    tmpl.arguments = arguments;
    tmpl.output_extension = output_extension;
    tmpl.output_is_sequence = output_is_sequence;
    tmpl.focus_literal = focus_literal;
    return tmpl;
}

} // namespace

const std::vector<FfmpegCommandTemplate>& FfmpegTemplateLibrary() {
    // 这里的每一行都在 ffmpeg 6.1 上跑通过（2026-11-21 实测），不是抄来的片段。
    // 统一不带 -y：覆盖确认由页面负责，模板不该预先替用户答应覆盖。
    static const std::vector<FfmpegCommandTemplate> library = {
        MakeTemplate(QStringLiteral("h264"), QStringLiteral("转码为 H.264 / AAC（兼容性最好）"),
                     QStringLiteral("老播放器、微信、网页都能直接放。重新编码：慢，而且有损。"),
                     {QStringLiteral("-i"), QLatin1String(kInputSlot), QStringLiteral("-c:v"),
                      QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("medium"),
                      QStringLiteral("-crf"), QStringLiteral("23"), QStringLiteral("-pix_fmt"),
                      QStringLiteral("yuv420p"), QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-b:a"),
                      QStringLiteral("128k"), QStringLiteral("-movflags"), QStringLiteral("+faststart"),
                      QLatin1String(kOutputSlot)},
                     QStringLiteral("mp4"), QStringLiteral("23")),

        MakeTemplate(QStringLiteral("copy"), QStringLiteral("只换封装（流拷贝）"),
                     QStringLiteral("不重新编码：几秒完成、画质一字不差。前提是源编码能被目标容器接受。"),
                     {QStringLiteral("-i"), QLatin1String(kInputSlot), QStringLiteral("-c"), QStringLiteral("copy"),
                      QStringLiteral("-movflags"), QStringLiteral("+faststart"), QLatin1String(kOutputSlot)},
                     QStringLiteral("mp4")),

        MakeTemplate(QStringLiteral("audio"), QStringLiteral("只留音轨（M4A）"),
                     QStringLiteral("去掉画面，音频转成 AAC 192k。比源音频多一次有损编码，换来处处能放。"),
                     {QStringLiteral("-i"), QLatin1String(kInputSlot), QStringLiteral("-vn"), QStringLiteral("-c:a"),
                      QStringLiteral("aac"), QStringLiteral("-b:a"), QStringLiteral("192k"),
                      QLatin1String(kOutputSlot)},
                     QStringLiteral("m4a")),

        MakeTemplate(QStringLiteral("clip"), QStringLiteral("截取片段（流拷贝，秒级）"),
                     QStringLiteral("改 -ss 起点与 -t 时长。流拷贝不重新编码，起点会吸附到最近的关键帧。"),
                     {QStringLiteral("-ss"), QStringLiteral("00:00:00"), QStringLiteral("-i"),
                      QLatin1String(kInputSlot), QStringLiteral("-t"), QStringLiteral("00:00:10"), QStringLiteral("-c"),
                      QStringLiteral("copy"), QLatin1String(kOutputSlot)},
                     QStringLiteral("mp4"), QStringLiteral("00:00:00")),

        MakeTemplate(QStringLiteral("hevc"), QStringLiteral("压体积 H.265 / HEVC"),
                     QStringLiteral("同画质体积约小 30%，代价是编码慢、老设备可能放不出来。"
                                    "-tag hvc1 是给 Apple 系兜底。"),
                     {QStringLiteral("-i"), QLatin1String(kInputSlot), QStringLiteral("-c:v"),
                      QStringLiteral("libx265"), QStringLiteral("-crf"), QStringLiteral("28"),
                      QStringLiteral("-preset"), QStringLiteral("medium"), QStringLiteral("-tag:v"),
                      QStringLiteral("hvc1"), QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-b:a"),
                      QStringLiteral("128k"), QLatin1String(kOutputSlot)},
                     QStringLiteral("mp4"), QStringLiteral("28")),

        MakeTemplate(QStringLiteral("gif"), QStringLiteral("生成 GIF"),
                     QStringLiteral("宽 480、15fps、无限循环。GIF 只有 256 色，长片段体积会很大。"),
                     {QStringLiteral("-i"), QLatin1String(kInputSlot), QStringLiteral("-vf"),
                      QStringLiteral("fps=15,scale=480:-1:flags=lanczos"), QStringLiteral("-loop"), QStringLiteral("0"),
                      QLatin1String(kOutputSlot)},
                     QStringLiteral("gif"), QStringLiteral("15")),

        MakeTemplate(QStringLiteral("frames"), QStringLiteral("逐秒抽帧（PNG 序列）"),
                     QStringLiteral("每秒一张。输出名必须带 %03d 序号 —— 多帧写同一个名字 ffmpeg 会直接报错。"),
                     {QStringLiteral("-i"), QLatin1String(kInputSlot), QStringLiteral("-vf"), QStringLiteral("fps=1"),
                      QLatin1String(kOutputSlot)},
                     QStringLiteral("png"), QStringLiteral("fps=1"), /*output_is_sequence=*/true),
    };
    return library;
}

const FfmpegCommandTemplate* FindFfmpegTemplate(const QString& id) {
    const auto& library = FfmpegTemplateLibrary();
    for (const FfmpegCommandTemplate& tmpl : library) {
        if (tmpl.id == id) {
            return &tmpl;
        }
    }
    return nullptr;
}

QString TemplateOutputPath(const FfmpegCommandTemplate& tmpl, const QString& input_path) {
    const QString trimmed = input_path.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1String("://"))) {
        // 网络流 / 合成源没有"同目录"这个概念
        return QString();
    }
    const QFileInfo info(trimmed);
    const QString base = info.completeBaseName(); // a.b.mp4 → "a.b"，别把 .b 当扩展名丢掉
    if (base.isEmpty() || tmpl.output_extension.isEmpty()) {
        return QString();
    }

    // 不要用 .arg() 拼这个串：%03d 里的 %0 会被当成占位符吃掉
    QString name = base + QLatin1Char('_') + tmpl.id;
    if (tmpl.output_is_sequence) {
        name += QLatin1String("_%03d");
    }
    name += QLatin1Char('.');
    name += tmpl.output_extension;
    return info.dir().filePath(name);
}

QStringList InstantiateTemplate(const FfmpegCommandTemplate& tmpl, const QString& input_path,
                                const QString& output_path) {
    const QString input_token = QuotePathIfNeeded(input_path.trimmed());
    const QString output_token = QuotePathIfNeeded(output_path.trimmed());

    QStringList out;
    out.reserve(tmpl.arguments.size());
    for (const QString& arg : tmpl.arguments) {
        if (arg == QLatin1String(kInputSlot)) {
            out.append(input_token.isEmpty() ? arg : input_token);
        } else if (arg == QLatin1String(kOutputSlot)) {
            out.append(output_token.isEmpty() ? arg : output_token);
        } else {
            out.append(arg);
        }
    }
    return out;
}

QStringList MissingTemplateSlots(const QStringList& arguments) {
    QStringList out;
    for (const QString& arg : arguments) {
        const bool is_slot = arg == QLatin1String(kInputSlot) || arg == QLatin1String(kOutputSlot);
        if (is_slot && !out.contains(arg)) {
            out.append(arg);
        }
    }
    return out;
}

QString ComposeTemplateCommand(const QStringList& arguments) {
    return arguments.join(QLatin1Char(' '));
}

int FindFocusToken(const QString& text, const QString& literal) {
    if (literal.isEmpty()) {
        return -1;
    }
    return text.indexOf(literal);
}

} // namespace ffmpeg
} // namespace videoeye
