#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/qc/BatchQcRunner.h"

using videoeye::qc::BatchItemStatus;
using videoeye::qc::BatchQcCallbacks;
using videoeye::qc::BatchQcItem;
using videoeye::qc::BatchQcOptions;
using videoeye::qc::BatchQcRunner;
using videoeye::qc::QcAnalyzeFn;
using videoeye::qc::QcAnalyzeRequest;
using videoeye::qc::QcRunResult;

namespace {
// 假分析函数：睡 ~20ms 模拟解码，期间轮询 cancel 标记。
// 记录被调用的路径，便于校验取消后没被继续派发。
QcAnalyzeFn MakeFakeAnalyze(std::vector<std::string>* called,
                            std::mutex* called_mutex,
                            std::atomic<int>* max_active,
                            std::atomic<int>* active) {
    return [called, called_mutex, max_active, active](const QcAnalyzeRequest& req) {
        active->fetch_add(1, std::memory_order_acq_rel);
        const int now = active->load();
        int prev = max_active->load();
        while (now > prev && !max_active->compare_exchange_weak(prev, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard<std::mutex> lock(*called_mutex);
            called->push_back(req.path);
        }
        active->fetch_sub(1, std::memory_order_acq_rel);

        QcRunResult result;
        result.ok = true;
        result.report.score = 100.0;
        result.report.verdict = "通过";
        return result;
    };
}

TEST(BatchQcRunnerTest, DiscoverFiltersByExtension) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      "videoeye_batch_discover_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::filesystem::create_directories(dir / "sub", ec);
    std::ofstream(std::filesystem::path(dir / "a.mp4"));
    std::ofstream(std::filesystem::path(dir / "b.MOV"));
    std::ofstream(std::filesystem::path(dir / "c.txt"));
    std::ofstream(std::filesystem::path(dir / "sub" / "d.mkv"));

    // 不递归 + 过滤 mp4/mov
    auto items = BatchQcRunner::Discover(dir.string(), /*recursive=*/false,
                                         {"mp4", "mov"});
    EXPECT_EQ(items.size(), 2u);

    // 递归 + 不过滤
    auto all = BatchQcRunner::Discover(dir.string(), /*recursive=*/true, {});
    EXPECT_EQ(all.size(), 4u);

    // 空扩展名列表 = 全部放行
    auto nofilter = BatchQcRunner::Discover(dir.string(), /*recursive=*/false, {});
    EXPECT_EQ(nofilter.size(), 3u);  // a.mp4 / b.MOV / c.txt

    std::filesystem::remove_all(dir, ec);
}

TEST(BatchQcRunnerTest, RunsAllAndJoinsThreads) {
    std::vector<std::string> called;
    std::mutex m;
    std::atomic<int> max_active{0};
    std::atomic<int> active{0};
    auto analyze = MakeFakeAnalyze(&called, &m, &max_active, &active);

    std::vector<BatchQcItem> items;
    for (int i = 0; i < 12; ++i) {
        BatchQcItem it;
        it.path = "file" + std::to_string(i) + ".mp4";
        items.push_back(it);
    }

    BatchQcRunner runner;
    runner.Run(items, BatchQcOptions{}, analyze, BatchQcCallbacks{});

    // Run() 返回即代表所有 worker 已 join —— 没有任何后台线程残留（12.6 要求）。
    EXPECT_EQ(called.size(), 12u);
    EXPECT_LE(max_active.load(), 4);  // 默认 max_parallel = 4
    EXPECT_EQ(active.load(), 0);      // 离开 Run 后没有线程还在跑
}

TEST(BatchQcRunnerTest, CancelStopsDispatchAndReportsStatus) {
    std::vector<std::string> called;
    std::mutex m;
    std::atomic<int> max_active{0};
    std::atomic<int> active{0};
    auto analyze = MakeFakeAnalyze(&called, &m, &max_active, &active);

    std::vector<BatchQcItem> items;
    for (int i = 0; i < 100; ++i) {
        BatchQcItem it;
        it.path = "file" + std::to_string(i) + ".mp4";
        items.push_back(it);
    }

    std::atomic<bool> cancel_flag{false};
    BatchQcCallbacks callbacks;
    callbacks.is_cancelled = [&cancel_flag]() { return cancel_flag.load(); };

    std::atomic<bool> first_done{false};
    callbacks.item_finished = [&cancel_flag, &first_done](const auto&) {
        if (!first_done.exchange(true)) {
            // 第一个文件一完成就取消
            cancel_flag.store(true);
        }
    };

    BatchQcOptions options;
    options.max_parallel = 4;

    BatchQcRunner runner;
    const auto run = runner.Run(items, options, analyze, callbacks);

    // 取消后不应把 100 个全跑完
    EXPECT_LT(called.size(), 100u);
    // 取消后没跑到的应落成 Cancelled / 已完成的为 Succeeded
    EXPECT_GT(run.summary.cancelled, 0);
    // 关键：Run 返回时所有线程已结束（无残留）
    EXPECT_EQ(active.load(), 0);
}

// 分析成功但导出失败时，必须标记为 ExportFailed 而非 Succeeded（P0#2 的收口）：
// "分析成功 / 导出失败" 要能区分，汇总里按失败计数。
QcAnalyzeFn MakeResultAnalyze(bool ok, bool export_failed) {
    return [ok, export_failed](const QcAnalyzeRequest& req) {
        QcRunResult r;
        r.ok = ok;
        r.report.score = 100.0;
        r.report.verdict = "通过";
        r.output_path = req.path + ".json";
        r.export_failed = export_failed;
        return r;
    };
}

TEST(BatchQcRunnerTest, SucceededVsExportFailed) {
    std::vector<BatchQcItem> items;
    BatchQcItem it;
    it.path = "file.mp4";
    items.push_back(it);

    {
        BatchQcRunner runner;
        const auto run = runner.Run(items, BatchQcOptions{}, MakeResultAnalyze(true, false), {});
        EXPECT_EQ(run.items[0].status, BatchItemStatus::Succeeded);
        EXPECT_EQ(run.summary.failed, 0);
        EXPECT_EQ(run.summary.succeeded, 1);
    }
    {
        BatchQcRunner runner;
        const auto run = runner.Run(items, BatchQcOptions{}, MakeResultAnalyze(true, true), {});
        EXPECT_EQ(run.items[0].status, BatchItemStatus::ExportFailed);
        EXPECT_EQ(run.summary.failed, 1);  // 导出失败计入失败
        EXPECT_EQ(run.summary.succeeded, 0);
    }
    {
        BatchQcRunner runner;
        const auto run = runner.Run(items, BatchQcOptions{}, MakeResultAnalyze(false, false), {});
        EXPECT_EQ(run.items[0].status, BatchItemStatus::Failed);
        EXPECT_EQ(run.summary.failed, 1);
    }
}
}  // namespace
