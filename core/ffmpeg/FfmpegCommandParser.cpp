#include "core/ffmpeg/FfmpegCommandParser.h"

#include <QFileInfo>

#include "core/ffmpeg/FfmpegCommandCatalog.h"

namespace videoeye {
namespace ffmpegtool {
namespace {

// shell 操作符。只在**引号外**检测 —— "-vf drawtext=text='a|b'" 里的竖线是参数内容。
bool IsShellOperatorChar(QChar c) {
    return c == QLatin1Char('|') || c == QLatin1Char(';') ||
           c == QLatin1Char('>') || c == QLatin1Char('<') ||
           c == QLatin1Char('&') || c == QLatin1Char('`') ||
           c == QLatin1Char('$');
}

QString ShellOperatorName(QChar c) {
    switch (c.toLatin1()) {
    case '|': return QStringLiteral("管道 |");
    case ';': return QStringLiteral("命令分隔 ;");
    case '>': return QStringLiteral("输出重定向 >");
    case '<': return QStringLiteral("输入重定向 <");
    case '&': return QStringLiteral("后台/串联 &");
    case '`': return QStringLiteral("命令替换 `");
    case '$': return QStringLiteral("变量/命令替换 $");
    default:  return QString(c);
    }
}

// 单独的 "-" 是"标准输入/输出"的约定写法，它不是选项。
bool IsOptionToken(const QString& token) {
    return token.startsWith(QLatin1Char('-')) && token != QLatin1String("-");
}

// 往进程通道（而不是文件）写的输出目标。注意 pipe:2 写的是 stderr，
// 一样会把二进制灌进本页的日志区。
bool IsPipeOutputTarget(const QString& target) {
    return target == QLatin1String("-") ||
           target.startsWith(QLatin1String("pipe:"), Qt::CaseInsensitive);
}

// 只产出文本的 muxer。它们即使写到管道上也只是几行文本，不会灌满日志框。
bool WritesTextOnly(const QString& format) {
    if (format.isEmpty()) {
        return false;
    }
    static const QStringList kTextOnly = {
        QStringLiteral("null"), QStringLiteral("md5"), QStringLiteral("framemd5"),
        QStringLiteral("framecrc"), QStringLiteral("ffmetadata"),
    };
    return kTextOnly.contains(format, Qt::CaseInsensitive);
}

// 从 from 往后还能不能再看到一个 -i（用于判断 -f 管的是输入还是输出）
bool HasLaterInput(const QStringList& arguments, int from) {
    for (int i = from; i < arguments.size(); ++i) {
        if (arguments.at(i) == QLatin1String("-i")) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool LooksLikeFfmpegProgram(const QString& token) {
    if (token.isEmpty()) {
        return false;
    }
    const QString base = QFileInfo(token).fileName();
    QString name = base;
    if (name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    return name.compare(QLatin1String("ffmpeg"), Qt::CaseInsensitive) == 0;
}

ParsedCommand ParseCommandLine(const QString& text) {
    ParsedCommand result;

    QStringList tokens;
    QString current;
    bool in_quote = false;
    QChar quote_char;
    bool has_content = false;   // 是否出现过非空字符（用于区分 "" 这种空引号参数）

    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);

        if (in_quote) {
            if (c == quote_char) {
                in_quote = false;
            } else {
                current.append(c);
            }
            continue;
        }

        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            in_quote = true;
            quote_char = c;
            has_content = true;
            continue;
        }

        if (IsShellOperatorChar(c)) {
            result.status = CommandParseStatus::ShellOperator;
            result.message = QStringLiteral("检测到 shell 操作符「%1」。\n"
                                            "命令工作台只执行单独一条 ffmpeg 命令，"
                                            "不支持管道、串联与重定向 —— 请把结果先输出到文件。")
                                 .arg(ShellOperatorName(c));
            return result;
        }

        if (c.isSpace()) {
            if (has_content) {
                tokens.append(current);
                current.clear();
                has_content = false;
            }
            continue;
        }

        current.append(c);
        has_content = true;
    }

    if (in_quote) {
        result.status = CommandParseStatus::UnterminatedQuote;
        result.message = QStringLiteral("引号没有闭合（缺少一个 %1）。\n"
                                        "含空格或中文的路径请用成对引号包起来。")
                             .arg(quote_char);
        return result;
    }
    if (has_content) {
        tokens.append(current);
    }

    if (tokens.isEmpty()) {
        result.status = CommandParseStatus::Empty;
        result.message = QStringLiteral("命令为空。");
        return result;
    }

    // 程序名: 是 ffmpeg → 丢掉（实际执行的是设置里那个 ffmpeg）; 以 '-' 开头 → 用户省略了程序名。
    int arg_start = 0;
    const QString& first = tokens.front();
    if (LooksLikeFfmpegProgram(first)) {
        result.program_token = first;
        arg_start = 1;
    } else if (first.startsWith(QLatin1Char('-'))) {
        arg_start = 0;
    } else {
        result.status = CommandParseStatus::UnsupportedProgram;
        result.message = QStringLiteral("第一个参数是「%1」，不是 ffmpeg。\n"
                                        "命令工作台只执行 ffmpeg 命令（可写 ffmpeg、ffmpeg.exe 或它的完整路径，"
                                        "也可以直接省略程序名从 -i 开始写）。")
                             .arg(first);
        return result;
    }

    result.arguments = tokens.mid(arg_start);
    result.status = CommandParseStatus::Ok;
    return result;
}

FfmpegCommandStructure AnalyzeCommandStructure(const QStringList& arguments) {
    FfmpegCommandStructure out;
    const int n = arguments.size();
    out.args.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.args[static_cast<size_t>(i)].index = i;
    }

    int inputs_seen = 0;
    int outputs_seen = 0;
    QString pending_output_format;   // 尚未落地的那个输出组当前的 -f 值

    for (int i = 0; i < n; ++i) {
        const QString token = arguments.at(i);
        FfmpegArgPlacement& place = out.args[static_cast<size_t>(i)];
        place.output_side = (inputs_seen > 0);

        if (token == QLatin1String("-i")) {
            place.role = FfmpegArgRole::Option;
            if (i + 1 >= n) {
                out.complete = false;   // -i 少了文件名
                continue;
            }
            place.value_index = i + 1;
            out.args[static_cast<size_t>(i + 1)].role = FfmpegArgRole::InputUrl;
            ++inputs_seen;
            ++i;
            continue;
        }

        if (IsOptionToken(token)) {
            place.role = FfmpegArgRole::Option;
            const FfmpegCatalogEntry* entry = FfmpegCommandCatalog::Find(token);

            if (entry != nullptr && entry->takes_value) {
                // 值 '-' 开头的不吞（-ss -10 这种负数值极罕见，吞了输出文件更亏）
                const bool has_value = (i + 1 < n) && !arguments.at(i + 1).startsWith(QLatin1Char('-'));
                if (!has_value) {
                    continue;
                }
                place.value_index = i + 1;
                FfmpegArgPlacement& value = out.args[static_cast<size_t>(i + 1)];
                value.role = FfmpegArgRole::OptionValue;
                value.output_side = place.output_side;
                if (token == QLatin1String("-f")) {
                    // 还没产出任何输出、且后面还有 -i → 这个 -f 管下一个输入，
                    // 否则它就是（下一个）输出的格式。
                    place.applies_to_input = (outputs_seen == 0) && HasLaterInput(arguments, i + 2);
                    if (!place.applies_to_input) {
                        pending_output_format = arguments.at(i + 1);
                    }
                }
                ++i;
                continue;
            }

            if (entry == nullptr && i + 1 < n && !IsOptionToken(arguments.at(i + 1))) {
                // 字典里没有这个选项 → 不知道它带不带值。紧跟着的裸 token 可能是它的值，
                // 也可能是输出文件。认不出来就标 Uncertain，不替用户下结论。
                FfmpegArgPlacement& next = out.args[static_cast<size_t>(i + 1)];
                next.role = FfmpegArgRole::Uncertain;
                next.output_side = place.output_side;
                out.complete = false;
                ++i;
            }
            continue;
        }

        // 裸 token（含单独的 "-"）
        if (inputs_seen == 0) {
            // 任何 -i 之前就出现的裸参数不合 ffmpeg 语法，多半是漏写了 -i
            place.role = FfmpegArgRole::Uncertain;
            out.complete = false;
            continue;
        }
        place.role = FfmpegArgRole::OutputUrl;
        place.output_index = outputs_seen++;
        place.output_format = pending_output_format;
        pending_output_format.clear();
    }

    out.input_count = inputs_seen;
    out.output_count = outputs_seen;
    out.has_input = (inputs_seen > 0);
    return out;
}

QString DetectStdoutMediaOutput(const QStringList& arguments) {
    const FfmpegCommandStructure structure = AnalyzeCommandStructure(arguments);
    for (const FfmpegArgPlacement& place : structure.args) {
        // 输出组里的明晰目标 + 归属不明但落在输出侧的候选，都要看一眼 ——
        // 后者宁可多问一次，也不能让二进制灌进日志框。
        const bool candidate = place.role == FfmpegArgRole::OutputUrl ||
                               (place.role == FfmpegArgRole::Uncertain && place.output_side);
        if (!candidate) {
            continue;
        }
        const QString& target = arguments.at(place.index);
        if (!IsPipeOutputTarget(target)) {
            continue;
        }
        // -f null / md5 ... 这类"只解码/只算摘要"的写法写到管道上也是文本
        if (WritesTextOnly(place.output_format)) {
            continue;
        }
        return target;
    }
    return QString();
}

}  // namespace ffmpegtool
}  // namespace videoeye
