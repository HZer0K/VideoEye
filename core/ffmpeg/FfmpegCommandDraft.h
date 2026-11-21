#pragma once

// 命令草稿的小工具：往输入框里插片段、按输入路径推默认输出路径。
//
// 这两件事放在 core 而不是 UI，是因为它们有明确、可测的规则：
//   * 片段直接贴在已有 token 后面会粘成一坨（"in.mp4" + "-c:v" → "in.mp4-c:v"）；
//   * "-i \"\"" 插进去之后光标若停在引号外，用户一输入就把路径写到了引号外面；
//   * 默认输出名固定写 "_out.mp4"，对 .mkv / .ts 素材就是错的目标容器。
// UI 只负责把这里算出的文本写回 QLineEdit、把光标放到这里算出的位置。

#include <QString>

namespace videoeye {
namespace ffmpeg {

struct CommandInsertion {
    QString text;   // 插入片段之后的完整命令文本
    int cursor = 0; // 插入之后的光标位置（0..text.size()）
};

/// 把 snippet 插到 text 的 cursor 处，返回新文本与新光标。
///   * 左邻不是空白且 snippet 不以空白开头 → 补一个空格（不粘 token）；
///   * 右邻不是空白、或插入点在末尾 → snippet 后面补一个空格
///     （否则 `-c:v` 之后用户打的第一个字符会变成 `-c:vlibx264`）；
///   * snippet 以一对空引号结尾（例如 `-i ""`）→ 光标落在引号**之间**。
CommandInsertion ComposeInsertion(const QString& text, int cursor, const QString& snippet);

/// 由输入路径推一个默认输出路径：同目录、基名加 suffix、**保留源扩展名**。
/// 输入为空、或看起来是 URL（不是本地路径）时返回空串 —— 调用方据此不给对话框预填。
QString DefaultOutputPath(const QString& input_path, const QString& suffix = QStringLiteral("_out"),
                          const QString& fallback_extension = QStringLiteral("mp4"));

} // namespace ffmpeg
} // namespace videoeye
