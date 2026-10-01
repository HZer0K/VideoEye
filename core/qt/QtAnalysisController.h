#pragma once

// 分析流程的 Qt 接线层。
//
// 它自己不做任何分析：执行在 AnalysisEngine，这里只负责
//   * 起后台线程
//   * generation 机制（快速切换文件时丢弃旧任务的回包）
//   * 把引擎的回调转发成 Qt 信号
//
// 回调是在工作线程里激发的，信号会在接收者所在线程排队执行 —— 这也是为什么本类
// 只发信号、不直接碰界面控件。

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <QObject>
#include <QString>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/AnalysisResult.h"
#include "core/analysis/orchestration/AnalysisEngine.h"

namespace videoeye {
namespace qt {

class QtAnalysisController : public QObject {
    Q_OBJECT

public:
    explicit QtAnalysisController(QObject* parent = nullptr);
    ~QtAnalysisController() override;

    // 启动一次分析，返回本次 generation。
    //
    // 若上一次分析的线程还没退出来（引擎卡在不可中断的调用里），本次请求会**排队**，
    // 等旧线程真正结束再启动 —— 绝不会在调用线程（UI 线程）上 join。原来那种
    // "先 Cancel 再 join" 的写法正是"点了取消/重新扫描，界面还是冻住"的根源。
    // 返回值语义不变：排队时也立刻分配代际号，将来完成时发的就是这个号。
    quint64 StartAnalysis(const std::string& file_path,
                          const analyzer::AnalysisOptions& options = analyzer::AnalysisOptions{});

    // 请求取消当前分析（异步，结果仍会以 completed=false 返回）。排队中的请求一并作废。
    void Cancel();

    bool IsRunning() const;

    quint64 generation() const { return generation_.load(std::memory_order_acquire); }

signals:
    // percent: 0..100
    void ProgressReported(quint64 generation, double percent, const QString& stage);
    void AnalysisFinished(quint64 generation, bool completed, const analyzer::AnalysisResult& result);
    void AnalysisFailed(quint64 generation, const QString& message);

    // 内部信号：工作线程体返回时发出（由工作线程 emit，自动排队回本对象所在线程）。
    // 用来在"线程确实结束"之后接手排队请求，而不是在 UI 线程上等它结束。
    void WorkerExited(quint64 generation);

private:
    // 排队中的分析请求（旧线程还没退出时暂存）。
    struct PendingRequest {
        std::string file_path;
        analyzer::AnalysisOptions options;
        quint64 generation = 0;
    };

    void Launch(const std::string& file_path, const analyzer::AnalysisOptions& options, quint64 gen);
    // WorkerExited 的处理体：join 已结束的旧线程并启动排队请求（不阻塞 UI，因为
    // 被 join 的线程已经返回，只剩线程收尾）。
    void OnWorkerExited(quint64 finished_generation);

    std::atomic<bool> running_{false};
    std::atomic<quint64> generation_{0};
    // 正在跑的那个线程对应的代际号，用来忽略迟到的 WorkerExited。
    std::atomic<quint64> worker_generation_{0};
    std::thread worker_;
    analyzer::AnalysisEngine engine_;

    // 只在 UI 线程访问（StartAnalysis / Cancel / OnWorkerExited 都在该线程）。
    std::optional<PendingRequest> pending_;
    bool shutting_down_ = false;
};

} // namespace qt
} // namespace videoeye
