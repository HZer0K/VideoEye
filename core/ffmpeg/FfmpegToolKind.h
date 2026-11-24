#pragma once

// 「命令工作台」能执行的不止 ffmpeg 一个程序。
//
// FFmpeg 项目一次发布三个可执行文件，命令行语法**互不相同**：
//   ffmpeg  —— 转码/处理。必须显式写 `-i 输入`，且必须有输出目标。
//   ffprobe —— 查媒体信息。直接把文件写在裸参数位，没有 -i，也没有输出文件。
//   ffplay  —— 极简播放器。同样把文件写在裸参数位，弹出窗口播放，不产出文件。
//
// 为什么要把这件事显式建模，而不是到处写 `token == "ffmpeg"`：
//   拿 ffmpeg 的语法去套 ffprobe 命令，`D:/a.mp4` 会被判成"缺 -i 的输入"、
//   然后整条命令被说成"没有输出文件"—— 全是错的。工具种类必须先定下来，
//   下游（字典、输入提示、预检、进度）才能各自按正确的语法工作。
//
// 判据只有一处：ClassifyProgramToken。别在业务层重抄"取文件名去 .exe"。

#include <QString>

namespace videoeye {
namespace ffmpeg {

enum class FfmpegToolKind {
    Unknown = 0, // 不是这三者之一，或者还没写程序名（省略程序名的写法）
    Ffmpeg,      // ffmpeg
    Ffprobe,     // ffprobe
    Ffplay,      // ffplay
};

/// 工具集合的位掩码。字典条目要回答"这个选项归哪些工具"，答案天然是集合
/// （`-hide_banner` 三个都认，`-vf` 只 ffmpeg 与 ffplay 认），用位掩码一次说清。
enum FfmpegToolMask : unsigned {
    kToolNone = 0u,
    kToolFfmpeg = 1u << 0,
    kToolFfprobe = 1u << 1,
    kToolFfplay = 1u << 2,
    kToolAll = kToolFfmpeg | kToolFfprobe | kToolFfplay,
};

/// 单个工具 → 它自己的那一位。Unknown 返回 kToolNone。
unsigned ToolMaskOf(FfmpegToolKind kind);

/// 从命令的第一个词判工具。
/// 取文件名、去掉 `.exe`、大小写不敏感：`ffprobe` / `FFPROBE.EXE` /
/// `C:/tools/ffmpeg-8.1/bin/ffprobe.exe` 都是 Ffprobe。
/// 空串、选项（"-i"）、以及别的程序一律 Unknown。
FfmpegToolKind ClassifyProgramToken(const QString& token);

/// 可执行文件名（无平台后缀）："ffmpeg" / "ffprobe" / "ffplay"；Unknown 返回空串。
/// 找程序、拼命令行抬头都用它，别再手写字符串字面量。
QString ToolProgramName(FfmpegToolKind kind);

/// 界面上的用途说明（"转码 / 处理"、"查询媒体信息"…）。
QString ToolPurposeText(FfmpegToolKind kind);

/// 这个工具的裸参数（不带任何选项）默认就是输入媒体吗。
/// ffmpeg 为 false —— 它要求显式 `-i`，裸路径会被当成输出目标或"忘了写 -i"；
/// ffprobe / ffplay 为 true —— `ffprobe a.mp4` 就是合法写法。
bool ToolTakesBareInput(FfmpegToolKind kind);

/// 这个工具会不会往磁盘写媒体文件。
/// 只有 ffmpeg 会。ffprobe 只打印信息，ffplay 只往窗口里画 ——
/// 输出目录预检、覆盖确认、进度跟踪都只对"会写文件"的工具成立。
bool ToolProducesOutputFile(FfmpegToolKind kind);

} // namespace ffmpeg
} // namespace videoeye
