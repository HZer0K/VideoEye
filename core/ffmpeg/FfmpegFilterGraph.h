#pragma once

// 把 -vf / -af / -filter_complex 的值拆成"哪几个滤镜、按什么顺序、怎么连"。
//
// 为什么值得单独做：`-vf "scale=1280:-2,unsharp=5:5"` 在解释区里原来只报第一个滤镜
// 能不能用，后面的一律不提；`-filter_complex` 更是整串当成一个黑盒。用户看不出
// 第 3 个滤镜拼错了名字、也看不出这条链其实有三个节点。
//
// 语法边界（都是 ffmpeg 自己的规则，这里只照做，不发明）：
//   - 一条链内用 ',' 分隔滤镜，滤镜参数内用 ':' 分隔；
//   - -filter_complex 里用 ';' 分隔多条链；
//   - 链首/链尾用 '[label]' 标输入输出（[0:v]、[v]、[aout]）；
//   - 参数里出现分隔符要靠引号或 '\' 转义。
//
// ⚠️ 一个已知的失真来源：上游 FfmpegCommandParser 会把命令行里的成对引号**吃掉**
// （它只负责把一行切成 token，引号不是 token 内容），所以 `select='eq(pict_type,I)'`
// 到这里的文本是 `select=eq(pict_type,I)` —— 引号没了，那个逗号看起来像滤镜分隔符。
// 处理办法不是猜，而是"切出来不像滤镜名就并回上一段"（`I)` 显然不是滤镜名），
// 并把这种合并标记为 ambiguous：拆解结果照常给，但文案里说明"参数里的逗号无法完全
// 确定，按能识别的部分拆的"。
//
// 与字典/能力缓存的关系：
//   - known   = 字典（FfmpegCommandCatalog）收录了这个滤镜，能给出中文作用；
//   - available = 当前 ffmpeg 的 `-filters` 里有它（探测之前一律按"有"）。
//     字典没收录**不是**错误（ffmpeg 有几百个滤镜），只有 available=false 才值得警告。

#include <QString>
#include <QStringList>
#include <vector>

namespace videoeye {
namespace ffmpeg {

struct FfmpegFilterNode {
    QString name;      // "scale"（已去掉 @ 实例名）
    QString instance;  // "big"（写成 scale@big 时）；没有则空
    QString arguments; // "1280:-2"；无参滤镜为空
    QString title;     // 字典里的中文名（"缩放"）；字典没收录则空
    bool known = false;
    bool available = true;
};

struct FfmpegFilterChain {
    QStringList input_labels;  // "[0:v]" 里的 "0:v"
    QStringList output_labels; // "[v]" 里的 "v"
    std::vector<FfmpegFilterNode> nodes;
};

struct FfmpegFilterGraph {
    bool complex_graph = false; // 来自 -filter_complex（允许 ';' 多链）
    bool parse_ok = true;
    bool ambiguous = false; // 有不能可靠切分的片段（见文件头的引号说明）
    QString error;          // parse_ok=false 时的原因
    QStringList notes;      // 能看懂但值得提一句的情况（如 -vf 里写了分号）
    std::vector<FfmpegFilterChain> chains;

    int NodeCount() const;
    /// 探测过能力清单时，列出「当前 ffmpeg 没有」的滤镜名（去重，保持顺序）。
    QStringList UnavailableFilters() const;
};

/// text 是选项的值（外层引号已被上游去掉）；complex_graph=true 表示来自
/// -filter_complex（允许 ';' 分多条链）。空串返回空图（parse_ok=true）。
FfmpegFilterGraph ParseFilterGraph(const QString& text, bool complex_graph);

/// 给「命令解释」的详情区用的多行文本；空图返回空串。
/// 例：
///   滤镜链拆解（2 个滤镜，按执行顺序）：
///     scale 1280:-2（缩放）→ unsharp 5:5（字典里没有这个滤镜）
QString DescribeFilterGraph(const FfmpegFilterGraph& graph);

} // namespace ffmpeg
} // namespace videoeye
