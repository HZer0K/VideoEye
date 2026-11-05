#include "core/qt/RepairController.h"

#include <QTimer>

#include "core/repair/RepairEngine.h"

namespace videoeye {
namespace qt {

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

    const quint64 task_id = tasks_.RunBlockingIoWithResult(
        kSlot,
        [state, owned](task::TaskId, task::CancelToken token) {
            const std::shared_ptr<std::atomic<bool>> cancel = token.flag();
            model::RepairResult result = repair::RepairEngine::Run(
                owned, cancel.get(), [state](int percent, const std::string& stage, const std::string& detail) {
                    state->percent.store(percent, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(state->text_mutex);
                    state->stage = stage;
                    state->detail = detail;
                });

            task::TaskState terminal = task::TaskState::Succeeded;
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

    if (!state->finished.load(std::memory_order_acquire))
        return;

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
