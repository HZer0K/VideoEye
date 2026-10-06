#pragma once

// 把一条 ffmpeg 命令拆成"这一段在干什么"。
//
// ffmpeg 的参数是**位置敏感**的，所以解释必须按顺序走一遍 token，而不是做字典查找:
//   ffmpeg -i a.mp4 -c:v libx264 out.mp4
//   → 输入文件 a.mp4 → 视频编码器 libx264 → 输出文件 out.mp4
//
// 一条命令可以有**多个输出**（同时出 mp4 和切片、-i a out1 -i b out2 之类），
// 所以这里按 FfmpegCommandParser 给出的"输入组 / 输出组"结构走，不再假设
// "最后一个裸参数就是唯一的输出文件"。结构确定不了的 token 一律标 Unresolved：
// 宁可说"未解析"，也不给一条看起来合理但错的纠正建议。
//
// 关于"未知参数": 认不出来的参数**必须如实说认不出来**。字典只收了高频选项，
// ffmpeg 的私有选项（-x264-params 之类）和私有编码器参数有几百个，编不出来很正常；
// 编一个看似合理的解释比不解释更糟。

#include <QString>
#include <QStringList>
#include <vector>

namespace videoeye {
namespace ffmpeg {

enum class FfmpegTokenRole {
    InputFile,      // -i 的值
    OutputFile,     // 输出目标（一条命令可以有多个）
    KnownOption,    // 字典里有的选项
    UnknownOption,  // 字典里没有的选项
    Unresolved,     // 结构不明：认不出它是某个选项的值还是输出文件，不做猜测
};

struct FfmpegTokenExplanation {
    QString token;              // "-crf"
    QString value;              // "23"（无值选项为空）
    FfmpegTokenRole role = FfmpegTokenRole::UnknownOption;
    QString role_label;         // "视频质量 (CRF)" / "输入文件" / "未解析"
    QString description;        // 这一项在干什么
    QString position;           // 该放在命令的哪个位置
    QString example;
    QStringList related;
    QStringList typical_values;
    bool known = false;
    int group_index = -1;       // OutputFile: 第几个输出（0-based）
    QString warning;            // 空表示没问题（比如"当前 ffmpeg 里没有这个编码器"）
};

struct FfmpegCommandExplanation {
    QStringList arguments;
    QString flow;                              // "输入文件 → 视频质量 (CRF) → 输出文件"
    std::vector<FfmpegTokenExplanation> tokens;
    QStringList notes;                         // 命令级的提醒
    bool has_input = false;
    bool has_output = false;
    int input_count = 0;
    int output_count = 0;
    /// false = 有 token 归属不明（tokens 里对应项是 Unresolved），别对结构下断言
    bool structure_known = true;
};

class FfmpegCommandExplainer {
public:
    static FfmpegCommandExplanation Explain(const QStringList& arguments);
};

}  // namespace ffmpeg
}  // namespace videoeye
