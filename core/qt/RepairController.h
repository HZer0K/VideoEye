#pragma once

// 修复流程的 Qt 接线层。
//
// 只做三件事：起后台任务（TaskManager 的 "video-repair" slot）、把引擎进度转成信号、
// 持有结果快照。它不写报告 —— core/qt 不允许依赖 core/reporting，报告由界面在拿到
// 结果后调用 RepairReportExporter 生成。
//
// 进度不是从工作线程直接 emit 的：工作线程只把数值写进一个线程安全的 RunState，
// 由 QTimer 在宿主线程轮询后 emit。这样即使关闭预算耗尽、任务被 detach，工作线程
// 也绝不会碰已经销毁的 QObject。

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <QObject>
#include <QString>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/domain/task/TaskProtocol.h"
#include "infrastructure/concurrency/TaskManager.h"

class QTimer;

namespace videoeye {
namespace qt {

class RepairController : public QObject {
    Q_OBJECT

public:
    explicit RepairController(QObject* parent = nullptr);
    ~RepairController() override;

    // 返回本次任务 id；0 表示已有任务在跑 / 并发已满，调用方应按失败处理。
    quint64 StartRepair(const model::RepairRequest& request);

    void Cancel();
    bool IsRunning() const;

    task::TaskState terminal_state() const {
        return tasks_.State(kSlot);
    }

    // 完成后的结果快照；任务未完成时为空。返回 shared_ptr 以避免在 UI 线程深拷贝
    // 诊断 / 证据 / 覆盖等大对象。
    std::shared_ptr<const model::RepairResult> result() const;

signals:
    void RepairStarted(quint64 task_id);
    void ProgressReported(quint64 task_id, int percent, const QString& stage, const QString& detail);
    void RepairFinished(quint64 task_id, const QString& status_text, const QString& summary);

private:
    static constexpr const char* kSlot = "video-repair";
    static constexpr int kShutdownBudgetMs = 3000;
    static constexpr int kPollIntervalMs = 100;

    struct RunState {
        std::mutex text_mutex;
        std::string stage;
        std::string detail;
        std::atomic<int> percent{0};
        std::atomic<bool> finished{false};

        std::mutex result_mutex;
        std::shared_ptr<const model::RepairResult> result;
    };

    void Poll();

    task::TaskManager tasks_{1};
    QTimer* poll_timer_ = nullptr;

    quint64 current_id_ = 0;
    std::shared_ptr<RunState> state_;
    mutable std::mutex result_mutex_;
    std::shared_ptr<const model::RepairResult> last_result_;
};

} // namespace qt
} // namespace videoeye
