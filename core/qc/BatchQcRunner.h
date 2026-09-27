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

#include "core/model/QcReport.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcReportFormat.h"

namespace videoeye {
namespace qc {

enum class BatchItemStatus {
    Pending,    // 还没轮到
    Running,    // 正在分析
    Succeeded,  // 分析完成（不代表没问题，只看 report 里的严重度计数）
    Failed,     // 打开失败 / 分析器报错
    Cancelled,  // 取消后未执行，或执行中被中断
    Skipped,    // 被选项排除（如超过大小上限）
    ExportFailed,  // 分析成功，但报告导出失败（目录不可写 / 写入异常）
};

const char* ToString(BatchItemStatus status);

struct BatchQcItem {
    std::string path;
    int64_t file_size_bytes = 0;
};

struct BatchQcItemResult {
    std::string path;
    BatchItemStatus status = BatchItemStatus::Pending;
    std::string error;
    // 报告本体。默认保留（UI 要按文件看详情）；批量导出成百上千个文件时可以把
    // options.keep_reports 关掉，只留下面几个计数，省内存。
    model::QcReport report;
    int critical_count = 0;
    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
    double score = 0.0;
    std::string verdict;
    double elapsed_ms = 0.0;
    std::string output_path;    // 主导出文件的落盘路径（由调用方通过 output_path_factory 决定）

    bool HasReport() const { return status == BatchItemStatus::Succeeded; }
    int IssueCount() const { return critical_count + error_count + warning_count + info_count; }
    int BlockingCount() const { return critical_count + error_count; }
};

struct BatchQcSummary {
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    int cancelled = 0;
    int skipped = 0;      // 超过大小上限被跳过
    int critical_count = 0;
    int error_count = 0;
    int warning_count = 0;
    int info_count = 0;
    double elapsed_ms = 0.0;
    bool completed = false;   // false = 被取消，列表里有没跑到的数据
};

struct BatchQcOptions {
    bool recursive = true;
    std::vector<std::string> extensions;   // 无点小写；空 = 不按扩展名过滤
    int max_parallel = 4;                  // 并发上限，会再夹到 [1, 16]
    int64_t max_file_size_bytes = 0;       // 0 = 不限制；超限记 Skipped
    bool keep_reports = true;              // false = 结果里只留计数，不留 QcReport 本体

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

struct BatchQcRun {
    std::vector<BatchQcItemResult> items;
    BatchQcSummary summary;
};

class BatchQcRunner {
public:
    BatchQcRunner() = default;
    ~BatchQcRunner() = default;

    BatchQcRunner(const BatchQcRunner&) = delete;
    BatchQcRunner& operator=(const BatchQcRunner&) = delete;

    // 目录发现。返回的列表已排序（Windows 上 filesystem 的遍历顺序不稳定，排一下便于复现）。
    static std::vector<BatchQcItem> Discover(const std::string& root,
                                             bool recursive = true,
                                             const std::vector<std::string>& extensions = {});

    // 扩展名过滤规则：空列表放行；命中列表时按无点小写比较（".MP4" 与 "mp4" 等价）
    static bool ExtensionMatches(const std::string& path,
                                 const std::vector<std::string>& extensions);

    // 执行批量分析。返回时所有 worker 已经 join，不留后台线程。
    BatchQcRun Run(const std::vector<BatchQcItem>& items,
                   const BatchQcOptions& options,
                   const QcAnalyzeFn& analyze,
                   const BatchQcCallbacks& callbacks = BatchQcCallbacks{});

    void Cancel() { cancel_.store(true, std::memory_order_release); }
    void Reset() { cancel_.store(false, std::memory_order_release); }
    bool IsCancelling() const { return cancel_.load(std::memory_order_acquire); }
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    int ActiveWorkerCount() const { return active_workers_.load(std::memory_order_acquire); }

private:
    bool IsCancelled(const BatchQcCallbacks& callbacks) const {
        if (cancel_.load(std::memory_order_acquire)) return true;
        return callbacks.is_cancelled ? callbacks.is_cancelled() : false;
    }

    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::atomic<int> active_workers_{0};
};

}  // namespace qc
}  // namespace videoeye
