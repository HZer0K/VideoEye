// 性能指标通道的回归：重点是"口径能不能被机器观察到"。
//
// 这里的指标只写日志、不进 UI，所以**唯一**能验证它们真的被记下来的办法
// 就是 PerfMetrics::Snapshot()。没有这些断言的话，"忘记埋点"和"埋了但没人看"
// 长得一模一样 —— 那正是本轮要消除的失败模式。

#include "infrastructure/logging/PerfMetrics.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace {

using videoeye::CancelLatencyProbe;
using videoeye::PerfMetrics;
using videoeye::PerfScope;

// 每个用例都从空表开始：指标是全局单例，用例之间互相污染会变成
// "上一个用例的 count 混进下一个断言"这类最难查的偶发失败。
class PerfMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { PerfMetrics::GetInstance().Reset(); }
    void TearDown() override { PerfMetrics::GetInstance().Reset(); }
};

TEST_F(PerfMetricsTest, RecordTimingAccumulates) {
    PerfMetrics::GetInstance().RecordTiming("t", 10.0);
    PerfMetrics::GetInstance().RecordTiming("t", 30.0);
    PerfMetrics::GetInstance().RecordTiming("t", 20.0);

    const auto s = PerfMetrics::GetInstance().Snapshot("t");
    EXPECT_EQ(3, s.count);
    EXPECT_DOUBLE_EQ(60.0, s.sum);
    EXPECT_DOUBLE_EQ(10.0, s.min);
    EXPECT_DOUBLE_EQ(30.0, s.max);
    EXPECT_DOUBLE_EQ(20.0, s.last);   // last 必须是最后一次，不是平均
}

TEST_F(PerfMetricsTest, SnapshotOfUnknownNameIsEmpty) {
    const auto s = PerfMetrics::GetInstance().Snapshot("never_recorded");
    EXPECT_EQ(0, s.count);
    EXPECT_DOUBLE_EQ(0.0, s.sum);
}

TEST_F(PerfMetricsTest, NamesExposesRecordedMetrics) {
    EXPECT_TRUE(PerfMetrics::GetInstance().Names().empty());

    PerfMetrics::GetInstance().RecordTiming("a", 1.0);
    PerfMetrics::GetInstance().RecordGauge("b", 2.0, "MB");

    const auto names = PerfMetrics::GetInstance().Names();
    EXPECT_EQ(2u, names.size());
    EXPECT_NE(names.end(), std::find(names.begin(), names.end(), "a"));
    EXPECT_NE(names.end(), std::find(names.begin(), names.end(), "b"));
}

TEST_F(PerfMetricsTest, GaugeKeepsExtremes) {
    PerfMetrics::GetInstance().RecordGauge("mem", 100.0, "MB");
    PerfMetrics::GetInstance().RecordGauge("mem", 250.0, "MB");
    PerfMetrics::GetInstance().RecordGauge("mem", 50.0, "MB");

    const auto s = PerfMetrics::GetInstance().Snapshot("mem");
    EXPECT_EQ(3, s.count);
    EXPECT_DOUBLE_EQ(50.0, s.min);
    EXPECT_DOUBLE_EQ(250.0, s.max);
    EXPECT_DOUBLE_EQ(50.0, s.last);
}

TEST_F(PerfMetricsTest, CountAddsUp) {
    PerfMetrics::GetInstance().RecordCount("trimmed");
    PerfMetrics::GetInstance().RecordCount("trimmed");
    PerfMetrics::GetInstance().RecordCount("trimmed", 5);

    const auto s = PerfMetrics::GetInstance().Snapshot("trimmed");
    EXPECT_EQ(7, s.count);
    EXPECT_DOUBLE_EQ(7.0, s.sum);
}

// PerfScope 的核心契约：析构即记录，不需要调用方记得写最后一行。
TEST_F(PerfMetricsTest, ScopeRecordsOnDestruction) {
    {
        PerfScope scope("scoped");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto s = PerfMetrics::GetInstance().Snapshot("scoped");
    EXPECT_EQ(1, s.count);
    EXPECT_GE(s.last, 1.0);   // 睡了 2ms，不会是 0
}

// Discard 的意义：被取消的流程不该把"半程耗时"混进正常样本，
// 否则取消频繁时平均值会被拉低，看起来比实际快。
TEST_F(PerfMetricsTest, DiscardedScopeRecordsNothing) {
    {
        PerfScope scope("discarded_scope");
        scope.Discard();
    }
    EXPECT_EQ(0, PerfMetrics::GetInstance().Snapshot("discarded_scope").count);
}

TEST_F(PerfMetricsTest, ScopeExposesLiveElapsed) {
    PerfScope scope("live");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_GE(scope.ElapsedMs(), 1.0);
    scope.Discard();
}

// ---- 取消延迟探针 ----

TEST_F(PerfMetricsTest, ProbeSettleBeforeArmIsMinusOne) {
    CancelLatencyProbe probe;
    EXPECT_FALSE(probe.Armed());
    EXPECT_DOUBLE_EQ(-1.0, probe.Settle());
    EXPECT_DOUBLE_EQ(-1.0, probe.ElapsedMs());
    EXPECT_EQ(0, PerfMetrics::GetInstance().Snapshot(videoeye::kMetricCancelSettleMs).count);
}

TEST_F(PerfMetricsTest, ProbeSettleOnce) {
    CancelLatencyProbe probe;
    probe.Arm();
    EXPECT_TRUE(probe.Armed());
    EXPECT_GE(probe.ElapsedMs(), 0.0);

    const double first = probe.Settle();
    ASSERT_GE(first, 0.0);   // 第一次有效

    // 重复终态必须被识别出来（-1），而不是再写一条假指标
    EXPECT_DOUBLE_EQ(-1.0, probe.Settle());
    EXPECT_EQ(1, PerfMetrics::GetInstance().Snapshot(videoeye::kMetricCancelSettleMs).count);
}

// 连点取消：以最后一次为准。用第一次会让"用户点了两次、第二次才生效"
// 的体验问题被算成"响应很快"。
TEST_F(PerfMetricsTest, ProbeRearmResetsClock) {
    CancelLatencyProbe probe;
    probe.Arm();
    probe.Arm();
    const double settle = probe.Settle();
    EXPECT_GE(settle, 0.0);
    EXPECT_LT(settle, 500.0);   // 中间没睡，不该被算成很久
}

TEST_F(PerfMetricsTest, ProbeAbandonStopsSettle) {
    CancelLatencyProbe probe;
    probe.Arm();
    probe.Abandon();
    EXPECT_FALSE(probe.Armed());
    EXPECT_DOUBLE_EQ(-1.0, probe.Settle());
    EXPECT_EQ(0, PerfMetrics::GetInstance().Snapshot(videoeye::kMetricCancelSettleMs).count);
}

// ---- 进程内存采样 ----

TEST_F(PerfMetricsTest, MemorySampleIsPositive) {
    const double rss = PerfMetrics::CurrentRssMb();
    const double peak = PerfMetrics::PeakRssMb();
    // 采样失败返回 0；本进程至少占几 MB，拿不到说明平台适配漏了。
    EXPECT_GT(rss, 0.0);
    EXPECT_GT(peak, 0.0);
}

TEST_F(PerfMetricsTest, PeakIsAtLeastCurrent) {
    // 峰值是进程启动以来的高水位，不可能低于当前驻留。
    // 这条断言兜的是"两个函数取了不同的量"（比如把虚拟内存当成了驻留）。
    EXPECT_GE(PerfMetrics::PeakRssMb(), PerfMetrics::CurrentRssMb());
}

// ---- 预算常量本身 ----

TEST_F(PerfMetricsTest, BudgetsAreSane) {
    // 响应预算必须远小于收敛预算：前者是主线程上的一次状态更新，
    // 后者要等后台退出。反过来写就等于"允许界面卡 3 秒"。
    EXPECT_LT(videoeye::kCancelResponseBudgetMs, videoeye::kCancelSettleBudgetMs);
    EXPECT_GT(videoeye::kOpenBudgetMs, 0.0);
    EXPECT_GT(videoeye::kReportExportBudgetMs, 0.0);
}

}  // namespace
