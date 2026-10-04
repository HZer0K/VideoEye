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
using videoeye::task::TaskKind;
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

// --- 终态一次性 ---
//
// 终态"一旦写入就不再变化"是 TaskManager 的核心契约: 做不到这一点,
// "任务体正常返回 -> 记 Succeeded" 与 "排队到 UI 线程的失败消息后到 -> 记 Failed"
// 就会互相覆盖, 终态取决于两条路径谁后跑完。下面这两条正对着那两种覆盖。

TEST(TaskManagerTest, TerminalStateIsNotOverwrittenByLaterEnd) {
    TaskManager mgr;
    const TaskId id = mgr.Begin(kSlot);
    mgr.End(kSlot, id, TaskState::Succeeded);
    ASSERT_EQ(mgr.State(kSlot), TaskState::Succeeded);

    mgr.End(kSlot, id, TaskState::Failed);
    EXPECT_EQ(mgr.State(kSlot), TaskState::Succeeded) << "Succeeded 不得被后到的 Failed 改写";
    mgr.End(kSlot, id, TaskState::Canceled);
    EXPECT_EQ(mgr.State(kSlot), TaskState::Succeeded) << "Succeeded 不得被后到的 Canceled 改写";
}

TEST(TaskManagerTest, CanceledIsNotOverwrittenBySucceeded) {
    TaskManager mgr;
    const TaskId id = mgr.Begin(kSlot);
    mgr.Cancel(kSlot);
    mgr.End(kSlot, id, TaskState::Canceled);
    ASSERT_EQ(mgr.State(kSlot), TaskState::Canceled);

    mgr.End(kSlot, id, TaskState::Succeeded);
    EXPECT_EQ(mgr.State(kSlot), TaskState::Canceled) << "Canceled 不得被后到的 Succeeded 改写";
}

// 同一任务的两次 End() 只有第一次算数; 且"旧任务的失败消息在新任务开始后才落地"
// 不得影响新任务的终态(这正是 UI 回调补写终态的现场)。
TEST(TaskManagerTest, StaleFailureAfterSupersedeKeepsNewTerminal) {
    TaskManager mgr;
    const TaskId first = mgr.Begin(kSlot);
    const TaskId second = mgr.Begin(kSlot);
    mgr.End(kSlot, second, TaskState::Succeeded);
    mgr.End(kSlot, first, TaskState::Failed);  // 旧任务的失败消息后到
    EXPECT_EQ(mgr.State(kSlot), TaskState::Succeeded);

    // 反过来: 新任务先失败, 旧任务的"成功"后到, 同样不得改写
    const TaskId third = mgr.Begin(kSlot);
    mgr.End(kSlot, third, TaskState::Failed);
    mgr.End(kSlot, second, TaskState::Succeeded);
    EXPECT_EQ(mgr.State(kSlot), TaskState::Failed);
}

// --- 任务体自己声明终态 (RunWithResult) ---

TEST(TaskManagerTest, RunWithResultHonorsBodyTerminal) {
    TaskManager mgr;
    mgr.RunWithResult(kSlot, [](TaskId, CancelToken) { return TaskState::Failed; });
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));
    EXPECT_EQ(mgr.State(kSlot), TaskState::Failed);
}

// 任务体返回过程态等于没给结论, 按违约处理: 记 Failed, 不能让 slot 挂在 Running 上。
TEST(TaskManagerTest, RunWithResultTreatsNonTerminalAsFailed) {
    TaskManager mgr;
    mgr.RunWithResult(kSlot, [](TaskId, CancelToken) { return TaskState::Running; });
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));
    EXPECT_EQ(mgr.State(kSlot), TaskState::Failed);
}

// 取消优先于任务体自己报的成功 —— 否则"用户取消了, 任务体却说成功"会盖掉取消。
TEST(TaskManagerTest, RunWithResultCancelWinsOverSucceeded) {
    TaskManager mgr;
    std::atomic<bool> entered{false};
    mgr.RunWithResult(kSlot, [&](TaskId, CancelToken token) {
        entered.store(true);
        while (!token.IsCanceled())
            SleepMs(5);
        return TaskState::Succeeded;
    });
    while (!entered.load())
        SleepMs(1);
    mgr.Cancel(kSlot);
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));
    EXPECT_EQ(mgr.State(kSlot), TaskState::Canceled);
}

// 任务体已经声明过终态之后, Run 收尾时的那次写入必须被丢弃(终态一次性)。
TEST(TaskManagerTest, RunWithResultKeepsBodyTerminalEvenIfBodyEndsFirst) {
    TaskManager mgr;
    mgr.RunWithResult(kSlot, [&](TaskId id, CancelToken) {
        mgr.End(kSlot, id, TaskState::Canceled);  // 任务体自己先收尾
        return TaskState::Succeeded;              // 返回值的这次写入必须被丢弃
    });
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000));
    EXPECT_EQ(mgr.State(kSlot), TaskState::Canceled);
}

// CurrentId 在"任务已终态"之后仍然能报出当前任务的 id —— 这是判定"排队到 UI 线程
// 的收尾消息是否过期"的唯一手段(IsCurrent 那时必然是 false, 会把正常消息一起丢掉)。
TEST(TaskManagerTest, CurrentIdSurvivesTerminalState) {
    TaskManager mgr;
    EXPECT_EQ(mgr.CurrentId("never-used"), 0u);
    const TaskId id = mgr.Begin(kSlot);
    EXPECT_EQ(mgr.CurrentId(kSlot), id);
    EXPECT_TRUE(mgr.IsCurrent(kSlot, id));

    mgr.End(kSlot, id, TaskState::Failed);
    EXPECT_EQ(mgr.CurrentId(kSlot), id) << "终态之后 current id 仍然是它, 过期消息才能被认出来";
    EXPECT_FALSE(mgr.IsCurrent(kSlot, id));

    const TaskId next = mgr.Begin(kSlot);
    EXPECT_EQ(mgr.CurrentId(kSlot), next);
    EXPECT_NE(mgr.CurrentId(kSlot), id);
    mgr.End(kSlot, next, TaskState::Succeeded);
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

// --- 关闭阶段的耗时上限 ---
//
// 评审 P2 的原话是"别把'状态超时'误认为'函数返回超时'": WaitForAll(t) 以前只保证"等
// 终态"不超过 t, 之后还要无条件 join 受管线程, 任务体不理取消时整个关闭就无限期了。
// 现在的约定是: t 是**整个函数**的总预算; 只有 TaskKind::BlockingIo 允许在超时后被放弃,
// Cooperative 仍然 join(它随时可能还在访问外部对象, detach 会造成悬空访问)。

// 不响应取消的 BlockingIo 任务: 必须在预算内被放弃, WaitForAll 不能陪它耗到底。
TEST(TaskManagerTest, WaitForAllAbandonsBlockingIoTaskWithinBudget) {
    constexpr char slot[] = "blocking-io-slot";
    TaskManager mgr(2);
    std::atomic<bool> entered{false};
    // 故意完全不轮询取消令牌: 模拟连 AVIOInterruptCB 都不响应的那种卡死的 FFmpeg IO。
    mgr.RunBlockingIo(slot, [&](TaskId, CancelToken) {
        entered.store(true);
        SleepMs(3000);   // 远超本次关闭预算
    });
    while (!entered.load())
        SleepMs(1);      // 等 body 真的跑起来, 否则测到的就不是"卡住"了

    const auto start = std::chrono::steady_clock::now();
    mgr.WaitForAll(200);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();

    EXPECT_LT(elapsed_ms, 2000) << "BlockingIo 任务超预算时必须被放弃, 不能 join 到它跑完(3s); "
                                   "实测耗时 " << elapsed_ms << "ms";
    EXPECT_EQ(mgr.AbandonedCount(), 1u) << "确实应该走'放弃'这条路径";
    // 放弃了它, slot 也必须能立刻重新投入使用 —— 否则之后每次 Begin 都要白等一整轮预算
    EXPECT_FALSE(mgr.IsRunning(slot));
    EXPECT_EQ(mgr.State(slot), TaskState::Canceled);
    const TaskId reused = mgr.Begin(slot);
    EXPECT_NE(reused, 0u);
    mgr.End(slot, reused, TaskState::Succeeded);
}

// 响应取消的 Cooperative 任务: 即使收尾慢一点也必须被 join 到底, 绝不能被 detach。
TEST(TaskManagerTest, WaitForAllJoinsCooperativeTask) {
    TaskManager mgr(2);
    std::atomic<bool> body_done{false};
    mgr.Run(kSlot, [&](TaskId, CancelToken token) {
        // 会响应取消, 只是退出前还要花点时间: 正常的协作式任务就是这个样子
        while (!token.IsCanceled())
            SleepMs(5);
        SleepMs(50);
        body_done.store(true);
    });
    while (!mgr.IsRunning(kSlot))
        SleepMs(1);

    mgr.CancelAll();
    mgr.WaitForAll(2000);

    EXPECT_TRUE(body_done.load()) << "协作式任务必须 join 到真正退出, 不能中途扔掉";
    EXPECT_EQ(mgr.AbandonedCount(), 0u);
    EXPECT_FALSE(mgr.IsRunning(kSlot));
}

// TaskKind 要能从 Begin 一路带到 WaitForAll: 同一次 WaitForAll 里混着两种任务时,
// 只有 BlockingIo 那一个被放弃。
TEST(TaskManagerTest, WaitForAllAppliesKindPerTask) {
    TaskManager mgr(4);
    std::atomic<bool> coop_done{false};
    std::atomic<bool> blocking_entered{false};

    mgr.Run("coop", [&](TaskId, CancelToken token) {
        while (!token.IsCanceled())
            SleepMs(5);
        coop_done.store(true);
    });
    mgr.RunBlockingIo("io", [&](TaskId, CancelToken) {
        blocking_entered.store(true);
        SleepMs(3000);
    });
    // coop 要等 CancelAll 才会退出, 这里只能等 io 的 body 真的跑起来
    while (!blocking_entered.load())
        SleepMs(1);

    mgr.CancelAll();
    mgr.WaitForAll(300);

    EXPECT_TRUE(coop_done.load()) << "协作式任务要被 join 完, 不能因为旁边的任务超时被一起扔掉";
    EXPECT_EQ(mgr.AbandonedCount(), 1u) << "只有一个任务是 BlockingIo, 也只有一个该被放弃";
    EXPECT_FALSE(mgr.IsRunning("coop"));
    EXPECT_FALSE(mgr.IsRunning("io"));
}

// --- 线程登记路径的健壮性（复查 P2）---
//
// RunWithResult 里"线程已经起来、但还没交到 slot / orphans_ 手上"这段窗口上, 任何
// 一步抛异常都会让局部 std::thread 带着 joinable 状态析构 —— 那是直接 std::terminate(),
// 连日志都来不及打。现在这段由 RAII 守卫兜住: 异常时 detach(不是 join, 这里在调用方
// 线程上, 而任务体可能正卡在第三方 IO), 容器的内存准备则尽量挪到线程启动之前。
//
// 故障注入本身不好做: 要让 orphans_.push_back 抛 bad_alloc 就得接管全局 operator new,
// 那会污染同一个进程里的其它用例。所以这里守住的是**主路径**: 高频取代会不断把
// 线程推进 orphans_, 只要登记或守卫有任何一处漏了 joinable 的线程, 进程就会在
// 析构时 terminate —— 用例根本走不到断言那一行。
TEST(TaskManagerTest, RapidSupersedeOnOneSlotKeepsSlotUsable) {
    constexpr int kRounds = 20;
    // 预算给到远大于轮数: 孤儿线程要等下一次 WaitForAll 才归还预算, 给小了后面的
    // Begin 会被"并发已满"拒掉 —— 那时测的就是拒绝路径, 不是这里要压的登记路径。
    // (顺带说明 Begin 的语义: 被拒绝时它也会置当前任务的取消标志, 所以一旦有轮次
    //  被拒, 最后一个任务的终态会是 Canceled 而不是 Succeeded。)
    TaskManager mgr(64);
    std::atomic<int> launched{0};

    for (int i = 0; i < kRounds; ++i) {
        // 任务体不理取消令牌: 上一个任务会被下一起 Begin 顶成孤儿, 正是要压的路径。
        mgr.RunWithResult(kSlot,
                          [&](TaskId, CancelToken) {
                              launched.fetch_add(1);
                              SleepMs(20);
                              return TaskState::Succeeded;
                          },
                          0, TaskKind::BlockingIo);
    }

    const TaskId last = mgr.CurrentId(kSlot);
    EXPECT_NE(last, 0u);
    // 最后一个任务必须能正常收尾: 孤儿收纳要是漏了线程, Begin 的并发预算会被占满,
    // 后面的任务就会被"并发已满"拒掉(返回 0)。
    EXPECT_TRUE(mgr.WaitForIdle(kSlot, 5000))
        << "高频取代之后 slot 必须还能到达终态; 终态卡住说明有线程没被正确收纳";
    EXPECT_EQ(mgr.State(kSlot), TaskState::Succeeded);
    EXPECT_GT(launched.load(), 0) << "任务体一个都没跑起来, 这条用例等于没测";
}

// 空任务体的 RunWithResult 必须记 Failed 而不是把 slot 挂在 Running 上。
// 与 Begin 分开看: 这里的 id 已经发出来了, 失败要写在同一个 id 上。
TEST(TaskManagerTest, RunWithResultWithoutBodyEndsFailedImmediately) {
    TaskManager mgr;
    const TaskId id = mgr.RunWithResult(kSlot, nullptr);
    EXPECT_NE(id, 0u) << "没有任务体也该登记出一个 id, 调用方要能统一处理";
    EXPECT_EQ(mgr.State(kSlot), TaskState::Failed);
    EXPECT_EQ(mgr.CurrentId(kSlot), id);
    EXPECT_FALSE(mgr.IsRunning(kSlot));
}

