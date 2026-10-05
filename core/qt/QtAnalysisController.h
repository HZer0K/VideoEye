#pragma once

// 分析流程的 Qt 接线层。
//
// 它自己不做任何分析：执行在 AnalysisEngine，这里只负责
//   * 起后台线程
//   * 排队（旧线程还没退出时不让两个任务同时踏进同一个引擎）
//   * 把引擎的回调转发成 Qt 信号
//
// 任务身份 / 取消 / 终态不再由本类自己定义（评审 P1-2）：那三样统一住在
// core/domain/task/TaskProtocol.h 的 TaskHandle 与 infrastructure/concurrency/TaskManager 里，
// 本类只是「一个 slot 的其中一种用法」。整改前的四件小事各自分叉：
//   * 代际号是 generation_ 自增出来的，和"slot 上的当前任务"是两套编号；
//   * 取消有两条来源（engine_.Cancel() 与后来加的 generation_ 判断），互不通知；
//   * 终态没人记，slot 上那个任务跑完就跑完，IsRunning 得靠自己那颗布尔量兜；
//   * 旧任务回包靠手动比对代际过滤，比 TaskManager::CurrentId 的语义脆弱得多。
//
// 现在：代际号就是 slot 上的 TaskId（TaskManager 锁内分配，唯一且单调）；取消只剩令牌一条
// 来源（引擎通过 AnalysisEngine::Run 的 cancel_source 参数看它）；终态由工作线程收尾时
// EndHandle 归还，被取代的旧任务因 id 校验自然写不进去。
//
// 回调是在工作线程里激发的，信号会在接收者所在线程排队执行 —— 这也是为什么本类
// 只发信号、不直接碰界面控件。

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <QObject>
#include <QString>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/orchestration/AnalysisEngine.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/task/TaskProtocol.h"
#include "infrastructure/concurrency/TaskManager.h"

namespace videoeye {
namespace qt {

class QtAnalysisController : public QObject {
    Q_OBJECT

public:
    explicit QtAnalysisController(QObject* parent = nullptr);
    ~QtAnalysisController() override;

    // 启动一次分析，返回本次代际（= 该 slot 上的 TaskId）。
    //
    // 若上一次分析的线程还没退出来（引擎卡在不可中断的调用里），本次请求会**排队**，
    // 等旧线程真正结束再启动 —— 绝不会在调用线程（UI 线程）上 join。原来那种
    // "先 Cancel 再 join" 的写法正是"点了取消/重新扫描，界面还是冻住"的根源。
    // 返回值语义不变：排队时也立刻分配代际号，将来完成时发的就是这个号；返回 0 表示
    // 并发已满（调用方应当按失败处理）。
    quint64 StartAnalysis(const std::string& file_path,
                          const analyzer::AnalysisOptions& options = analyzer::AnalysisOptions{});

    // 请求取消当前分析（异步，结果仍会以 completed=false 返回）。排队中的请求一并作废。
    void Cancel();

    bool IsRunning() const;

    // 当前代际 = slot 上那个任务的 id。
    //
    // 故意不做成"自己自增出来的计数器"：facade 拿它给回包判代际（AnalysisFacade 里
    // `generation != coordinator.generation()` 就把旧任务的回包丢掉），而 slot 的
    // current_id 由 TaskManager 在锁内维护 —— 两者是同一个数，就不用再比一次。
    quint64 generation() const { return tasks_.CurrentId(kSlot); }

signals:
    // percent: 0..100
    void ProgressReported(quint64 generation, double percent, const QString& stage);
    void AnalysisFinished(quint64 generation, bool completed, const model::AnalysisResult& result);
    void AnalysisFailed(quint64 generation, const QString& message);

    // 内部信号：工作线程体返回时发出（由工作线程 emit，自动排队回本对象所在线程）。
    // 用来在"线程确实结束"之后接手排队请求，而不是在 UI 线程上等它结束。
    void WorkerExited(quint64 generation);

private:
    static constexpr const char* kSlot = "diagnostics-analysis";
    // 析构收尾预算：任务体必须在这段时间内响应取消令牌退出，超了就放弃（见 Shutdown）。
    static constexpr int kJoinBudgetMs = 3000;

    // 排队中的分析请求（旧线程还没退出时暂存）。handle 是它占下的那个任务 ——
    // 排队期间 slot 上已经有这个任务了，取消/关闭时必须把终态还回去，否则 slot
    // 永远停在 Running。
    struct PendingRequest {
        std::string file_path;
        analyzer::AnalysisOptions options;
        task::TaskHandle handle;
    };

    void Launch(const std::string& file_path, const analyzer::AnalysisOptions& options,
                const task::TaskHandle& handle);
    // WorkerExited 的处理体：回收已结束的工作线程并启动排队请求（不阻塞 UI，因为
    // 被 join 的线程已经返回，只剩线程收尾）。
    void OnWorkerExited(task::TaskId exited_id);

    // 把一个任务的终态还回它的 slot。只能在宿主线程调：工作线程在关闭预算耗尽时
    // 会被放弃(detach)，那时 tasks_ 比它先析构，从线程里调就是悬空访问。
    void SettleTask(const task::TaskHandle& handle, task::TaskState terminal);

    bool Alive() const;
    // 当前这个盒子里的线程体是否已经返回（UI 线程读、工作线程写那颗标志）。
    bool WorkerBodyReturned() const;
    // 等待工作线程体返回（不 join）。返回 false 表示预算内没等到。
    bool AwaitWorker(int budget_ms);
    // 回收句柄。**仅在线程体已返回时调用**，否则会在调用线程上阻塞 ——
    // 这正是本类要避免的事情，所以每个调用点都得先用 worker_finished_ 判过。
    void ReapWorker();

    // 关闭路径：作废排队请求 + 置取消令牌 + 在预算内等线程退出；超预算只能放弃。
    void Shutdown();

    // 一次分析所需的全部自持状态。工作线程按值捕获它（shared_ptr），**不捕获 this** ——
    // 与 QcRunner 同一条约定：关闭预算耗尽时任务可能被放弃(detach)，那时它得比宿主活得久。
    // 以前的写法是线程体捕获 this、直接跑成员上的 engine_，放弃路径就是悬空访问；
    // 现在引擎住在盒子里由盒子自己保活。
    struct Box {
        analyzer::AnalysisEngine engine;
        task::TaskHandle handle;
        analyzer::AnalysisOptions options;
        std::string path;
        analyzer::AnalysisCallbacks callbacks;
        std::shared_ptr<std::atomic<bool>> alive;
        // 线程体是否已经返回。以前这是个 bool 成员，工作线程与 UI 线程同时读写它 ——
        // 既是数据竞争，也让线程体必须碰到 this。挪进盒子之后，工作线程只读盒子，
        // "线程体有没有回来"这件事由盒子自己回答。
        std::atomic<bool> body_done{false};
        // 线程体收尾时通知宿主（在宿主线程里执行）。用 weak_ptr 取盒子而不是直接
        // 按值捕获，免得盒子还有人用时形成自引用环。
        std::function<void(task::TaskId)> on_body_done;
    };

    task::TaskManager tasks_{2};

    // tasks_ 全部方法都可以并发调用。engine_ 已经搬进 Box（连同一个任务的数据一起
    // 交给工作线程），这里只剩调度器。

    // 以下字段只由 UI 线程访问（StartAnalysis / Cancel / OnWorkerExited 都在该线程）。
    std::thread worker_;
    std::shared_ptr<Box> worker_box_;
    task::TaskId worker_id_ = 0;  // worker_ 身上那个任务的 id；WorkerExited 按它认领
    // 宿主还活着。工作线程 emit 前先看它 —— 关闭超预算、线程被迫放弃时，
    // 它拿到的是已经销毁的 this，没有这颗标志就一定会打上去。
    std::shared_ptr<std::atomic<bool>> alive_;
    std::optional<PendingRequest> pending_;
    bool shutting_down_ = false;
};

} // namespace qt
} // namespace videoeye
