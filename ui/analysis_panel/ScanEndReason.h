#pragma once

// 一次全文件扫描的终态。
//
// 单独成头的原因：这个枚举出现在 DiagnosticsPage 的公共信号与 AnalysisPanel 的
// 公共 slot 签名里。以前它定义在 DiagnosticsPage 类内，于是任何只用到"扫描结束了"
// 这一事实的调用方都必须 include 整个 DiagnosticsPage.h（连同它的子视图、Qt 控件
// 与 AnalysisFacade）。提到命名空间级之后，面板头可以前向声明页面，不再为了一个
// 三值枚举扇出整棵页面头树。
//
// Q_NAMESPACE + Q_ENUM_NS 保留原来的反射能力（QMetaEnum / 队列连接的类型注册）；
// DiagnosticsPage 内部用 `using ScanEndReason = videoeye::ui::ScanEndReason;`
// 维持既有的 `DiagnosticsPage::ScanEndReason` 写法。

#include <QObject>

namespace videoeye {
namespace ui {

Q_NAMESPACE

enum class ScanEndReason {
    Completed, // 正常跑完（含用户中途取消后回包 completed=false 的情形见 Cancelled）
    Cancelled, // 被取消（结果不完整，但可用）
    Failed,    // 打开 / 探测 / 读取失败，没有可用结果
};
Q_ENUM_NS(ScanEndReason)

} // namespace ui
} // namespace videoeye
