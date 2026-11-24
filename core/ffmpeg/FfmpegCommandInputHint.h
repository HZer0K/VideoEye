#pragma once

// 边打边提示：光标停在命令行的哪个位置，就回答两件事 ——
//   1. **现在这个词是干什么的**（"视频质量 (CRF)"、"输入文件"…）；
//   2. **下一个词该写什么**（该填值 / 该写输出选项 / 该写输出文件名），
//      并给出可直接补全的候选（按已输入的前缀过滤）。
//
// 为什么不能直接复用 FfmpegCommandExplainer:
//   解释器面向**已经写完的一条命令**，靠 AnalyzeCommandStructure 给每个 token 定角色。
//   输入到一半时那份结构并不成立：`-crf` 后面还没写值、`-scale=1280` 还在打、光标停在
//   空白处等新词。拿它去判"光标处属于哪一段"会把半截选项名当成未知选项，再把后面
//   的裸词吞成它的值。这里的判据必须**容忍未完成**：词写到一半也要能指出"这是选项名，
//   接着该填值"，而不是等整条命令写完。
//
// 与解释器的分工（两边不能各算一套结论）:
//   * 词条文案（title / summary / detail / position）一律来自 FfmpegCommandCatalog；
//   * 值清单（编码器 / 滤镜 / 格式）一律来自 FfmpegCapabilityCache —— 它由本机
//     `ffmpeg -encoders` 之类探测出来，内置表不回答"当前这份 ffmpeg 支持什么"；
//   * 本模块只新增"位置判断 + 候选 + 下一步文案"这三件事。
//
// 关于"值不能以 - 开头"：与解析器保持同一条口径 —— 只有字典标了
// value_may_start_with_dash 的选项（-map / -map_metadata）才把 `-0:d` 当值。

#include <QString>
#include <QStringList>

#include <vector>

namespace videoeye {
namespace ffmpeg {

/// 光标所在词在命令结构里扮演的角色。
enum class FfmpegInputSlot {
    Program,     // 第一个词：ffmpeg 程序（或完整路径）
    Option,      // 选项名（-crf / -c:v）
    OptionValue, // 某个已知选项的值（-crf 23 里的 23）
    InputUrl,    // -i 的路径（含"忘了写 -i"的裸路径，见 missing_dash_i）
    OutputUrl,   // 输出目标
};

/// 下拉里的一条候选。
struct FfmpegInputCandidate {
    QString text;  // 接受后写进命令的文本（原子片段，不带尾随空格）
    QString label; // 主行：选项名 / 值
    QString note;  // 副行：一句话说明（来自字典；值候选多为空）
    /// 接受后光标停在原位等用户填值（选项名类候选都是 true）
    bool expects_value = false;
    /// 接受后要写成"新词"，不是在光标处替换当前词。
    /// 典型场景：`-c` 这类选项已经打完、光标还停在这个选项上，候选给的是它的**值**
    /// （copy / libx264 …）—— 那时替换会把 `-c` 本身吃掉，只剩一个 `copy`。
    bool starts_new_word = false;
};

/// 光标处的完整判断结果。UI 只负责排版，不再自己判定位置。
struct FfmpegInputHint {
    bool missing_dash_i = false; // 光标处的裸词其实是输入文件，但前面没有 -i
    QString problem;             // 非空 = 有先说清楚的问题（引号未闭合之类）

    FfmpegInputSlot slot = FfmpegInputSlot::Option;
    QString word; // 光标所在词的原文（含引号；停在空白处时为空）
    int word_begin = 0;
    int word_end = 0;
    QString prefix; // 光标之前那一段（前缀补全用）

    // ---- 当前词的说明（光标在空白处时，说明刚打完的那个词）----
    bool has_subject = false;
    QString subject;          // "-crf" / "libx264" / "D:/a.mp4"
    QString subject_title;    // "视频质量 (CRF)" / "输入文件"
    QString subject_summary;  // 一句话作用
    QString subject_detail;   // 详细说明
    QString subject_position; // 该放在命令的哪个位置
    QString subject_example;
    bool subject_known = false;     // 字典里查得到（或能确定是路径）
    bool subject_is_prefix = false; // 只是前缀（"你可能想写 -crf"）

    // ---- 下一步 ----
    QString next_label;        // 一句话：这个位置该填什么
    QStringList next_examples; // 可直接接上的片段（'-crf'、'23'…），给"下一个字符"用

    /// 候选上限固定（避免把几百个编码器全倒出来），按前缀匹配 + 常用优先排序。
    std::vector<FfmpegInputCandidate> candidates;
};

/// 帮光标（0..text.size()）算出提示。text 是命令框的**整行文本**。
FfmpegInputHint BuildInputHint(const QString& text, int cursor);

/// 用候选替换当前词 [word_begin, word_end)，返回新文本与新光标。
///
/// 之所以单独成函数：替换要把"词边界"和"空格补齐"两件事同时做对 ——
/// 直接 insert 会把 `-crf` 变成 `-crf-crf`；只 replace 不加尾随空格，用户接着打的
/// 第一个字符会粘成 `-crf23`。规则与 ComposeInsertion 保持一致（复用它的空格判断）。
struct FfmpegCompletionEdit {
    QString text;
    int cursor = 0;
};
FfmpegCompletionEdit ApplyCompletion(const QString& text, int word_begin, int word_end, const QString& replacement);

} // namespace ffmpeg
} // namespace videoeye
