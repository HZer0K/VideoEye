#include "core/qt/RepairController.h"

#include <exception>
#include <utility>

#include <QTimer>

#include "core/repair/RepairEngine.h"

namespace videoeye {
namespace qt {
namespace {

RepairController::RepairEngineRunner& TestEngineRunner() {
    static RepairController::RepairEngineRunner runner;
    return runner;
}

} // namespace

void RepairController::SetEngineRunnerForTesting(RepairEngineRunner runner) {
    TestEngineRunner() = std::move(runner);
}

RepairController::RepairController(QObject* parent) : QObject(parent), poll_timer_(new QTimer(this)) {
    poll_timer_->setInterval(kPollIntervalMs);
    connect(poll_timer_, &QTimer::timeout, this, &RepairController::Poll);
}

RepairController::~RepairController() {
    if (poll_timer_)
        poll_timer_->stop();
    tasks_.CancelAll();
    tasks_.WaitForAll(kShutdownBudgetMs);
    state_.reset();
}

quint64 RepairController::StartRepair(const model::RepairRequest& request) {
    if (IsRunning())
        return 0;

    auto state = std::make_shared<RunState>();
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_result_.reset();
    }
    state_ = state;

    model::RepairRequest owned = request;
    // 在发起任务之前取一次 runner（值捕获），避免 worker 运行期间钩子被改。
    const RepairEngineRunner runner = TestEngineRunner();

    const quint64 task_id = tasks_.RunBlockingIoWithResult(
        kSlot,
        [state, owned, runner](task::TaskId, task::CancelToken token) {
            const std::shared_ptr<std::atomic<bool>> cancel = token.flag();
            task::TaskState terminal = task::TaskState::Failed;
            // ⚠️ TaskManager 会捕获 body 抛出的异常并把任务标为 Failed，但它不认识
            // 控制器自己的 finished 标志。旧实现只在引擎正常返回且结果构造完成后
            // 才置位，于是 bad_alloc / filesystem_error 会让界面永远停在"运行中"。
            // 这里把终态收尾做成异常安全：任何路径都保证 finished 落地。
            try {
                const repair::RepairProgress progress = [state](int percent, const std::string& stage,
                                                                const std::string& detail) {
                    state->percent.store(percent, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(state->text_mutex);
                    state->stage = stage;
                    state->detail = detail;
                };
                model::RepairResult result = runner ? runner(owned, cancel.get(), progress)
                                                    : repair::RepairEngine::Run(owned, cancel.get(), progress);

                switch (result.status) {
                case model::RepairBusinessStatus::Canceled:
                    terminal = task::TaskState::Canceled;
                    break;
                case model::RepairBusinessStatus::TimedOut:
                    terminal = task::TaskState::TimedOut;
                    break;
                case model::RepairBusinessStatus::Failed:
                    terminal = task::TaskState::Failed;
                    break;
                default:
                    terminal = task::TaskState::Succeeded;
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(state->result_mutex);
                    state->result = std::make_shared<const model::RepairResult>(std::move(result));
                }
                state->finished.store(true, std::memory_order_release);
                return terminal;
            } catch (const std::exception& e) {
                // 结果对象本身也可能分配失败：内层再包一次，最后只做不抛的原子写。
                try {
                    model::RepairResult fallback;
                    fallback.status = model::RepairBusinessStatus::Failed;
                    fallback.summary = "修复任务异常终止，未能取得结果";
                    fallback.error = std::string("后台任务抛出异常: ") + e.what();
                    std::lock_guard<std::mutex> lock(state->result_mutex);
                    state->result = std::make_shared<const model::RepairResult>(std::move(fallback));
                } catch (...) {
                }
                state->finished.store(true, std::memory_order_release);
                return task::TaskState::Failed;
            } catch (...) {
                state->finished.store(true, std::memory_order_release);
                return task::TaskState::Failed;
            }
        },
        0);

    if (task_id == 0) {
        state_.reset();
        return 0;
    }

    current_id_ = task_id;
    emit RepairStarted(task_id);
    poll_timer_->start();
    return task_id;
}

void RepairController::Cancel() {
    tasks_.Cancel(kSlot);
}

bool RepairController::IsRunning() const {
    return tasks_.IsRunning(kSlot);
}

std::shared_ptr<const model::RepairResult> RepairController::result() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_result_;
}

void RepairController::Poll() {
    auto state = state_;
    if (!state)
        return;

    int percent = 0;
    std::string stage;
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(state->text_mutex);
        stage = state->stage;
        detail = state->detail;
    }
    percent = state->percent.load(std::memory_order_relaxed);
    emit ProgressReported(current_id_, percent, QString::fromStdString(stage), QString::fromStdString(detail));

    if (!state->finished.load(std::memory_order_acquire)) {
        // 兜底：线程启动失败等情形里 body 从未执行，TaskManager 自己把任务标成
        // 终态，但控制器看不到任何结果。观察 TaskManager 的终态即可收尾，
        // 不会让 Poll 无限等一个永远不来的 finished。
        const task::TaskState task_state = tasks_.State(kSlot);
        if (task::IsTerminalState(task_state)) {
            if (state->terminal_polls.fetch_add(1, std::memory_order_relaxed) + 1 >= 2)
                state->finished.store(true, std::memory_order_release);
        } else {
            state->terminal_polls.store(0, std::memory_order_relaxed);
            return;
        }
        if (!state->finished.load(std::memory_order_acquire))
            return;
    }

    poll_timer_->stop();

    std::shared_ptr<const model::RepairResult> finished;
    {
        std::lock_guard<std::mutex> lock(state->result_mutex);
        finished = state->result;
    }
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_result_ = finished;
    }

    if (finished) {
        emit RepairFinished(current_id_, QString::fromStdString(model::DisplayName(finished->status)),
                            QString::fromStdString(finished->summary));
    } else {
        emit RepairFinished(current_id_, QStringLiteral("修复失败"), QStringLiteral("任务结束但没有拿到结果"));
    }
}

} // namespace qt
} // namespace videoeye
