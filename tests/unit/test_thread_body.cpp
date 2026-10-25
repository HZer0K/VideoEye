// 线程体统一收口的单测（core/domain/task/ThreadBody.h + TaskProtocol.h）。
//
// 这一组用例回答的是"用户为什么该相信所有线程入口都会收尾"：
//   * GuardBody     —— 异常不再越过 noexcept 边界；
//   * ResolveTerminal —— 终态判定只有一处定义（取消优先于异常）；
//   * ScopeExit     —— 提前 return / 抛异常都会置位关键标志；
//   * 真线程集成    —— 从 std::thread 里抛异常不会带走进程。
//
// 源文件的位置有讲究：ThreadBody.h 必须与 TaskProtocol.h 一起待在 core/domain/task，
// 否则 core/qc / core/qt / core/player 按分层规则看不见它 —— 统一就只能是口头上的。
// 本测试顺带把这个"能被所有层 include"的事实编译一遍。

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "core/domain/task/ThreadBody.h"

namespace {

// 用具名 using 而不是 `namespace videoeye { namespace task { ... } }`：
// audit_namespace_layout.py 的 R3 要求 videoeye 的具名子命名空间与所在目录同名，
// 而本文件在 tests/unit（同类测试如 test_task_manager.cpp 也走这个形态）。
using videoeye::task::CancelToken;
using videoeye::task::GuardBody;
using videoeye::task::IsTerminalState;
using videoeye::task::MakeScopeExit;
using videoeye::task::ResolveTerminal;
using videoeye::task::RunTerminalBody;
using videoeye::task::TaskState;
using videoeye::task::TaskStateName;

TEST(ThreadBodyProtocolTests, TerminalStatesAreRecognized) {
    EXPECT_FALSE(IsTerminalState(TaskState::Idle));
    EXPECT_FALSE(IsTerminalState(TaskState::Running));
    EXPECT_TRUE(IsTerminalState(TaskState::Succeeded));
    EXPECT_TRUE(IsTerminalState(TaskState::Failed));
    EXPECT_TRUE(IsTerminalState(TaskState::Canceled));
    // TimedOut 是第五种终态：宿主预算耗尽。以前各处用 bool / 独立枚举自己记，
    // 混进 Failed 之后上层分不清"要不要重试"与"有没有后台线程还在收尾"。
    EXPECT_TRUE(IsTerminalState(TaskState::TimedOut));
    EXPECT_STREQ(TaskStateName(TaskState::TimedOut), "TimedOut");
}

TEST(ThreadBodyProtocolTests, GuardBodyReturnsEmptyOnNormalReturn) {
    int ran = 0;
    const std::optional<std::string> failure = GuardBody([&]() { ++ran; });
    EXPECT_EQ(ran, 1);
    EXPECT_FALSE(failure.has_value());
}

TEST(ThreadBodyProtocolTests, GuardBodyCapturesStdExceptionMessage) {
    const std::optional<std::string> failure = GuardBody([&]() { throw std::runtime_error("boom"); });
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(*failure, "boom");
}

TEST(ThreadBodyProtocolTests, GuardBodyCapturesNonStdThrowables) {
    // throw 1 / throw "..." 这类也会被 GuardBody 收进来：拿不到 what()，但同样
    // 不许越过 noexcept 边界 —— 越过就是 std::terminate()，整进程被带走。
    const std::optional<std::string> failure = GuardBody([&]() { throw 42; });
    ASSERT_TRUE(failure.has_value());
    EXPECT_FALSE(failure->empty());
}

TEST(ThreadBodyProtocolTests, ResolveTerminalPrefersCancellationOverFailure) {
    auto make_token = [](bool canceled) {
        const auto flag = std::make_shared<std::atomic<bool>>(canceled);
        return CancelToken(flag);
    };
    const std::optional<std::string> failed(std::string("x"));
    // 取消优先：任务体被中断时常表现为"包着异常退出"，那种情况该显示已取消而不是报错。
    EXPECT_EQ(ResolveTerminal(failed, make_token(true)), TaskState::Canceled);
    EXPECT_EQ(ResolveTerminal(failed, make_token(false)), TaskState::Failed);
    EXPECT_EQ(ResolveTerminal(std::nullopt, make_token(true)), TaskState::Canceled);
    EXPECT_EQ(ResolveTerminal(std::nullopt, make_token(false)), TaskState::Succeeded);
    // 空令牌（默认构造）没有标志，IsCanceled() 恒假 —— 这是有意保留的行为。
    EXPECT_EQ(ResolveTerminal(std::nullopt, CancelToken{}), TaskState::Succeeded);
}

TEST(ThreadBodyProtocolTests, RunTerminalBodyCombinesGuardAndVerdict) {
    const auto flag = std::make_shared<std::atomic<bool>>(false);
    const CancelToken token(flag);
    EXPECT_EQ(RunTerminalBody(token, []() {}), TaskState::Succeeded);
    EXPECT_EQ(RunTerminalBody(token, []() { throw std::logic_error("no"); }), TaskState::Failed);
    flag->store(true);
    EXPECT_EQ(RunTerminalBody(token, []() { throw std::logic_error("no"); }), TaskState::Canceled);
}

TEST(ThreadBodyProtocolTests, ScopeExitRunsOnNormalAndExceptionalExit) {
    int calls = 0;
    {
        const auto settle = MakeScopeExit([&]() { ++calls; });
        EXPECT_EQ(calls, 0);
    }
    EXPECT_EQ(calls, 1);

    int flagged = 0;
    const std::optional<std::string> failure = GuardBody([&]() {
        const auto settle = MakeScopeExit([&]() { ++flagged; });
        throw std::runtime_error("leave early");
    });
    ASSERT_TRUE(failure.has_value());
    // 这正是"线程体抛异常后忘了置完成标志"的兜底：标志到位，回收路径不会干等满预算。
    EXPECT_EQ(flagged, 1);
}

TEST(ThreadBodyProtocolTests, RealThreadBodySurvivesThrownException) {
    // 集成意义最强的一条：不套 GuardBody 的话，这一句就把整个测试进程带走了。
    std::atomic<bool> body_done{false};
    std::thread worker([&]() {
        const auto settle = MakeScopeExit([&]() { body_done.store(true); });
        GuardBody([&]() { throw std::runtime_error("from worker"); });
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!body_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(body_done.load());
    ASSERT_TRUE(worker.joinable());
    worker.join(); // 能走到这里就说明线程体正常退出，没有 terminate
}

} // namespace
