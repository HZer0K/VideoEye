#pragma once

// 场景模板：把「常见的一整条命令」做成可一键插入的起点。
//
// 右侧字典区是**参数级**的（-crf 是什么、-map 怎么用），这里补的是**场景级**
// （"转码给微信发" / "只留音轨" / "抽帧" 到底怎么写）。两者不重复：
// 字典回答"这个选项干什么"，模板回答"整条命令长什么样"。
//
// 模板里的 argv 用 {input} / {output} 两个占位符指向输入与输出。三条规矩:
//   1. 占位符必须是**整个 token**，不能拼在别的 token 里 —— 否则路径带空格时
//      没法决定引号该加在哪儿；
//   2. 输出扩展名与"要不要序号"由模板自己声明，不靠猜输入后缀：抽帧必须写
//      `frame_%03d.png`，实测 ffmpeg 6.1 对多帧写同一个文件名会直接报
//      "Cannot write more than one file with the same name"；
//   3. 模板只给起点，不保证是用户想要的终点 —— 片段时间、CRF 这些值留成显眼的
//      默认值（focus_literal 指明插入后先选中哪一个），让用户自己改。

#include <QString>
#include <QStringList>

#include <vector>

namespace videoeye {
namespace ffmpeg {

struct FfmpegCommandTemplate {
    QString id;                      // 稳定标识（"h264"），也是默认输出名里的标记
    QString title;                   // 菜单里显示的一行标题
    QString summary;                 // 用途 + 代价，一到两句
    QStringList arguments;           // 完整 argv，含 {input} / {output}
    QString output_extension;        // "mp4" / "m4a" / "gif" / "png"
    bool output_is_sequence = false; // true → 默认输出名带 %03d
    QString focus_literal;           // 插入后希望用户先改的值（空 = 不选中）
};

/// 内置模板表（顺序即菜单顺序）。
const std::vector<FfmpegCommandTemplate>& FfmpegTemplateLibrary();

/// 按 id 找模板，找不到返回 nullptr。
const FfmpegCommandTemplate* FindFfmpegTemplate(const QString& id);

/// 由输入路径推这个模板的默认输出路径（同目录、`<基名>_<id>.<扩展名>`）。
/// output_is_sequence 时扩展名前面补 `_%03d`。
/// 输入为空、或看起来是 URL / 设备（rtmp://、lavfi）时返回空串。
QString TemplateOutputPath(const FfmpegCommandTemplate& tmpl, const QString& input_path);

/// 把占位符换成实际路径（含空格时自动加引号，保证仍是一个 token）。
/// input/output 为空时**保留占位符原文** —— 没加载素材也能把模板插进去先看着，
/// 缺什么由 MissingTemplateSlots() 告诉 UI。
QStringList InstantiateTemplate(const FfmpegCommandTemplate& tmpl, const QString& input_path,
                                const QString& output_path);

/// 参数里还没被替换掉的占位符（按出现顺序、去重）。
QStringList MissingTemplateSlots(const QStringList& arguments);

/// 拼回一行命令文本（token 之间单空格）。
QString ComposeTemplateCommand(const QStringList& arguments);

/// literal 在 text 里首次出现的下标；找不到或 literal 为空返回 -1。
/// UI 用它决定插入模板后把光标选在哪（例如截片段模板里的起始时间）。
int FindFocusToken(const QString& text, const QString& literal);

} // namespace ffmpeg
} // namespace videoeye
