#pragma once

// 运行前的本地路径预检。
//
// 这一类错误**不必等 ffmpeg 启动**就能看出来：输入文件拼错、输出目录还没建、
// 输出和输入写成了同一个文件。放任它跑，用户要先等 ffmpeg 打完 banner 和流信息
// 才看到 `No such file or directory`，中间那段解码白等了。
//
// 边界（宁可少报，不可误报）：
//   * 只看**本地文件路径**：URL（http/rtmp/rtsp…）、管道（`-` / `pipe:`）、
//     协议前缀（concat:/subfile:…）一律放过 —— 它们本来就"不存在于磁盘"；
//   * 判断不了命令结构（有选项不在字典里，后面那个 token 归属不明）时**整体跳过**：
//     拿猜出来的路径去拦用户，比不拦更糟；
//   * 不判可写性与剩余空间：QFileInfo 在 Windows 上对 ACL / 网络盘经常谎报，
//     误报会把本来能跑的命令挡下来。这类失败让 ffmpeg 自己报，再由
//     DiagnoseFailure 翻译成"没有权限 / 磁盘满"。

#include <QString>
#include <QStringList>
#include <vector>

namespace videoeye {
namespace ffmpeg {

enum class PreflightSeverity {
    Error,   // 按现在的命令几乎必然失败
    Warning, // 可能失败，但仍值得让用户知道
};

struct PreflightIssue {
    PreflightSeverity severity = PreflightSeverity::Error;
    QString id;      // "input-missing" / "output-dir-missing" / "same-as-input" / "output-is-directory"
    QString message; // 给用户看的一句话（含路径）
    QString path;    // 涉及的路径（便于界面做二次处理）
};

/// 检查这条命令的输入 / 输出路径。没有问题返回空。
std::vector<PreflightIssue> PreflightCommand(const QStringList& arguments);

/// 这个 token 是不是"磁盘上的本地文件路径"。
/// false 的典型值：`-`、`pipe:1`、`http://…`、`concat:a.txt|b.txt`、`C:\a.mp4` 里的 `C:` 不算协议。
bool IsLocalPathTarget(const QString& target);

} // namespace ffmpeg
} // namespace videoeye
