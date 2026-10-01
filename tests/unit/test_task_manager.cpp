// TaskManager 单元测试: 覆盖统一后台任务调度约定
//   - 任务 ID / 终态结果
//   - 取消标志能被任务体观测到
//   - 同 slot 上旧任务被取代后结果过期(IsCurrent == false)
//   - 并发上限
//   - 析构等待在跑的任务收尾

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "infrastructure/concurrency/TaskManager.h"

namespace {

using videoeye::task::CancelToken;
using videoeye::task::TaskId;
using videoeye::task::TaskManager;
using videoeye::task::TaskState;

constexpr char kSlot[] = "test-slot";

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

// --- 任务 ID / 终态 ---

TEST(TaskManagerTest, BeginAssignsFreshMonotonicIds) {
    TaskManager mgr;
    const TaskId a = mgr.Begin("slot-a");
    const TaskId b = mgr.Begin("slot-b");
    const TaskId c = mgr.Begin("slot-a"); // 同 slot 再次登记
    EXPECT_NE(a, 0u);
    EXPECT_NE(b, 0u);
    EXPECT_NE(c, 0u);
    EXPECT_NE(a, b);
    EXPECT_GT(c, a);

    // 收尾: 任务必须上报终态, 否则 TaskManager 析构时要等满超时
    mgr.End("slot-a", c, TaskState::Succeeded);
    mgr.End("slot-b", b, TaskState::Succeeded);
}

TEST(TaskManagerTest, EndRecordsTerminalStateOnly) {
    TaskManager mgr;
    const TaskId id = mgr.Begin(kSlot);
    EXPECT_TRUE(mgr.IsRunning(kSlot));
    EXPECT_EQ(mgr.State(kSlot), TaskState::Running);

    // 过程态不该被 End 接受
    mgr.End(kSlot, id, TaskState::Running);
    EXPECT_EQ(mgr.State(kSlot), TaskState::Running);

    mgr.End(kSlot, id, TaskState::Succeeded);
    EXPECT_FALSE(mgr.IsRunning(kSlot));
    EXPECT_EQ(mgr.State(kSlot), TaskState::Succeeded);
}

TEST(TaskManagerTest, EndIgnoresStaleTaskId) {
    TaskManager mgr;
    const TaskId old_id = mgr.Begin(kSlot);
    const TaskId new_id = mgr.Begin(kSlot);
    mgr.End(kSlot, old_id, TaskState::Succeeded); // 旧任务的终态不应生效
    EXPECT_TRUE(mgr.IsRunning(kSlot));
    EXPECT_TRUE(mgr.IsCurrent(kSlot, new_id));
    EXPECT_FALSE(mgr.IsCurrent(kSlot, old_id));
    mgr.End(kSlot, new_id, TaskState::Succeeded);
}

TEST(TaskManagerTest, WaitForAllForcesTerminalOnUnreportedTask) {
    TaskManager mgr;
    const TaskId id = mgr.Begin(kSlot); // 故意不调 End: 模拟 worker 线程被外部干掉的情况
    EXPECT_TRUE(mgr.IsRunning(kSlot));

    mgr.WaitForAll(100); // 限时等到超时

    EXPECT_FALSE(mgr.IsRunning(kSlot)) << "自管线程的任务超时未报终态时, 应被强制收尾";
    EXPECT_EQ(mgr.State(kSlot), TaskState::Canceled);
    EXPECT_FALSE(mgr.IsCurrent(kSlot, id));
    EXPECT_EQ(mgr.RunningCount(), 0u);
}

TEST(TaskManagerTest, WaitForAllTreatsTimeoutAsTotalBudget) {
    TaskManager mgr;
    for (int i = 0; i < 3; ++i)
        mgr.Begin("slot-" + std::to_string(i));   // 故意都不报终态

    const auto start = std::chrono::steady_clock::now();
    mgr.WaitForAll(200);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();

    EXPECT_LT(elapsed_ms, 600) << "timeout 是总预算, 不是每个 slot 各等一份 (3 个 slot 不该等 600ms)";
    EXPECT_EQ(mgr.RunningCount(), 0u) << "超时的自管线程任务应被强制收尾";
}

// --- 取消 ---

TEST(TaskManagerTest, CancelIsObservableByTaskBody) {
    TaskManager mgr;
    std::atomic<bool> observed_cancel{false};
    std::atomic<bool> body_done{false};

    const TaskId id = mgr.Run(kSlot, [&](TaskId, CancelToken token) {
        for (int i = 0; i < 500 && !token.IsCanceled(); ++i)
            SleepMs(5);
        observed_cancel.store(token.IsCanceled());
        body_done.store(true);
    });

    SleepMs(20);
    mgr.Cancel(kSlot);
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));
    ASSERT_TRUE(body_done.load());
    EXPECT_TRUE(observed_cancel.load());
    // 被取消的任务终态是 Canceled, 不能被记成成功
    EXPECT_EQ(mgr.State(kSlot), TaskState::Canceled);
    EXPECT_NE(id, 0u);
}

TEST(TaskManagerTest, CancelOnIdleSlotIsNoop) {
    TaskManager mgr;
    mgr.Cancel("never-used");
    EXPECT_FALSE(mgr.IsRunning("never-used"));
    EXPECT_EQ(mgr.State("never-used"), TaskState::Idle);
}

// --- 过期结果丢弃 ---

TEST(TaskManagerTest, SupersededTaskResultIsDiscarded) {
    TaskManager mgr;
    std::atomic<bool> published{false};

    // 旧任务: 慢慢算, 算完再看结果是否还新鲜
    mgr.Run(kSlot, [&](TaskId id, CancelToken token) {
        SleepMs(200);
        if (token.IsCanceled() || !mgr.IsCurrent(kSlot, id))
            return;
        published.store(true);
    });
    SleepMs(20);

    // 新任务接管 slot
    const TaskId new_id = mgr.Begin(kSlot);
    EXPECT_TRUE(mgr.IsCurrent(kSlot, new_id));
    mgr.End(kSlot, new_id, TaskState::Succeeded);
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));

    EXPECT_FALSE(published.load()) << "被取代的旧任务不应再投递结果";
}

TEST(TaskManagerTest, BeginWaitsForPreviousWhenAsked) {
    TaskManager mgr;
    std::atomic<bool> body_done{false};
    mgr.Run(kSlot, [&](TaskId, CancelToken token) {
        for (int i = 0; i < 200 && !token.IsCanceled(); ++i)
            SleepMs(5);
        body_done.store(true);
    });
    SleepMs(20);

    const TaskId new_id = mgr.Begin(kSlot, 5000); // 限时等旧任务退出
    EXPECT_TRUE(body_done.load());
    EXPECT_TRUE(mgr.IsCurrent(kSlot, new_id));
    mgr.End(kSlot, new_id, TaskState::Succeeded);
}

// --- 受管线程 ---

TEST(TaskManagerTest, RunExecutesBodyAndAutoEnds) {
    TaskManager mgr;
    std::atomic<int> calls{0};
    const TaskId id = mgr.Run(kSlot, [&](TaskId, CancelToken) { calls.fetch_add(1); });
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(mgr.State(kSlot), TaskState::Succeeded);
    EXPECT_NE(id, 0u);
}

TEST(TaskManagerTest, RunRecordsFailedOnThrowingBody) {
    TaskManager mgr;
    const TaskId id = mgr.Run(kSlot, [](TaskId, CancelToken) { throw std::runtime_error("boom"); });
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));
    EXPECT_EQ(mgr.State(kSlot), TaskState::Failed);
    EXPECT_NE(id, 0u);
}

// --- 并发上限 ---

TEST(TaskManagerTest, ConcurrencyCapRejectsExtraSlots) {
    TaskManager mgr(1); // 最多 1 个并发
    EXPECT_NE(mgr.Begin("slot-a"), 0u);
    EXPECT_EQ(mgr.Begin("slot-b"), 0u) << "并发已满时 Begin 应返回 0";

    // 释放 slot-a (重新登记一次拿到当前 id, 再写终态)
    const TaskId a = mgr.Begin("slot-a");
    mgr.End("slot-a", a, TaskState::Succeeded);
    EXPECT_EQ(mgr.RunningCount(), 0u);
    const TaskId b = mgr.Begin("slot-b");
    EXPECT_NE(b, 0u);
    mgr.End("slot-b", b, TaskState::Succeeded);
}

TEST(TaskManagerTest, RunningCountTracksActiveSlots) {
    TaskManager mgr(4);
    EXPECT_EQ(mgr.RunningCount(), 0u);
    const TaskId a = mgr.Begin("a");
    const TaskId b = mgr.Begin("b");
    EXPECT_EQ(mgr.RunningCount(), 2u);
    mgr.CancelAll();
    mgr.End("a", a, TaskState::Canceled);
    mgr.End("b", b, TaskState::Canceled);
    EXPECT_EQ(mgr.RunningCount(), 0u);
}

// --- 析构 ---

TEST(TaskManagerTest, DestructorWaitsForRunningBody) {
    std::atomic<bool> body_done{false};
    {
        TaskManager mgr;
        mgr.Run(kSlot, [&](TaskId, CancelToken) {
            SleepMs(150);
            body_done.store(true);
        });
        SleepMs(10);
    } // 析构: 取消 + 等待
    EXPECT_TRUE(body_done.load()) << "析构后任务体必须已经跑完(否则线程会在对象销毁后继续跑)";
}

// --- 关闭路径可中断性（配合 FFmpeg AVIO 中断回调）---

// flag() 必须把底层取消标志暴露出来, 且 Cancel 之后该标志必须置位 ——
// 这正是关闭流程里 CancelAll() 能让卡在 FFmpeg 阻塞 IO 的受管线程及时退出的前提。
TEST(TaskManagerTest, CancelTokenExposesFlagReflectsCancel) {
    TaskManager mgr(1);
    const TaskId id = mgr.Begin(kSlot);
    ASSERT_NE(id, 0u);

    CancelToken token = mgr.Token(kSlot);
    ASSERT_NE(token.flag(), nullptr)
        << "flag() 必须返回底层取消标志, 供 FFmpeg 中断回调读取";
    EXPECT_FALSE(token.IsCanceled());

    mgr.Cancel(kSlot);
    EXPECT_TRUE(token.IsCanceled());
    EXPECT_TRUE(token.flag()->load())
        << "Cancel 后底层标志必须置位, 否则 WaitForAll 不会因中断而返回";

    mgr.End(kSlot, id, TaskState::Succeeded);
}

// 取消后受管任务必须在有界时间内到达终态, 关闭路径(WaitForAll)才不会在 join 上挂死。
// 这里的轮询循环与 FFmpeg 中断回调同源: 都只读同一个取消标志。
TEST(TaskManagerTest, RunTaskReachesTerminalAfterCancel) {
    TaskManager mgr(1);
    std::atomic<bool> saw_cancel{false};
    mgr.Run(kSlot, [&](TaskId, CancelToken token) {
        while (!token.IsCanceled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        saw_cancel = true;
    });

    EXPECT_TRUE(mgr.IsRunning(kSlot));
    mgr.Cancel(kSlot);

    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 2000))
        << "取消后受管任务必须及时到达终态, 关闭路径才不会挂死";
    EXPECT_TRUE(saw_cancel.load());
    EXPECT_FALSE(mgr.IsRunning(kSlot));
}

