// ReportingTaskCoordinator 的行为回归。
//
// 报告页是 ui/ 里唯一带后台线程的一页，这套机制的历史缺陷全在"生命周期"上：
//   * 对仍 joinable 的 std::thread 直接赋值 -> std::terminate（连续分析两次的老根因）；
//   * 线程体不收口异常 -> 要么 terminate，要么 body_done 永不置位、回收路径按严格
//     Cooperative 无限 join（关界面永久挂住）；
//   * 终态不归还 -> slot 永远停在 Running。
// 三条现在都由协调器统一保证，这里逐条钉住。
//
// 协调器不是 QObject，UI 侧动作全靠 std::function 注入（Deps），所以这里只需要一个
// QCoreApplication（让 PostToUi 的 QueuedConnection 真能投递）和一个接收者 QObject，
// 不需要 QWidget / offscreen 平台。

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QObject>
#include <QString>

#include <gtest/gtest.h>

#include "core/domain/task/TaskProtocol.h"

#include "ui/reporting_panel/ReportingTaskCoordinator.h"

namespace {

using videoeye::task::TaskState;
using videoeye::ui::AnalysisTask;
using videoeye::ui::ReportingTaskCoordinator;

// QueuedConnection 要真的投递，得有一个应用实例和一个活着的接收者。
int g_argc = 1;
char g_argv0[] = "test_reporting_task_coordinator";
char* g_argv[] = {g_argv0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

// 让排队的 UI 更新落地（等价于面板所在线程转了一圈事件循环）
void PumpUi() {
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool WaitBodyDone(const std::shared_ptr<AnalysisTask>& task, int budget_ms = 3000) {
    if (!task) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!task->body_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

struct Recording {
    std::vector<QString> failures;
    std::vector<QString> appended;
    std::vector<bool> busy_calls;
    int aborts = 0;

    ReportingTaskCoordinator::Deps MakeDeps(QObject* ui) {
        ReportingTaskCoordinator::Deps deps;
        deps.ui = ui;
        deps.show_failure = [this](const QString& text) { failures.push_back(text); };
        deps.append = [this](const QString& text) { appended.push_back(text); };
        deps.set_busy = [this](bool busy) { busy_calls.push_back(busy); };
        deps.abort_pending_export = [this]() { aborts += 1; };
        return deps;
    }
};

class ReportingTaskCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        receiver_ = new QObject;
        rec_ = Recording{};
    }
    void TearDown() override { delete receiver_; }

    QObject* receiver_ = nullptr;
    Recording rec_;
};

TEST_F(ReportingTaskCoordinatorTest, StartRunsBodyAndMarksSlotRunning) {
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    EXPECT_FALSE(runner.IsRunning());

    std::atomic<bool> ran{false};
    const std::shared_ptr<AnalysisTask> task = runner.Start(
        ReportingTaskCoordinator::Slot::Single,
        [&](const std::shared_ptr<AnalysisTask>&) { ran.store(true); });
    ASSERT_TRUE(task != nullptr);
    EXPECT_TRUE(WaitBodyDone(task));
    EXPECT_TRUE(ran.load());
    EXPECT_TRUE(runner.IsRunning());  // 还没归还终态

    runner.Finish(task, TaskState::Succeeded);
    EXPECT_FALSE(runner.IsRunning());
}

TEST_F(ReportingTaskCoordinatorTest, BothSlotsCanRunConcurrently) {
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    // 两个槽位各归各的：单文件与批量不被彼此的启动回收掉（回收只针对同槽位）。
    std::atomic<bool> release{false};
    auto waiter = [&](const std::shared_ptr<AnalysisTask>&) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    const auto single = runner.Start(ReportingTaskCoordinator::Slot::Single, waiter);
    const auto batch = runner.Start(ReportingTaskCoordinator::Slot::Batch, waiter);
    ASSERT_TRUE(single != nullptr);
    ASSERT_TRUE(batch != nullptr);
    EXPECT_FALSE(WaitBodyDone(single, 100));
    EXPECT_FALSE(WaitBodyDone(batch, 100));
    EXPECT_TRUE(runner.IsRunning());

    release.store(true);
    ASSERT_TRUE(WaitBodyDone(single));
    ASSERT_TRUE(WaitBodyDone(batch));
    runner.Finish(single, TaskState::Succeeded);
    runner.Finish(batch, TaskState::Succeeded);
    EXPECT_FALSE(runner.IsRunning());
}

TEST_F(ReportingTaskCoordinatorTest, RestartingSameSlotRetiresPreviousBody) {
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    std::atomic<bool> first_saw_cancel{false};
    std::atomic<bool> first_done{false};

    const auto first = runner.Start(
        ReportingTaskCoordinator::Slot::Single,
        [&](const std::shared_ptr<AnalysisTask>& t) {
            // 一个"会响应取消"的任务体：不响应就永远收不回来
            while (!t->cancel.IsCanceled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            first_saw_cancel.store(true);
            first_done.store(true);
        });
    ASSERT_TRUE(first != nullptr);

    // 同槽位再启动：必须先回收上一个 —— 对仍 joinable 的 std::thread 直接赋值
    // 会 std::terminate（连续分析两次的老根因）。
    const auto second = runner.Start(ReportingTaskCoordinator::Slot::Single,
                                     [](const std::shared_ptr<AnalysisTask>&) {});
    ASSERT_TRUE(second != nullptr);
    EXPECT_TRUE(WaitBodyDone(second));
    EXPECT_TRUE(first_done.load());       // 上一个任务体确实被收回来了（join 到了）
    EXPECT_TRUE(first_saw_cancel.load()); // 且是响应取消收回来的，不是撞运气
    runner.Finish(second, TaskState::Succeeded);
}

TEST_F(ReportingTaskCoordinatorTest, BodyExceptionIsContainedAndReported) {
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    const auto task = runner.Start(
        ReportingTaskCoordinator::Slot::Batch,
        [](const std::shared_ptr<AnalysisTask>&) { throw std::runtime_error("boom"); });
    ASSERT_TRUE(task != nullptr);
    ASSERT_TRUE(WaitBodyDone(task));

    // 异常路径把 UI 那一半排队回主线程：busy_ 必须解开、槽位必须归还终态，
    // 否则按钮全灰 + 转圈不停 + slot 永远 Running。
    PumpUi();
    ASSERT_EQ(rec_.failures.size(), 1u);
    EXPECT_TRUE(rec_.failures[0].contains(QString::fromUtf8("boom")));
    EXPECT_EQ(rec_.appended.size(), 1u);
    ASSERT_EQ(rec_.busy_calls.size(), 1u);
    EXPECT_FALSE(rec_.busy_calls[0]);  // set_busy(false)
    EXPECT_EQ(rec_.aborts, 1);         // 挂起的输出目录必须丢弃
    EXPECT_FALSE(runner.IsRunning());
}

TEST_F(ReportingTaskCoordinatorTest, CancelWinsOverExceptionForTerminalState) {
    // 终态按统一语义：取消优先于异常（见 core/domain/task/ThreadBody.h）。
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    const auto task = runner.Start(
        ReportingTaskCoordinator::Slot::Single,
        [](const std::shared_ptr<AnalysisTask>& t) {
            t->cancel.RequestCancel();  // 任务体自己判定为"被中断"
            throw std::runtime_error("interrupted");
        });
    ASSERT_TRUE(task != nullptr);
    ASSERT_TRUE(WaitBodyDone(task));
    PumpUi();
    EXPECT_EQ(rec_.failures.size(), 1u);  // 仍然要报告（异常文本不能消失）
    EXPECT_FALSE(runner.IsRunning());     // 终态已归还，slot 不停在 Running
}

TEST_F(ReportingTaskCoordinatorTest, RequestCancelOnlySignalsTerminalComesFromBody) {
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    const auto task = runner.Start(
        ReportingTaskCoordinator::Slot::Batch,
        [](const std::shared_ptr<AnalysisTask>& t) {
            while (!t->cancel.IsCanceled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    ASSERT_TRUE(task != nullptr);
    EXPECT_FALSE(task->cancel.IsCanceled());

    runner.RequestCancel(ReportingTaskCoordinator::Slot::Batch);  // 取消按钮那条路
    EXPECT_TRUE(WaitBodyDone(task));
    runner.Finish(task, TaskState::Canceled);
    EXPECT_FALSE(runner.IsRunning());
}

TEST_F(ReportingTaskCoordinatorTest, PostToUiSkipsDeadTask) {
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    // alive=false 表示面板正在销毁：此时排队的 UI 更新必须被丢掉，
    // 否则会落到已销毁的 QWidget 上。
    auto dead = std::make_shared<AnalysisTask>();
    dead->alive.store(false, std::memory_order_release);
    std::atomic<int> calls{0};
    runner.PostToUi(dead, [&]() { calls += 1; });
    PumpUi();
    EXPECT_EQ(calls.load(), 0);

    auto live = std::make_shared<AnalysisTask>();
    runner.PostToUi(live, [&]() { calls += 1; });
    PumpUi();
    EXPECT_EQ(calls.load(), 1);
}

TEST_F(ReportingTaskCoordinatorTest, RetireAllIsSafeWithoutAnyTask) {
    ReportingTaskCoordinator runner(rec_.MakeDeps(receiver_));
    runner.RetireAll();  // 一次任务都没起过：不该崩、不该卡
    EXPECT_FALSE(runner.IsRunning());
    EXPECT_TRUE(rec_.busy_calls.empty());
}

}  // namespace
