// QtAnalysisController 并发取消压力测试（第三批：并发与取消压力）
//
// 已有的 test_qt_analysis_controller 覆盖了「单次取消」「取消后重启」「失败后重启」
// 这些**一次一个动作**的路径。真实用户的手速不是这样的：
//   * 取消按钮会被连点好几下；
//   * 上一轮还没退出来时又点「开始分析」，于是请求进入 pending_，随后又被取消；
//   * 切文件/关窗口时宿主对象直接析构，而工作线程还在阻塞 IO 里。
//
// 这些路径的共同风险是「终态只还一次」这条协议被并发踩坏：重复取消把 slot 写成
// Canceled 之后旧任务又补写 Succeeded；排队请求被取消后仍然被 WorkerExited 启动；
// 宿主析构时没人回收线程。单测盯的就是这几种"界面永远停在分析中/关窗口卡住"的现场。
//
// 本文件只测 QtAnalysisController（UI 线程侧的调度），不重复 TaskManager 的协议测试。

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#include "SanitizerSkip.h"

#include "core/qt/QtAnalysisController.h"
#include "infrastructure/logging/PerfMetrics.h"

#include "BlockingTcpEndpoint.h"

namespace {

using namespace videoeye;

QString WriteManifest(const QString& name) {
    const QString path = QDir::tempPath() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write("#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-TARGETDURATION:4\n"
            "#EXT-X-MEDIA-SEQUENCE:0\n"
            "#EXTINF:4.0,\n"
            "seg0.ts\n"
            "#EXTINF:4.0,\n"
            "seg1.ts\n"
            "#EXT-X-ENDLIST\n");
    f.close();
    return path;
}

// 收集终态（成功/失败都算）。协议保证每次**启动过**的分析只产生一个终态；
// 这里连重复终态一起记下来，重复本身就是断言要抓的问题。
struct TerminalSink {
    std::vector<quint64> generations;

    void Record(quint64 gen) {
        generations.push_back(gen);
    }
    bool Saw(quint64 gen) const {
        return std::find(generations.begin(), generations.end(), gen) != generations.end();
    }
    int CountOf(quint64 gen) const {
        return static_cast<int>(std::count(generations.begin(), generations.end(), gen));
    }
};

void WireSink(videoeye::qt::QtAnalysisController& controller, TerminalSink& sink) {
    QObject::connect(
        &controller, &videoeye::qt::QtAnalysisController::AnalysisFinished, &controller,
        [&sink](quint64 gen, bool, const std::shared_ptr<const model::AnalysisResult>&) { sink.Record(gen); });
    QObject::connect(&controller, &videoeye::qt::QtAnalysisController::AnalysisFailed, &controller,
                     [&sink](quint64 gen, const QString&) { sink.Record(gen); });
}

bool PumpUntil(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

void EnsureApp() {
    static int argc = 1;
    static char arg0[] = "test_controller_cancel_stress";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance())
        new QCoreApplication(argc, argv);
}

// 取消的收敛上限：与生产预算 kCancelSettleBudgetMs 对齐，再留一点 CI 抖动余量。
constexpr int kSettleLimitMs = 5000;

} // namespace

// ---------------------------------------------------------------------------
// 1. 飞行中连点取消：多个 Cancel 只能产生一个终态
//
// 用户连点取消按钮时，Cancel() 会在 UI 线程上被调多次。TaskManager::Cancel 本身
// 是幂等的，但"取消 -> 工作线程回包 -> 宿主归还终态"这条链上任何一处把终态当成
// 可覆盖的，就会出现 Canceled 之后又冒出一个 Succeeded/Failed。这里断言：
//   * 终态信号恰好一个；
//   * 协议里的终态是 Canceled；
//   * 收敛时间落在预算内。
// ---------------------------------------------------------------------------
TEST(ControllerCancelStress, RepeatedCancelSettlesExactlyOnce) {
    EnsureApp();

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    videoeye::qt::QtAnalysisController controller;
    TerminalSink sink;
    WireSink(controller, sink);

    const quint64 gen = controller.StartAnalysis(endpoint.tcp_url());
    ASSERT_GT(gen, 0u);
    ASSERT_TRUE(PumpUntil([&] { return controller.IsRunning(); }, 3000)) << "分析没有进入运行态，无法验证取消路径";
    // 端口若瞬间回包，说明本用例没测到"阻塞中的取消"，跳过而不是放宽判定。
    if (PumpUntil([&] { return sink.Saw(gen); }, 200)) {
        GTEST_SKIP() << "阻塞输入瞬间就回包了（端口未真正阻塞），本轮不算测到取消路径";
    }

    const auto cancel_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i)
        controller.Cancel();

    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen); }, kSettleLimitMs))
        << "连点取消后没有在预算内回到终态（gen=" << gen << "）";
    const auto settle_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cancel_start).count();

    EXPECT_EQ(sink.CountOf(gen), 1) << "同一代际产生了 " << sink.CountOf(gen) << " 个终态信号";
    EXPECT_EQ(sink.generations.size(), 1u) << "连点取消不该凭空多出一个终态（总数=" << sink.generations.size() << "）";
    EXPECT_EQ(controller.terminal_state(), task::TaskState::Canceled)
        << "连点取消后的终态必须是 Canceled，实际是 " << task::TaskStateName(controller.terminal_state());
    EXPECT_LT(settle_ms, kSettleLimitMs) << "取消到终态耗时 " << settle_ms << "ms，超过预算";
    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 3000)) << "取消收敛后 IsRunning() 仍为 true";
}

// ---------------------------------------------------------------------------
// 2. 取消排队中的请求：旧线程退出后它绝不能"复活"
//
// 现场：上一轮卡在阻塞 IO，用户又点了开始分析（请求进 pending_），然后点取消。
// Cancel() 会作废 pending_ 并置位 slot 令牌。旧线程随后退出时，
// OnWorkerExited 必须看到"没有排队请求"，绝不能再 Launch 一次。
// 否则用户点的是取消，界面却重新开始扫描 —— 而且这个新任务没有任何人期待它的结果。
// ---------------------------------------------------------------------------
TEST(ControllerCancelStress, CanceledPendingRequestNeverStarts) {
    EnsureApp();

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    const QString manifest = WriteManifest("videoeye_cancel_stress_pending.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    videoeye::qt::QtAnalysisController controller;
    TerminalSink sink;
    WireSink(controller, sink);

    const quint64 gen_running = controller.StartAnalysis(endpoint.tcp_url());
    ASSERT_GT(gen_running, 0u);
    ASSERT_TRUE(PumpUntil([&] { return controller.IsRunning(); }, 3000)) << "分析没有进入运行态，无法验证排队路径";
    if (PumpUntil([&] { return sink.Saw(gen_running); }, 200)) {
        GTEST_SKIP() << "阻塞输入瞬间就回包了（端口未真正阻塞），本轮不算测到排队取消路径";
    }

    // 旧线程还在跑：这次请求只能进 pending_，但代际号立刻分配。
    const quint64 gen_pending = controller.StartAnalysis(manifest.toStdString());
    ASSERT_GT(gen_pending, gen_running) << "排队请求的代际号必须单调递增";
    EXPECT_TRUE(controller.IsRunning()) << "有排队请求时 IsRunning() 必须为 true";

    controller.Cancel(); // 作废排队请求 + 置位运行中任务的令牌

    ASSERT_TRUE(PumpUntil([&] { return sink.Saw(gen_running); }, kSettleLimitMs))
        << "取消后运行中的分析没有在预算内收敛（gen=" << gen_running << "）";
    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, kSettleLimitMs));

    // 给 WorkerExited 一点时间把排队的收尾消息消费完；若它错误地启动了 gen_pending，
    // 这里就能看到它的终态信号。
    PumpUntil([&] { return false; }, 300);

    EXPECT_FALSE(sink.Saw(gen_pending)) << "排队请求已被取消，旧线程退出后它仍然被启动了（gen=" << gen_pending << "）";
    EXPECT_EQ(sink.CountOf(gen_pending), 0);
    EXPECT_EQ(controller.terminal_state(), task::TaskState::Canceled)
        << "slot 终态必须是 Canceled，实际是 " << task::TaskStateName(controller.terminal_state());

    QFile::remove(manifest);
}

// ---------------------------------------------------------------------------
// 3. 快速开始/取消循环：反复顶替后必须回到空闲，且代际单调
//
// 每次 StartAnalysis 都会先 Cancel 上一轮（TaskManager::Begin 的语义），所以这个
// 循环同时在压「取消」与「排队顶替」两条路径。它不检查每一次都成功 —— 取消本来
// 就可能先于分析完成 —— 只检查不崩、不挂、代际不重复、最后一定回到空闲。
// ---------------------------------------------------------------------------
TEST(ControllerCancelStress, RapidStartCancelCyclesReturnToIdle) {
    EnsureApp();

    const QString manifest = WriteManifest("videoeye_cancel_stress_rapid.m3u8");
    ASSERT_FALSE(manifest.isEmpty());

    videoeye::qt::QtAnalysisController controller;
    TerminalSink sink;
    WireSink(controller, sink);

    constexpr int kRounds = 20;
    std::vector<quint64> generations;
    for (int round = 0; round < kRounds; ++round) {
        const quint64 gen = controller.StartAnalysis(manifest.toStdString());
        ASSERT_GT(gen, 0u) << "第 " << round << " 轮启动返回 0（并发预算异常耗尽）";
        generations.push_back(gen);
        controller.Cancel();
        QCoreApplication::processEvents(); // 让排队信号有推进机会，但不等待收敛
    }

    ASSERT_TRUE(PumpUntil([&] { return !controller.IsRunning(); }, 30000))
        << kRounds << " 轮开始/取消后没有回到空闲状态（界面会停在'取消中'）";

    for (size_t i = 1; i < generations.size(); ++i) {
        EXPECT_GT(generations[i], generations[i - 1]) << "第 " << i << " 轮的代际号没有严格递增";
    }
    EXPECT_TRUE(task::IsTerminalState(controller.terminal_state()))
        << "收敛后 slot 必须落在终态，实际是 " << task::TaskStateName(controller.terminal_state());

    // 每个终态信号都必须属于某一轮真实启动过的代际 —— 不能有串号的回包。
    for (const quint64 gen : sink.generations) {
        EXPECT_NE(std::find(generations.begin(), generations.end(), gen), generations.end())
            << "收到了未启动代际 " << gen << " 的终态信号";
    }
    EXPECT_LE(sink.generations.size(), generations.size()) << "终态信号数不能超过实际启动次数";

    QFile::remove(manifest);
}

// ---------------------------------------------------------------------------
// 4. 分析飞行中析构宿主：关窗口/切文件时必须走完 Shutdown，而不是卡住或崩
//
// QtAnalysisController 的析构是全类里唯一"没有界面可等"的路径：先取消、在
// kJoinBudgetMs 内等线程体返回，超预算按 BlockingIo 契约 detach（盒子自持活）。
// 这里用阻塞端口让线程体真的停在 IO 上，然后直接销毁对象：
//   * 销毁必须在预算内返回（否则关窗口就是卡死）；
//   * 不能崩（线程体不能碰已销毁的 QObject —— alive_ / QPointer 这两层挡箭牌）。
// TSan 下跳过：libtsan 的线程注册表对"宿主先析构、线程后被放弃"这种 DETACH 场景
// 有已知的 CHECK 失败风险（见 SanitizerSkip.h 的说明），与业务缺陷无关。
// ---------------------------------------------------------------------------
TEST(ControllerCancelStress, DestroyWhileBlockingOpenReturnsWithinBudget) {
    SKIP_UNDER_TSAN("libtsan 对 detach 后仍在收尾的线程有已知 CHECK 失败风险");

    EnsureApp();

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    auto controller = std::make_unique<videoeye::qt::QtAnalysisController>();
    TerminalSink sink;
    WireSink(*controller, sink);

    const quint64 gen = controller->StartAnalysis(endpoint.tcp_url());
    ASSERT_GT(gen, 0u);
    ASSERT_TRUE(PumpUntil([&] { return controller->IsRunning(); }, 3000)) << "分析没有进入运行态，无法验证析构路径";
    if (PumpUntil([&] { return sink.Saw(gen); }, 200)) {
        GTEST_SKIP() << "阻塞输入瞬间就回包了（端口未真正阻塞），本轮不算测到析构取消路径";
    }

    const auto destroy_start = std::chrono::steady_clock::now();
    controller.reset(); // 析构：Cancel -> AwaitWorker(3000) -> 超预算 detach
    const auto destroy_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - destroy_start).count();

    // 正常路径实测 ~130ms；漏还终态时 TaskManager 析构的 WaitForAll(5000) 会让这里
    // 涨到 5s 以上。阈值取 4s：既能稳稳接住回归，又给慢机器留足余量。
    EXPECT_LT(destroy_ms, videoeye::kCancelSettleBudgetMs + 1000)
        << "分析飞行中析构耗时 " << destroy_ms << "ms，关闭窗口会被用户感知为卡死";
}
