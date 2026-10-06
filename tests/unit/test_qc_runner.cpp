// QcRunner 生命周期与"超时 detach"的回归测试
//
// 背景（评审 P1-1 / P1-2 的落点）：
//   1) 旧实现：Box 里只放 AnalysisEngine 的裸指针，引擎本体挂在 AnalyzeFile 的局部
//      shared_ptr 上。join_budget_ms 到点后 worker 被 detach，而 AnalyzeFile 一返回
//      局部 shared_ptr 就析构 —— 后台线程随后访问的就是已释放的引擎（悬空访问）。
//      现在 Box 自持 shared_ptr<AnalysisEngine>。本文件用 join_budget_ms=0 把
//      "超时 → detach" 这条路径稳定打出来，并在返回后让测试继续存活一小段时间，
//      给被 detach 的 worker 留出收尾窗口 —— 这条用例就是 ASan 的回归落点
//      （回退成裸指针实现时，ASan 会在这里报 use-after-free）。
//   2) 旧实现"未获准（handle.valid()==false）仍会创建线程"：QcRunner 已改成无状态
//      同步执行器，不再自持 TaskManager，这条路径随之消失；调度拒绝语义由
//      TaskManager 的 BeginHandleRejectionReturnsFullyEmptyHandle 与并发句柄配对
//      测试覆盖（见 test_task_manager.cpp）。
//
// 为什么用"垃圾内容 + 媒体扩展名"的假文件触发超时：
//   IsAnalyzableFile 要求"存在 / 非空 / 不是目录"，而 AnalysisEngine 要真实走完
//   avformat_open_input（读盘 + 探测）才可能失败结算 —— 比主线程"到点检查预算"
//   的一步慢几个数量级，join_budget_ms=0 因此稳定走超时分支，且不依赖任何真实
//   媒体文件（CI 不需要媒体样本）。

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "core/qc/QcProfileMapper.h"
#include "core/qc/QcRunner.h"

namespace fs = std::filesystem;

using videoeye::qc::IsAnalyzableFile;
using videoeye::qc::OptionsForDepth;
using videoeye::qc::QcAnalysisDepth;
using videoeye::qc::QcAnalyzeRequest;
using videoeye::qc::QcProfile;
using videoeye::qc::QcRunCallbacks;
using videoeye::qc::QcRunResult;
using videoeye::qc::QcRunner;

namespace {

// AnalyzeFile 超时收尾的固定文案；用整串相等断言，防止未来改文案时静默漂移。
constexpr char kTimeoutError[] = "分析在等待预算内没有结束";

// 用 Fast 档：假文件本来打不开，档位只影响"打开成功后"的扫描深度。
QcProfile FastProfile() {
    QcProfile profile;
    profile.id = "unit-test";
    profile.name = "unit-test";
    profile.depth = QcAnalysisDepth::Fast;
    return profile;
}

// 建一个临时目录；同名目录先清掉，避免上次失败运行的残留干扰。
fs::path MakeScratchDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// 写一个"能过 IsAnalyzableFile 但 FFmpeg 一定打不开"的假媒体文件。
// 4 MiB 接近 FFmpeg 默认探测上限（5 MiB），把 open 失败前的读盘/探测拉得足够长：
// join_budget_ms=0 的"立刻检查预算"必定发生在引擎结算之前，超时分支才是确定的。
fs::path WriteGarbageMediaFile(const fs::path& dir, const std::string& file_name) {
    const fs::path file = dir / file_name;
    std::ofstream out(file, std::ios::binary);
    const std::string chunk = "VIDEOEYE-QC-RUNNER-NOT-A-MEDIA-FILE-0123456789\n";  // 无任何容器 magic
    const int chunk_count = (4 * 1024 * 1024) / static_cast<int>(chunk.size());
    for (int i = 0; i < chunk_count; ++i) {
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    out.close();
    return file;
}

}  // namespace

// 生命周期错误路径：这三种输入在起线程之前就被挡下，不创建盒子 / worker。
TEST(QcRunnerTest, RejectsMissingEmptyAndDirectoryPaths) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_lifecycle");
    QcRunner runner;
    const QcProfile profile = FastProfile();

    const QcRunResult missing = runner.AnalyzeFile((dir / "missing.mp4").string(), profile);
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("文件不存在"), std::string::npos);

    const fs::path empty = dir / "empty.mp4";
    std::ofstream(empty).close();
    const QcRunResult empty_result = runner.AnalyzeFile(empty.string(), profile);
    EXPECT_FALSE(empty_result.ok);
    EXPECT_NE(empty_result.error.find("文件为空"), std::string::npos);

    const QcRunResult directory = runner.AnalyzeFile(dir.string(), profile);
    EXPECT_FALSE(directory.ok);
    EXPECT_NE(directory.error.find("目录"), std::string::npos);

    // IsAnalyzableFile 是批量发现的前置闸门，直接断言它的边界行为。
    std::string reason;
    EXPECT_FALSE(IsAnalyzableFile("", reason));
    EXPECT_FALSE(reason.empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 正常结算路径（预算充足）：引擎自己失败 → 回调交回结果 → join 收线程，
// 不出现"预算内没有结束"。这条用例保证超时分支之外的主流程没被等待循环改坏。
TEST(QcRunnerTest, ReturnsEngineFailureWithinBudgetWithoutTimeout) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_within_budget");
    const fs::path file = WriteGarbageMediaFile(dir, "garbage.mp4");

    QcRunner runner;
    const QcRunResult result = runner.AnalyzeFile(file.string(), FastProfile(),
                                                  OptionsForDepth(QcAnalysisDepth::Fast),
                                                  QcRunCallbacks{}, /*join_budget_ms=*/8000);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error, kTimeoutError);  // 走的是结算路径，不是超时
    EXPECT_FALSE(result.error.empty());      // 失败原因来自引擎（FFmpeg 的打开错误）
    EXPECT_EQ(result.profile_id, "unit-test");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- 评审要求的核心回归：join_budget_ms = 0 的真超时 + detach ----
//
// 断言之外的关键：函数返回后测试不立刻结束，继续睡 ~1.5s —— 被 detach 的 worker
// 仍在跑（盒子由它自己保活）。在"引擎挂局部 shared_ptr"的旧实现下，这段时间里
// worker 访问 box->engine 就是 use-after-free，ASan 构建必报；现在的实现应当安静。
TEST(QcRunnerTest, JoinBudgetZeroTimesOutAndDetachesWithoutCrash) {
    const fs::path dir = MakeScratchDir("videoeye_qc_runner_timeout");
    const fs::path file = WriteGarbageMediaFile(dir, "garbage.mp4");

    QcRunner runner;
    const auto started = std::chrono::steady_clock::now();
    const QcRunResult result = runner.AnalyzeFile(file.string(), FastProfile(),
                                                  OptionsForDepth(QcAnalysisDepth::Fast),
                                                  QcRunCallbacks{}, /*join_budget_ms=*/0);
    const double wall_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started)
                               .count();

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, kTimeoutError);
    // 预算 0 = 一秒都不等：引擎打开垃圾文件也要毫秒级，主线程不能等它。
    EXPECT_LT(wall_ms, 1000.0);

    // ASan 回归窗口：给被 detach 的 worker 足够时间跑完 Run 并触发回调收尾。
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// MakeAnalyzeFunction 的封装只做参数绑定，错误路径同样在起线程前返回。
TEST(QcRunnerTest, MakeAnalyzeFunctionKeepsPathErrors) {
    const auto analyze = QcRunner::MakeAnalyzeFunction(
        FastProfile(), OptionsForDepth(QcAnalysisDepth::Fast));
    QcAnalyzeRequest request;
    request.path = (fs::temp_directory_path() / "videoeye_qc_runner_absent.mp4").string();
    std::error_code ec;
    fs::remove(request.path, ec);

    const QcRunResult result = analyze(request);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("文件不存在"), std::string::npos);
    EXPECT_EQ(result.profile_id, "unit-test");
}