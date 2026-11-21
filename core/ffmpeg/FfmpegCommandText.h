#pragma once

// 一条命令的两种排版：单行文本 ↔ 便于阅读的多行。
//
// 为什么需要：命令一长（多路 -map、长滤镜串、七八个编码参数）挤在一个 QLineEdit 里
// 就没法看了，改一个值要在横向滚动条上来回找。多行编辑器解决的是"看得清"，
// 但**执行前必须压回一行** —— ffmpeg 命令行没有行继续符，换行就是参数分隔符，
// 直接把多行文本喂给 QProcess 会多出空参数。
//
// 因此本模块的契约是: FlattenCommandLines(WrapIntoLines(text)) 与 text 等价
// （空白规范化到单个空格）。实现上所有切分都在**原文区间**上做，不经过
// ParseCommandLine 去引号的结果 —— 拿剥掉引号的 token 拼回去，含空格的路径
// 会被拆成两个参数，这是最容易踩的坑。

#include <QString>
#include <QStringList>
#include <vector>

namespace videoeye {
namespace ffmpeg {

/// 一个词在原文里的区间 [begin, end)。引号属于词的一部分（不剥掉）。
struct FfmpegCommandWord {
    int begin = 0;
    int end = 0;
};

/// 按命令行的规则切词，返回原文区间。口径与 ParseCommandLine 一致:
/// 空白分隔、成对引号内的空白不切、没有反斜杠转义。
/// 引号未闭合时，词一直延续到行尾（不报错 —— 报错是解析器的活）。
std::vector<FfmpegCommandWord> SplitCommandWords(const QString& text);

/// 单行命令 → 多行排版：程序名一行，之后每个「选项 + 它的值」一行，
/// 裸的参数（输入 / 输出路径）各自一行。
/// 只影响显示，不改任何字符（引号原样保留）。
QStringList WrapCommandIntoLines(const QString& text);

/// 多行排版 → 单行命令：每行 trim、丢掉空行、用单个空格连接。
/// 行内空白不动（引号里可能有连续空格，压掉就改写了路径）。
QString FlattenCommandLines(const QString& text);

} // namespace ffmpeg
} // namespace videoeye
