#pragma once

// ffmpeg 失败之后的"人话翻译"。
//
// 失败时真正的原因埋在 stderr 里，而且是几十行英文：前面是 banner、输入流信息、
// 进度行，结论只有中间一两行。用户看到的界面只有「退出码 1」和一个可以往下滚的
// 文本框 —— 这一页要解决的就是"到底哪一步错了、我该改哪个参数"。
//
// 设计上刻意**不做猜测**：
//   * 只认 ffmpeg 自己会打出来的原话（钉死字符串），不做语义推断；
//   * 没命中就返回空，界面退回原来的"请看错误输出"文案 —— 编一个"可能是 XX 问题"
//     比不说更糟，用户会照着错的建议改命令；
//   * 同一条规则只出一条（ffmpeg 会把同一个错重复打好几遍，例如每个输出流各一遍）。

#include <QString>
#include <QStringList>
#include <vector>

namespace videoeye {
namespace ffmpeg {

/// 一条诊断结论。
struct FfmpegDiagnosis {
    QString id;         // 稳定标识（"missing-file" 等），用例按它断言，改文案不改语义
    QString title;      // 一句话结论
    QString evidence;   // 触发它的那句 ffmpeg 原话（已 trim，过长会截断）
    QString suggestion; // 怎么办
};

/// 扫描 stderr 全文，返回命中的诊断，按"最需要先看的在前"排序，最多 4 条。
///
/// `Conversion failed!` 这类收尾行本身不含信息（任何失败都会打），**不算命中**：
/// 只凭它推不出原因，报出来只会挤掉真正有用的那一条。
std::vector<FfmpegDiagnosis> DiagnoseFailure(const QString& stderr_text);

} // namespace ffmpeg
} // namespace videoeye
