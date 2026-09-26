#pragma once

// 统一的单文件 QC 入口
//
// 背景：以前"分析一个文件"这件事散落在两条路径上 ——
//   UI: AnalysisCoordinator（异步，Qt 信号）→ QcRuleEngine::Evaluate
//   CLI/批量: 需要同步调用，且不能依赖 Qt 事件循环
// QcRunner 把这两步缝成一个同步接口：AnalyzeFile() 返回就一定能拿到报告，
// AnalysisCoordinator 内部那条 worker 线程在返回前已经 join 干净。
//
// 同步的实现方式是用 Qt::DirectConnection 接收 worker 线程的信号（回调跑在 worker 线程里），
// 再用条件变量等待结果 —— 因此调用方不需要 QCoreApplication，也不会拖起事件循环。
// 代价是 progress / should_cancel 回调同样在 worker 线程触发，跨线程更新 UI 前要自己投递回主线程。

#include <atomic>
#include <functional>
#include <string>

#include "core/analyzer/AnalysisCoordinator.h"
#include "core/analyzer/AnalysisTask.h"
#include "core/analyzer/QcRuleEngine.h"
#include "core/model/QcReport.h"
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
    QcRunResult AnalyzeFile(const std::string& path,
                            const QcProfile& profile,
                            analyzer::AnalysisOptions options = analyzer::AnalysisOptions{},
                            const QcRunCallbacks& callbacks = QcRunCallbacks{});

    // 生成一个绑定了 profile + options 的分析闭包（批量扫描的每个 worker 各持一份 QcRunner）
    static QcAnalyzeFn MakeAnalyzeFunction(const QcProfile& profile,
                                           analyzer::AnalysisOptions options);

    // 异步取消（批量任务的"取消"按钮最终走到这里）
    void Cancel() { cancel_.store(true, std::memory_order_release); }
    void Reset() { cancel_.store(false, std::memory_order_release); }
    bool IsCancelling() const { return cancel_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> cancel_{false};
};

// 文件是否可被分析器打开（存在且非空）。把"文件不存在""路径是目录""文件为空"分开 ——
// 报告里这三件事的处置方式是不同的，混成一句"分析失败"会让批量扫描的结果没法看。
bool IsAnalyzableFile(const std::string& path, std::string& reason);

}  // namespace qc
}  // namespace videoeye
