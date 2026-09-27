#pragma once

// 命令行文本 → 参数数组。
//
// 「FFmpeg 命令工作台」**不把命令交给系统 shell**: 页面上的一行文本先在这里被切成一个
// 个 token, 再交给 QProcess 以 (program, QStringList) 的形式启动。理由有三:
//   1. 安全: `|` `&&` `>` 这类 shell 元字符不会被解释, 用户粘进来什么就只可能是参数;
//   2. 可移植: Windows 上没有 /bin/sh, 走 shell 等于给自己找两套行为;
//   3. 路径: 含空格 / 中文的路径只要加了引号就能原样保留, 不需要转义规则。
//
// 这里刻意**不把反斜杠当转义字符** —— Windows 路径处处是 `\`, 一旦照搬 POSIX 规则,
// `D:\media\new\a.mp4` 会被吃掉反斜杠变成 `D:medianewa.mp4`。

#include <QString>
#include <QStringList>

namespace videoeye {
namespace ffmpegtool {

enum class CommandParseStatus {
    Ok,                 // 解析成功
    Empty,              // 空命令
    UnterminatedQuote,  // 引号没闭合
    ShellOperator,      // 出现了 | && || ; > < 等 shell 操作符
    UnsupportedProgram  // 第一个 token 不是 ffmpeg
};

struct ParsedCommand {
    CommandParseStatus status = CommandParseStatus::Empty;
    QString message;       // 失败时给用户的说明（成功时为空）
    QString program_token; // 命令里写的可执行程序 token（可能是 "ffmpeg" 或完整路径）
    QStringList arguments; // 传给 ffmpeg 的参数，不含程序本身
};

/// 解析一行命令文本。
/// 允许两种写法:
///   ffmpeg -i in.mp4 out.mp4     （带程序名，程序名会被丢弃，实际用的是设置里的 ffmpeg）
///   -i in.mp4 out.mp4            （省略程序名）
ParsedCommand ParseCommandLine(const QString& text);

/// 判断 token 是否"看起来像 ffmpeg 可执行程序"（文件名去掉 .exe 后等于 ffmpeg）。
bool LooksLikeFfmpegProgram(const QString& token);

/// 参数里是否把媒体数据写到了 stdout（pipe:1 / - 等）。
/// 这种情况日志框会被二进制流灌满, 页面必须拦下来提示改用文件输出。
/// 返回命中的那个参数（未命中返回空串）。
QString DetectStdoutMediaOutput(const QStringList& arguments);

}  // namespace ffmpegtool
}  // namespace videoeye
