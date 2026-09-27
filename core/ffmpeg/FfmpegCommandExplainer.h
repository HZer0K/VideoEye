#pragma once

// 把一条 ffmpeg 命令拆成"这一段在干什么"。
//
// ffmpeg 的参数是**位置敏感**的，所以解释必须按顺序走一遍 token，而不是做字典查找:
//   ffmpeg -i a.mp4 -c:v libx264 out.mp4
//   → 输入文件 a.mp4 → 视频编码器 libx264 → 输出文件 out.mp4
//
// 关于"未知参数": 认不出来的参数**必须如实说认不出来**。字典只收了高频选项，
// ffmpeg 的私有选项（-x264-params 之类）和私有编码器参数有几百个，编不出来很正常；
// 编一个看似合理的解释比不解释更糟。

#include <QString>
#include <QStringList>
#include <vector>

namespace videoeye {
namespace ffmpegtool {

enum class FfmpegTokenRole {
    InputFile,      // -i 的值
    OutputFile,     // 命令末尾的落盘目标
    KnownOption,    // 字典里有的选项
    UnknownOption,  // 字典里没有的选项
    BareArgument,   // 位置不明的裸露参数（滤镜标签 [out]、URL 之类）
};

struct FfmpegTokenExplanation {
    QString token;              // "-crf"
    QString value;              // "23"（无值选项为空）
    FfmpegTokenRole role = FfmpegTokenRole::UnknownOption;
    QString role_label;         // "视频质量 (CRF)" / "输入文件" / "未知参数"
    QString description;        // 这一项在干什么
    QString position;           // 该放在命令的哪个位置
    QString example;
    QStringList related;
    QStringList typical_values;
    bool known = false;
    QString warning;            // 空表示没问题（比如"当前 ffmpeg 里没有这个编码器"）
};

struct FfmpegCommandExplanation {
    QStringList arguments;
    QString flow;                              // "输入文件 → 视频质量 (CRF) → 输出文件"
    std::vector<FfmpegTokenExplanation> tokens;
    QStringList notes;                         // 命令级的提醒
    bool has_input = false;
    bool has_output = false;
};

class FfmpegCommandExplainer {
public:
    static FfmpegCommandExplanation Explain(const QStringList& arguments);
};

}  // namespace ffmpegtool
}  // namespace videoeye
