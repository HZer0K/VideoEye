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
#include <vector>

namespace videoeye {
namespace ffmpeg {

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
    bool program_is_path = false;  // program_token 是一个路径（而不是裸的 "ffmpeg"）
    QStringList arguments; // 传给 ffmpeg 的参数，不含程序本身
};

/// 解析一行命令文本。
/// 允许三种写法:
///   C:/tools/ffmpeg.exe -i in.mp4 out.mp4   （完整路径 —— 用它，不用设置里那个）
///   ffmpeg -i in.mp4 out.mp4                （裸程序名 —— 用设置里那个 ffmpeg）
///   -i in.mp4 out.mp4                       （省略程序名 —— 同上）
ParsedCommand ParseCommandLine(const QString& text);

/// 判断 token 是否"看起来像 ffmpeg 可执行程序"（文件名去掉 .exe 后等于 ffmpeg）。
bool LooksLikeFfmpegProgram(const QString& token);

/// 判断 token 是否是一个**显式路径**（含 / 或 \，或 ./ ../ 开头）。
/// "ffmpeg" 不是路径，"C:/tools/ffmpeg.exe" / "./ffmpeg" 才是。
bool IsExplicitProgramPath(const QString& token);

/// 这条命令是"查信息"的（-version / -encoders / -formats …）。
/// 这类命令既不需要输入也不需要输出，不能按转码命令去挑它的毛病。
bool IsInformationCommand(const QStringList& arguments);

/// 信息查询选项的一句话说明；不是已知的查询选项时返回空串。
QString InformationOptionDescription(const QString& option);

// ---- 命令结构：输入组 / 输出组 ----
//
// ffmpeg 的命令行是有结构的:
//   ffmpeg [全局选项] {[输入选项] -i 输入} ... {[输出选项] 输出} ...
// 一条命令**可以有多个输出**（例如同时出一份 mp4 和一份 m3u8），所以任何"最后一个
// 裸参数就是输出文件"的假设都是错的。这里把每个参数的角色钉下来，让管道检测与
// 命令解释共用同一份判断 —— 两处各算一遍迟早会算得不一样。

enum class FfmpegArgRole {
    Option,       // 选项本身（-crf / -y / -i ...）
    OptionValue,  // 上面那个选项的值
    InputUrl,     // -i 的值
    OutputUrl,    // 输出目标（文件、管道、rtmp:// ...）
    Uncertain,    // 归属不明（多半是字典没收录的选项后面的那个 token）
};

struct FfmpegArgPlacement {
    int index = -1;
    FfmpegArgRole role = FfmpegArgRole::Uncertain;
    int value_index = -1;        // role == Option 时: 值的下标（-1 = 这个选项不带值）
    bool output_side = false;    // 位于至少一个输入组之后（而不是"-i 之前"）
    /// 仅 -f 有意义: 它管的是**紧接着的那个输入**还是**紧接着的那个输出**。
    /// 判据是"它后面先遇到 -i 还是先遇到输出目标"，与之前出过几个输出无关 ——
    /// 输出之后继续加输入组是合法写法。
    bool applies_to_input = false;
    int output_index = -1;       // role == OutputUrl: 第几个输出（0-based）
    QString output_format;       // role == OutputUrl: 落到这个输出上的 -f 值（可能为空）
};

struct FfmpegCommandStructure {
    std::vector<FfmpegArgPlacement> args;  // 与 arguments 一一对应
    int input_count = 0;
    int output_count = 0;
    bool has_input = false;
    bool complete = true;   // false = 有 token 归属不明，别对结构下断言
};

/// 给每个参数定角色。认不出来的地方如实标 Uncertain，不做猜测。
FfmpegCommandStructure AnalyzeCommandStructure(const QStringList& arguments);

/// 参数里有没有把**媒体数据**写到标准输出/标准错误（pipe:1 / pipe:2 / - 等）。
/// 这种情况日志框会被二进制流灌满, 页面必须拦下来提示改用文件输出。
/// 返回命中的那个参数（未命中返回空串）。
///
/// 按**每个输出目标**判断，而不是看整条命令里有没有出现过某个组合:
///   * `-f null -` 是"只解码不出片"的基准写法，不产出任何数据；但同一条命令里
///     后面的 `-f matroska -` 是真的要往 stdout 写 —— 必须分别看各自生效的 -f；
///   * `pipe:2` 写的是 stderr，同样灌爆日志区，一样要拦；
///   * `-f md5` / `-f ffmetadata` 这类只产出文本的 muxer 可以放过。
QString DetectStdoutMediaOutput(const QStringList& arguments);

}  // namespace ffmpeg
}  // namespace videoeye
