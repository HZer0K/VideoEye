#pragma once

// 运行结束后「这次到底产出了哪些文件」。
//
// 看起来只是读一遍参数，其实只有一个真难点：**序号模板**。抽帧的
// `out_%03d.png` 指向的是一族文件，不是一条路径 —— 既不能直接 QFileInfo::exists，
// 也不能拿它去打开（拿模板原文去开只会得到"文件不存在"）。
// 这里把它展开成第一张实际存在的帧，并把"声明了但没产出"单独列出来：
// 跑完却没产出是最需要告诉用户的状态，不能和"命令里没有本地输出"混为一谈。
//
// 判断口径：
//   * 只认本地文件目标：URL（含 `://`）、`pipe:`、`-`（stdout）一律不算；
//   * 存在性用 QFileInfo 实测，不猜；
//   * 序号模板只探第 1 张（ffmpeg 默认从 1 开始，`-start_number` 改过就探不到，
//     这种情况会落到 missing 里，界面文案要说"没找到"，不能假装成功）。

#include <QString>
#include <QStringList>

namespace videoeye {
namespace ffmpeg {

struct FfmpegRunOutputs {
    QStringList files;         // 磁盘上确实存在、可以直接打开的文件（序号模板取第一张）
    QStringList missing;       // 命令里声明的本地输出、但磁盘上没有（序号模板保留原文）
    bool has_sequence = false; // 有输出是图片序列：打开时只能给第一帧
};

/// 按命令参数收集本次运行的产出。顺序与命令里的输出顺序一致。
FfmpegRunOutputs CollectRunOutputs(const QStringList& arguments);

/// 路径里带 `%d` / `%03d` 这类序号模式（ffmpeg 的 image2 语法）。
bool HasSequencePattern(const QString& path);

/// 把序号模式按 index 展开：`out_%03d.png` + 1 → `out_001.png`；`%d` + 12 → `12`。
/// 没有模式的路径原样返回；模式出现多次时一起换。
QString ExpandSequencePattern(const QString& path, int index);

/// 「打开输出目录」该去哪个目录：先看真正产出的文件，再退回"声明过但没产出"的父目录。
/// 都没有（或都是 URL）返回空串 —— 调用方据此禁用按钮，而不是打开一个瞎猜的目录。
QString RevealDirectory(const FfmpegRunOutputs& outputs);

} // namespace ffmpeg
} // namespace videoeye
