// 分析任务登记处（core/qc/AnalysisTaskRegistry）的并发契约测试
//
// 为什么单独成文件：登记处是**全局单例**，而它的全部价值就在"计数准不准" ——
// 准，派发侧的背压才成立；不准，max_parallel 就是个摆设（或者反过来把整批卡死）。
// 所以它必须自己有一组用例，而不是蹭在批量扫描的用例里当配角。
//
// 这里钉住四件事：
//   1. 额度与计数的进出配对（含"没被认领"那条兜底路径）；
//   2. 运行 -> 退出中 -> 已释放 是唯一的合法转换方向，重复调用一律幂等；
//   3. MarkExiting 与 Release **并发**打在同一张凭证上时不留残账
//      （这条就是评审点名的竞态：先改原子标志、再加锁改计数会留下一个永远还不清的
//      退出计数，背压从此偏严）；
//   4. 拿不到额度时 AcquireUntilFit 会排队 —— 这是"并发上限是硬约束"的那一半。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/qc/AnalysisTaskRegistry.h"

using videoeye::qc::AnalysisTaskRegistry;
using videoeye::qc::AnalysisTaskSlotPtr;

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

int64_t ElapsedMs(Clock::time_point from) {
    return std::chrono::duration_cast<Ms>(Clock::now() - from).count();
}

// 等登记处归零。用例之间共享同一个单例，收尾不干净会污染下一条用例的断言。
void WaitUntilIdle(int timeout_ms = 5000) {
    const auto t0 = Clock::now();
    while (AnalysisTaskRegistry::Instance().TotalCount() != 0 && ElapsedMs(t0) < timeout_ms) {
        std::this_thread::sleep_for(Ms(2));
    }
}

} // namespace

// ---- 1. 进出配对：认领过的与没认领的都要退账 ----
TEST(AnalysisTaskRegistryTest, AcquireAndReleaseKeepCountersPaired) {
    WaitUntilIdle();
    AnalysisTaskRegistry& registry = AnalysisTaskRegistry::Instance();
    ASSERT_EQ(registry.TotalCount(), 0) << "上一条用例留下了额度";

    {
        auto adopted = registry.AcquireUnbounded();
        EXPECT_EQ(registry.RunningCount(), 1);
        adopted->Adopt();
        EXPECT_TRUE(adopted->IsAdopted());
        adopted->Release();
        EXPECT_EQ(registry.TotalCount(), 0) << "认领过的凭证归还后必须立刻退账";
        EXPECT_TRUE(adopted->IsReleased());
    }

    {
        // 假分析函数（批量单测里那种）不认领：派发侧兜底归还，同样要退账。
        auto unadopted = registry.AcquireUnbounded();
        EXPECT_EQ(registry.RunningCount(), 1);
        unadopted->Release();
        EXPECT_EQ(registry.TotalCount(), 0) << "未认领的凭证由派发侧归还，也要退账";
    }

    WaitUntilIdle();
}

// ---- 2. 状态只能单向推进，重复调用幂等 ----
TEST(AnalysisTaskRegistryTest, StateTransitionsAreOneWayAndIdempotent) {
    WaitUntilIdle();
    AnalysisTaskRegistry& registry = AnalysisTaskRegistry::Instance();

    auto slot = registry.AcquireUnbounded();
    slot->Adopt();
    EXPECT_EQ(registry.RunningCount(), 1);
    EXPECT_EQ(registry.ExitingCount(), 0);

    slot->MarkExiting();
    EXPECT_EQ(registry.RunningCount(), 0) << "标退出之后不再占运行额度";
    EXPECT_EQ(registry.ExitingCount(), 1);
    slot->MarkExiting(); // 幂等：detach 路径上派发侧与线程体都会调一次
    EXPECT_EQ(registry.ExitingCount(), 1) << "重复标退出不得重复计数";

    slot->Release();
    EXPECT_EQ(registry.TotalCount(), 0);
    slot->Release();
    slot->MarkExiting(); // 已归还之后不得再改账
    EXPECT_EQ(registry.TotalCount(), 0) << "归还之后再动状态不得把计数改回来";
    WaitUntilIdle();
}

// ---- 3. 竞态：MarkExiting 与 Release 打在同一张凭证上 ----
//
// 超时等待线程（判定放弃 -> MarkExiting）与分析线程体退出（Release）操作的就是同一张
// 凭证。分段实现（先改原子标志、再加锁改计数）会留下一个"加过没减 / 减了没加过"的
// 残账；这里用高频并发把这个窗口撞出来。
TEST(AnalysisTaskRegistryTest, ConcurrentMarkExitingAndReleaseLeaveNoResidue) {
    WaitUntilIdle();
    AnalysisTaskRegistry& registry = AnalysisTaskRegistry::Instance();

    constexpr int kRounds = 300;
    for (int round = 0; round < kRounds; ++round) {
        auto slot = registry.AcquireUnbounded();
        slot->Adopt();
        std::thread exiting([slot]() { slot->MarkExiting(); });
        std::thread releasing([slot]() { slot->Release(); });
        exiting.join();
        releasing.join();
        slot->Release(); // 无论谁先跑，最终必须归零
    }

    EXPECT_EQ(registry.RunningCount(), 0) << "并发打完 " << kRounds << " 轮之后仍有运行额度没退";
    EXPECT_EQ(registry.ExitingCount(), 0) << "并发打完 " << kRounds << " 轮之后留下退出残账（背压会永久偏严）";
    WaitUntilIdle();
}

// ---- 4a. 拿不到额度时排队，而不是放行 ----
TEST(AnalysisTaskRegistryTest, AcquireUntilFitWaitsUntilASlotIsReturned) {
    WaitUntilIdle();
    AnalysisTaskRegistry& registry = AnalysisTaskRegistry::Instance();

    auto first = registry.Acquire(1, 1, 0, nullptr); // 上限 1：占满
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(registry.TotalCount(), 1);
    EXPECT_EQ(registry.Acquire(1, 1, 0, nullptr), nullptr) << "上限为 1 时第二个必须拿不到";

    std::atomic<bool> got{false};
    std::thread waiter([&got]() {
        auto slot = AnalysisTaskRegistry::Instance().AcquireUntilFit(1, 1, nullptr);
        got.store(slot != nullptr, std::memory_order_release);
    });

    std::this_thread::sleep_for(Ms(60));
    EXPECT_FALSE(got.load(std::memory_order_acquire)) << "额度还没还，排队的那一路就被放行了";

    first.reset(); // 归还：排队的那一路应当立刻接上
    const auto t0 = Clock::now();
    while (!got.load(std::memory_order_acquire) && ElapsedMs(t0) < 3000) {
        std::this_thread::sleep_for(Ms(2));
    }
    waiter.join();
    EXPECT_TRUE(got.load(std::memory_order_acquire)) << "额度归还之后排队者仍没被放行";

    WaitUntilIdle();
    EXPECT_EQ(registry.TotalCount(), 0);
}

// ---- 4b. 取消是这条队列唯一的出口 ----
TEST(AnalysisTaskRegistryTest, AcquireUntilFitReturnsNullptrWhenCancelled) {
    WaitUntilIdle();
    AnalysisTaskRegistry& registry = AnalysisTaskRegistry::Instance();

    auto first = registry.Acquire(1, 1, 0, nullptr);
    ASSERT_NE(first, nullptr);

    const auto t0 = Clock::now();
    const auto slot = registry.AcquireUntilFit(1, 1, []() { return true; });
    const int64_t dt = ElapsedMs(t0);

    EXPECT_EQ(slot, nullptr) << "取消时必须立刻停下，不能继续排队";
    EXPECT_LT(dt, 1000) << "取消到返回不该有可感知的延迟（实测 " << dt << "ms）";

    first.reset();
    WaitUntilIdle();
}
