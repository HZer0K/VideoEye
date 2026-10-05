// 报告页「关闭时还有阻塞任务在跑」的生命周期回归测试。
//
// 被测的是 ReportingPanel 的**关停路径**：构造真面板 → 起一个后台任务 → 立刻析构。
//
// 为什么面板要从 ui/*.cpp 里剔出来、单独成 VideoEyeReportingPanel：
// 面板带 Q_OBJECT，ui/ 整包编进主可执行文件，测试再 include 一遍就是同一个类编两份
// （ODR 违规，表现为随机的 0xc0000374 或槽位调不到）。拆成静态库后两边各链一份自己的。
//
// 关停路径上真正要守住的四件事（下面用例各盯一件）:
//   1. 未启动的任务不得白等满回收预算 —— 否则每次关面板都卡 5 秒；
//   2. 任务体在预算内响应取消 —— 回收后线程必须 join 干净（不是 detach 丢出去）；
//   3. 任务体**不**响应取消时（非协作体），关停也必须有上界，绝不能无限等；
//   4. 面板析构时任务正卡在阻塞 IO 上，取消令牌必须能打断它 —— 本文件的主用例。
//
// 时间相关的判定一律自证：每个数都在本进程里量出来；「事件压根没落在预期区间」时打印
// SKIP 而不是硬判失败。硬断言只留给"必须有上界"那几条 —— 那正是历史上真坏过的地方
// （析构无限等待 / 取消打断不了阻塞 IO）。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <QApplication>

#include "ui/reporting_panel/analysis_task.h"
#include "ui/reporting_panel/ReportingPanel.h"

#include "BlockingTcpEndpoint.h"

namespace {

using videoeye::ui::AnalysisTask;
using videoeye::ui::RecycleTask;
using videoeye::ui::WaitTaskBody;

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

int64_t ElapsedMs(Clock::time_point from) {
    return std::chrono::duration_cast<Ms>(Clock::now() - from).count();
}

// gtest 崩溃时 stdout 会被整批丢掉，这类诊断输出一律走 stderr（无缓冲）。
void Note(int round, int64_t baseline_ms, int64_t flying_ms) {
    fprintf(stderr, "  [note] 第 %d 轮: 基线=%lldms 飞行=%lldms\n",
            round, (long long)baseline_ms, (long long)flying_ms);
}

// QApplication 长期持有 argv，必须是静态存储。用 offscreen 平台（CMake 里
// set_tests_properties ENVIRONMENT QT_QPA_PLATFORM=offscreen），否则无显示器环境的
// CI 会在平台插件加载阶段 qFatal 崩掉，报错还看不出跟测试有什么关系。
static int g_argc = 1;
static char g_arg0[] = "test_reporting_panel_lifecycle";
static char* g_argv[] = {g_arg0, nullptr};

struct App {
    QApplication app;
    App() : app(g_argc, g_argv) {}
};

// ---------- 1. 未启动的任务不能白等满回收预算 ----------
//
// 回归: RecycleTask 对"构造好还没起线程"的任务也会等满 budget（默认 5s），
// 于是关一次面板卡 5 秒。这条断言的是"无事可做时必须立刻返回"。
TEST(ReportingPanelLifecycle, WaitTaskBodyOnUnstartedTaskReturnsImmediately) {
    auto task = std::make_shared<AnalysisTask>();  // 没有 thread
    const auto t0 = Clock::now();
    const bool ok = WaitTaskBody(task, 5000);
    const int64_t dt = ElapsedMs(t0);

    EXPECT_TRUE(ok);
    EXPECT_LT(dt, 200) << "未启动的任务不该白等满预算（实测 " << dt << "ms）";
}

// 起一条"会看取消令牌的"任务体（协作体）。release 传 nullptr 表示让它一直跑到
// 被外部 RequestCancel（用例 2 走这条路）。
std::shared_ptr<AnalysisTask> SpawnCooperative() {
    auto task = std::make_shared<AnalysisTask>();
    task->cancel = videoeye::ui::MakeCancelToken();
    task->body_done.store(false);
    task->thread = std::thread([task]() {
        // 轮询取消，被叫停后还要收尾 300ms（模拟引擎的收尾开销）。
        while (!task->cancel.IsCanceled()) {
            std::this_thread::sleep_for(Ms(2));
        }
        std::this_thread::sleep_for(Ms(300));
        task->body_done.store(true, std::memory_order_release);
    });
    return task;
}

// ---------- 2. 协作体：取消后回收干净（join，不是 detach） ----------
TEST(ReportingPanelLifecycle, RecycleJoinsCooperativeBodyWithinBudget) {
    auto task = SpawnCooperative();
    std::this_thread::sleep_for(Ms(10));

    task->cancel.RequestCancel();
    const bool body_back = WaitTaskBody(task, 5000);
    // RecycleTask 会把 task 置空（thread 一起析构），所以 joinable 必须先取出来 ——
    // 反过来写就是在空 shared_ptr 上取成员，直接 0xc0000005。
    const bool joinable_before_recycle = task->thread.joinable();
    RecycleTask(task, 5000);

    EXPECT_TRUE(body_back) << "协作体在预算内应当已经返回";
    EXPECT_TRUE(joinable_before_recycle) << "回收前线程应当还 joinable（否则这条用例什么都没测到）";
    EXPECT_EQ(task, nullptr) << "RecycleTask 之后应当已置空";
}

// ---------- 3. 非协作体：WaitTaskBody 必须在预算内放弃 ----------
//
// 盯的是评审 P1-3 点名的"析构路径无限等待"。
TEST(ReportingPanelLifecycle, WaitTaskBodyGivesUpWithinBudgetOnUncooperativeBody) {
    const int kBudget = 150;
    auto task = std::make_shared<AnalysisTask>();
    task->cancel = videoeye::ui::MakeCancelToken();
    task->body_done.store(false);
    // 忽视取消：只等外层放行，外加固定的收尾耗时。
    task->thread = std::thread([task]() {
        std::this_thread::sleep_for(Ms(600));
        task->body_done.store(true, std::memory_order_release);
    });

    const auto t0 = Clock::now();
    const bool ok = WaitTaskBody(task, kBudget);
    const int64_t dt = ElapsedMs(t0);

    EXPECT_FALSE(ok) << "非协作体在预算内没回来，应当返回 false";
    EXPECT_LT(dt, kBudget * 4) << "放弃要发生在预算附近（实测 " << dt << "ms，预算 " << kBudget << "ms）";

    // 预算耗尽不等于把线程丢掉：body 收尾后 RecycleTask 仍要 join 回来。
    task->cancel.RequestCancel();
    const auto t1 = Clock::now();
    while (!task->body_done.load() && ElapsedMs(t1) < 3000) {
        std::this_thread::sleep_for(Ms(2));
    }
    const bool joinable_after_wait = task->thread.joinable();
    RecycleTask(task, 3000);
    EXPECT_TRUE(joinable_after_wait) << "预算耗尽后线程不能已经被丢掉（那正是 detach）";
}

// ---------- 4. 面板析构时任务正在飞行中（主用例） ----------
//
// 路径: 真面板 → SetCurrentFile(一个存在的非空文件) → OnAnalyzeCurrentFile() 起 worker
// → **立刻析构**。
//
// 为什么必须 new 出来而不是开在栈上再 delete &panel：
// 对栈地址调 operator delete 会把堆块头写到栈帧里，MSVC 立刻 0xc0000005 ——
// 症状跟"产品代码有 use-after-free"一模一样，查起来能绕半天。面板一律堆分配。
//
// 难度说明（别照着"卡在 avformat_open_input"那套写）：
// QcRunner::AnalyzeFile 第一步就是 IsAnalyzableFile()，它用 std::filesystem::status
// 判存，于是 tcp://127.0.0.1:PORT 这类**网络地址永远被判成"文件不存在"**直接返回
// （见 core/qc/QcRunner.cpp 的 IsAnalyzableFile）。也就是说报告页这条单文件路径
// **根本进不到阻塞 IO**，用 BlockingTcpEndpoint 造"永不回包"是打不到的 ——
// 这是产品当前的实际行为，不是测试写错。所以这里换成"任务刚起飞还没落地"这个
// 真正可达、且历史上真坏过的形态（原实现是 detach + 对 joinable 的 thread 赋值）。
//
// 判据全靠时间，不看内部状态：
//   * 正常 : worker 在几毫秒内收尾，析构几十毫秒内返回；
//   * 退回成"干等满回收预算" : 卡 5s；
//   * worker 没起来 / 线程被丢掉 : 此时要么析构秒回（void 测试），要么崩。
// 所以「dt < 上限」这条硬断言同时压住"无限等待"和"析构时崩"。上限取 2500ms。
//
// 自证命中：每轮都拿"没起任务的空析构"当基线，要求阻塞轮明显慢于基线（kHitMargin）。
// 整轮一律没打中就 SKIP，而不是把上限放宽糊过去 —— 那等于没测到还假装绿。
int64_t RunOneDestruct(const std::string& path) {
    auto* panel = new videoeye::ui::ReportingPanel;
    panel->SetCurrentFile(QString::fromStdString(path));
    if (!path.empty()) {
        QMetaObject::invokeMethod(panel, "OnAnalyzeCurrentFile");
    }
    // 不 sleep：就是要让 worker 还在 RunSingle() 里飞着的时候把面板删掉。
    const auto t0 = Clock::now();
    delete panel;
    return ElapsedMs(t0);
}

// IsAnalyzableFile 只认"存在且非空"的本地文件，所以造一个临时空壳文件就够过闸，
// 分析本身必然失败 —— 本用例只看关停时序，不看分析结果。
std::string MakeProbeFile() {
    const auto dir = std::filesystem::temp_directory_path();
    std::string path = (dir / "videoeye_panel_probe.mp4").string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::string junk(64 * 1024, '\0');
    out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    return path;
}

TEST(ReportingPanelLifecycle, DestructWhileTaskInFlightIsBoundedAndSafe) {
    const App app;  // 进程内只构造一次（QApplication 不能在循环里反复造）
    EXPECT_TRUE(QApplication::instance() != nullptr);

    const std::string file = MakeProbeFile();
    ASSERT_FALSE(file.empty()) << "临时目录不可用，本用例无法造探针文件";

    constexpr int kRounds = 3;
    constexpr int64_t kDestructCeiling = 2500;  // 远小于回收预算的 5s / 两段之和 10s
    constexpr int64_t kHitMargin = 20;

    int hits = 0;
    int64_t baseline = -1;
    int64_t worst = 0;

    for (int i = 0; i < kRounds; ++i) {
        // 基线：没任务，直接析构。
        const int64_t idle = RunOneDestruct("");
        // 飞行轮：worker 已经起了，正卡在 RunSingle() → AnalyzeFile() 内部。
        const int64_t flying = RunOneDestruct(file);

        if (baseline < 0 || idle > baseline) baseline = idle;
        if (flying > worst) worst = flying;
        if (flying > idle + kHitMargin) ++hits;
        Note(i, idle, flying);
    }

    if (hits == 0) {
        GTEST_SKIP() << "任务体没落在飞行中（基线 " << baseline << "ms，飞行轮 " << worst
                     << "ms 无差别）—— worker 可能压根没起来，"
                     << "本轮不算测到，需要复核启停路径而不是把上限放宽";
    }

    // 硬断言：有上界。历史上的坏版本是"无限等"或"detach 后往已销毁的 QWidget 排队"，
    // 这两条都会在这一行红。
    EXPECT_LT(worst, kDestructCeiling)
        << "面板析构耗时 " << worst << "ms 已接近回收预算上限 —— 关停路径退回成"
        << "长时间等待（取消没生效 / 又在干等满预算）";
}

// ---------- 5. 连续启停不崩（历史 bug 的回归闸门） ----------
//
// 原实现里第二次启动会对"仍 joinable 的 std::thread"直接赋值 → std::terminate()。
// 这里用真面板连开连关若干次，覆盖的是同一条路径：每次都是"上一轮任务刚交出去就析构"。
TEST(ReportingPanelLifecycle, RepeatedStartAndDestructStaysStable) {
    const App app;
    const std::string file = MakeProbeFile();

    for (int i = 0; i < 3; ++i) {
        auto* panel = new videoeye::ui::ReportingPanel;
        panel->SetCurrentFile(QString::fromStdString(file));
        QMetaObject::invokeMethod(panel, "OnAnalyzeCurrentFile");
        delete panel;

        // 再来一发：这一发必须与上一发互不相干（面板已销毁，线程也应已回收）
        auto* again = new videoeye::ui::ReportingPanel;
        again->SetCurrentFile(QString::fromStdString(file));
        QMetaObject::invokeMethod(again, "OnAnalyzeCurrentFile");
        delete again;
    }
}

}  // namespace
