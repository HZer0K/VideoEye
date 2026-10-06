#pragma once

// 统一的单文件 QC 入口
//
// 背景：以前"分析一个文件"这件事散落在两条路径上 ——
//   UI: QtAnalysisController（异步，Qt 信号）→ QcRuleEngine::Evaluate
//   CLI/批量: 需要同步调用，且不能依赖 Qt 事件循环
// QcRunner 把这两步缝成一个同步接口：AnalyzeFile() 返回就一定能拿到报告。
//
// 职责边界（评审 P1-2 收敛的结果）：QcRunner 是**无状态同步执行器** —— 不持有任务
// 调度器，并发完全由调用方（BatchQcRunner / ReportingPanel）决定与限制。
// 以前它自己持有一个 TaskManager{2}，而批量扫描里每个分析请求都新建一个 QcRunner，
// 于是"并发上限 2"变成"每个 QcRunner 两个"，全局上限根本没生效；且被 TaskManager 拒绝
// （handle.valid()==false）之后代码仍会创建线程，"拒绝"形同虚设。现在这两件事都归
// 调用方的调度器：没有内部 slot，也就没有"未获准仍启动线程"这条路径。
//
// 取消入口只剩一个：AnalyzeFile 的 callbacks.should_cancel。它被接进引擎的
// cancel_source（FFmpeg 中断回调也能看见）—— 一条链路上只有这一颗标志。
//
// 实现方式：AnalyzeFile 起一条 worker 线程跑 AnalysisEngine（不依赖 QObject），
// 用条件变量等待结果 —— 调用方不需要 QCoreApplication，也不会拖起事件循环。
// worker 只按值捕获自持有的 shared state（Box，内含引擎本体），所以超预算时可以安全
// detach：没有栈引用、也没有"宿主局部 shared_ptr"可悬空。
// 代价是 progress / should_cancel 回调同样在 worker 线程触发，
// 跨线程更新 UI 前要自己投递回主线程。

#include <functional>
#include <string>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/diagnostics/QcRuleEngine.h"
#include "core/domain/model/QcReport.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcProfile.h"

namespace videoeye {
namespace qc {

// 分析过程中的回调。两者都在后台线程执行。
struct QcRunCallbacks {
    std::function<void(double percent, const std::string& stage)> progress;
    std::function<bool()> should_cancel;
};

class QcRunner {
public:
    QcRunner() = default;
    ~QcRunner() = default;

    QcRunner(const QcRunner&) = delete;
    QcRunner& operator=(const QcRunner&) = delete;

    // 统一入口：分析结果 = AnalyzeFile(path, profile, options)
    //
    // options 由调用方给出时按调用方的来（UI 有自己的开关面板）；批量/CLI 场景通常
    // 直接传 OptionsForDepth(profile.depth)。
    //
    // join_budget_ms 是**这次分析本身的等待预算**: 到点还没出结果就按失败收尾,
    // 工作线程转入受控回收(不再无限等)。以前这里是无条件 join —— 引擎卡在第三方 IO
    // 上时整条批量扫描会永久挂死, 而"有一份结果"和"卡死"之间只有这个预算可区分。
    QcRunResult AnalyzeFile(const std::string& path,
                            const QcProfile& profile,
                            videoeye::AnalysisOptions options = videoeye::AnalysisOptions{},
                            const QcRunCallbacks& callbacks = QcRunCallbacks{},
                            int join_budget_ms = kDefaultJoinBudgetMs);

    // 生成一个绑定了 profile + options 的分析闭包（批量扫描的每个 worker 各持一份 QcRunner）
    static QcAnalyzeFn MakeAnalyzeFunction(const QcProfile& profile,
                                           videoeye::AnalysisOptions options);

    static constexpr int kDefaultJoinBudgetMs = 30000;
};

// 文件是否可被分析器打开（存在且非空）。把"文件不存在""路径是目录""文件为空"分开 ——
// 报告里这三件事的处置方式是不同的，混成一句"分析失败"会让批量扫描的结果没法看。
bool IsAnalyzableFile(const std::string& path, std::string& reason);

}  // namespace qc
}  // namespace videoeye