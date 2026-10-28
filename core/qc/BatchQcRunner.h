#pragma once

// 批量 QC 扫描
//
// 只负责任务调度：目录递归发现 → 扩展名过滤 → 有限并发执行 → 取消 → 汇总。
// 报告怎么写、写在哪一概不关心（那是 QcReportExporter 与调用方的事），
// 这样这个模块可以用一个"睡 50 毫秒就返回"的假分析函数做单测，不必准备任何媒体文件。
//
// 取消语义（12.6 要求"取消后不遗留后台线程"）：
//   Cancel() 只置标记 —— worker 每取一个任务前检查一次，正在跑的那个则由
//   QcAnalyzeRequest::cancel 传进分析函数内部；Run() 返回前必定 join 完所有 worker。

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/qc/BatchQcResults.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcReportFormat.h"
#include "core/qc/QcRunBudgets.h"

namespace videoeye {
namespace qc {

struct BatchQcItem {
    std::string path;
    int64_t file_size_bytes = 0;
};

struct BatchQcOptions {
    bool recursive = true;
    std::vector<std::string> extensions; // 无点小写；空 = 不按扩展名过滤
    int max_parallel = 4;                // 并发上限，会再夹到 [1, 16]
    int64_t max_file_size_bytes = 0;     // 0 = 不限制；超限记 Skipped
    bool keep_reports = true;            // false = 结果里只留计数，不留 QcReport 本体

    // ---- 单文件的三项等待策略（见 QcRunBudgets.h）----
    // 单个文件允许的**运行时长**（毫秒）。<=0 = 不限时（由下面两项兜底）。
    // 默认 30 秒：批量场景里"某一个文件卡住"必须能被跳过，否则整批挂着不动；
    // 想让长文件跑完就显式调大或设 0 —— 这是业务配置，不该在别处埋魔法值。
    // 超预算的条目落成 BatchItemStatus::TimedOut。
    int per_item_timeout_ms = 30000;
    // 多久没有任何动静就算卡住（毫秒）。0 = 不判空闲。与总时长无关：跑了 10 分钟但
    // 一直在出进度不算卡住，打开后再无回音则会被及时放弃。
    int io_idle_timeout_ms = 0;
    // 判定放弃（取消 / 超预算 / 无响应）之后最多再等线程体多久（毫秒）。
    int cancel_wait_ms = kDefaultCancelWaitMs;

    // ---- 并发额度（评审 P2-3）----
    // 正在退出（已判定放弃、线程体还没退）的分析任务数上限。<=0 = 取 max_parallel。
    // 一条被 detach 的分析线程在真正退出前仍占着 FFmpeg / 内存 / 磁盘，放任它们堆积
    // 等于"并发上限形同虚设"，所以这里单独设一道闸。
    int max_exiting_tasks = 0;
    // 额度不够时最多背压等待多久（毫秒）。0 = 不额外等待。
    // ❗ 等待超时**不是**放行的信号：之后转入持续排队（AcquireUntilFit），直到腾出额度
    // 或整批被取消 —— max_parallel / max_exiting_tasks 是硬约束，不是建议值。
    // 取消是这条队列唯一的出口：那一刻停止派发，未处理的文件留在队列里（末尾落成
    // Cancelled），既不静默漏文件，也不悄悄超额。
    int backpressure_wait_ms = 10000;

    // 输出路径由调用方算（例如 <out>/<相对路径>.json），runner 不碰磁盘
    std::function<std::string(const std::string& source_path)> output_path_factory;
};

struct BatchQcCallbacks {
    // 有一个文件出结果就回调一次（在 worker 线程，UI 侧要自己投递回主线程）
    std::function<void(const BatchQcItemResult& item)> item_finished;
    std::function<void(int finished, int total)> progress;
    // 额外的取消源（UI 的"停止"按钮）
    std::function<bool()> is_cancelled;
};

class BatchQcRunner {
public:
    BatchQcRunner() = default;
    ~BatchQcRunner() = default;

    BatchQcRunner(const BatchQcRunner&) = delete;
    BatchQcRunner& operator=(const BatchQcRunner&) = delete;

    // 目录发现。返回的列表已排序（Windows 上 filesystem 的遍历顺序不稳定，排一下便于复现）。
    static std::vector<BatchQcItem> Discover(const std::string& root, bool recursive = true,
                                             const std::vector<std::string>& extensions = {});

    // 扩展名过滤规则：空列表放行；命中列表时按无点小写比较（".MP4" 与 "mp4" 等价）
    static bool ExtensionMatches(const std::string& path, const std::vector<std::string>& extensions);

    // 执行批量分析。返回时所有**调度 worker** 已经 join，本层不留后台线程。
    //
    // 注意区分两个时点（评审 P2-5 要求）：
    //   * "批量调度结束" = 本函数返回，run.items 里每一项都已是终态；
    //   * "单文件线程完全回收" = 每个 analyze() 内部的执行器把自己那条分析线程 join 回来。
    // 超预算 / IO 无响应时 analyze() 会放弃它自己的分析线程（detach + 请求取消），
    // 于是本函数会先于"那条被放弃的线程真正退出"返回 —— 对应条目记为 TimedOut，
    // 汇总里的 timed_out 就是这批"已停止派发、后台仍在收尾"的条目数。
    //
    // 并发额度（评审 P2-3）：派发每一项之前先向 AnalysisTaskRegistry 领一个额度，
    // 额度在**分析线程体真正退出**时才归还 —— 于是 max_parallel 约束的是"实际存活的
    // 分析任务"，而不是"调度 worker 数"。正在退出的任务单独计数并设上限，超了就背压。
    BatchQcRun Run(const std::vector<BatchQcItem>& items, const BatchQcOptions& options, const QcAnalyzeFn& analyze,
                   const BatchQcCallbacks& callbacks = BatchQcCallbacks{});

    void Cancel() {
        cancel_.store(true, std::memory_order_release);
    }
    void Reset() {
        cancel_.store(false, std::memory_order_release);
    }
    bool IsCancelling() const {
        return cancel_.load(std::memory_order_acquire);
    }
    bool IsRunning() const {
        return running_.load(std::memory_order_acquire);
    }
    int ActiveWorkerCount() const {
        return active_workers_.load(std::memory_order_acquire);
    }

private:
    bool IsCancelled(const BatchQcCallbacks& callbacks) const {
        if (cancel_.load(std::memory_order_acquire))
            return true;
        return callbacks.is_cancelled ? callbacks.is_cancelled() : false;
    }

    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::atomic<int> active_workers_{0};
};

} // namespace qc
} // namespace videoeye
