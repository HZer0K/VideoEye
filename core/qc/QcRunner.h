#pragma once

// 统一的单文件 QC 入口
//
// 背景：以前"分析一个文件"这件事散落在两条路径上 ——
//   UI: QtAnalysisController（异步，Qt 信号）→ QcRuleEngine::Evaluate
//   CLI/批量: 需要同步调用，且不能依赖 Qt 事件循环
// QcRunner 把这两步缝成一个同步接口：AnalyzeFile() 返回就一定能拿到报告，
// AnalyzeFile 内部那条 worker 线程在返回前已经 join 干净。
//
// 实现方式：AnalyzeFile 起一条 worker 线程跑 AnalysisEngine（不依赖 QObject），
// 用条件变量等待结果 —— 调用方不需要 QCoreApplication，也不会拖起事件循环。
// 代价是 progress / should_cancel 回调同样在 worker 线程触发，
// 跨线程更新 UI 前要自己投递回主线程。

#include <atomic>
#include <functional>
#include <string>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/diagnostics/QcRuleEngine.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/task/TaskProtocol.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcProfile.h"
#include "infrastructure/concurrency/TaskManager.h"

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
                            analyzer::AnalysisOptions options = analyzer::AnalysisOptions{},
                            const QcRunCallbacks& callbacks = QcRunCallbacks{},
                            int join_budget_ms = kDefaultJoinBudgetMs);

    // 生成一个绑定了 profile + options 的分析闭包（批量扫描的每个 worker 各持一份 QcRunner）
    static QcAnalyzeFn MakeAnalyzeFunction(const QcProfile& profile,
                                           analyzer::AnalysisOptions options);

    // 异步取消（批量任务的"取消"按钮最终走到这里）。
    //
    // 这里只剩这一个取消源: 过去 QcRunner 自己还有一枚 cancel_ 原子标志, 是"任务外"
    // 的一层, 与分析内部的取消令牌各走各的; 现在取消统一落到 TaskManager 的 slot 令牌上,
    // 任务体轮询的就是它 —— 一条链路上不再有第二枚标志。
    void Cancel() { tasks_.Cancel(kSlot); }
    bool IsCancelling() const { return tasks_.Token(kSlot).IsCanceled(); }

    static constexpr int kDefaultJoinBudgetMs = 30000;

private:
    // 每个 QcRunner 一条 slot: 分析线程归它管, 终态与取消也都记在这条 slot 上。
    // 令牌每次 Begin 换一份, 所以这里不缓存, 要取就现取 IsCancelling()。
    static constexpr const char* kSlot = "qc-runner";

    task::TaskManager tasks_{2};
};

// 文件是否可被分析器打开（存在且非空）。把"文件不存在""路径是目录""文件为空"分开 ——
// 报告里这三件事的处置方式是不同的，混成一句"分析失败"会让批量扫描的结果没法看。
bool IsAnalyzableFile(const std::string& path, std::string& reason);

}  // namespace qc
}  // namespace videoeye
