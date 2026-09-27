#include "core/ffmpeg/FfmpegCommandParser.h"

#include <QFileInfo>

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

QString DetectStdoutMediaOutput(const QStringList& arguments) {
    // `-f null -` 是常见的"只解码不出片"基准测试写法, 它不往 stdout 写任何东西,
    // 不能因为末尾有个 `-` 就误判。
    bool null_muxer = false;
    for (int i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments.at(i) == QLatin1String("-f") &&
            arguments.at(i + 1).compare(QLatin1String("null"), Qt::CaseInsensitive) == 0) {
            null_muxer = true;
        }
        if (arguments.at(i) == QLatin1String("-f") &&
            arguments.at(i + 1).startsWith(QLatin1String("pipe"), Qt::CaseInsensitive)) {
            return arguments.at(i + 1);   // -f pipe / -f pipe:1 明确走管道
        }
    }

    for (int i = 0; i < arguments.size(); ++i) {
        const QString& a = arguments.at(i);
        // 注意顺序: 裸 "-" 自己也以 '-' 开头, 必须先单独判断再跳过其它选项
        if (a.compare(QLatin1String("pipe:1"), Qt::CaseInsensitive) == 0 ||
            a.compare(QLatin1String("pipe:"), Qt::CaseInsensitive) == 0) {
            return a;
        }
        // 紧跟 -i 的 "-" 是从 stdin 读(工作台也没法喂 stdin), 末尾的 "-" 是往 stdout 写
        if (a == QLatin1String("-") && !null_muxer) {
            return a;
        }
        if (a.startsWith(QLatin1Char('-'))) {
            continue;
        }
    }
    return QString();
}

}  // namespace ffmpegtool
}  // namespace videoeye
