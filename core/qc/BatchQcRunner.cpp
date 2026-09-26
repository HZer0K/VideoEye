#include "core/qc/BatchQcRunner.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include "utils/Logger.h"

namespace videoeye {
namespace qc {
namespace {

constexpr int kMaxParallelLimit = 16;

std::string LowerWithoutDot(const std::string& extension) {
    std::string result;
    result.reserve(extension.size());
    for (char c : extension) {
        if (c == '.') continue;
        if (c >= 'A' && c <= 'Z') {
            result += static_cast<char>(c - 'A' + 'a');
        } else {
            result += c;
        }
    }
    return result;
}

std::string ExtensionOf(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    return LowerWithoutDot(path.substr(dot + 1));
}

}  // namespace

const char* ToString(BatchItemStatus status) {
    switch (status) {
        case BatchItemStatus::Pending:   return "等待中";
        case BatchItemStatus::Running:   return "分析中";
        case BatchItemStatus::Succeeded: return "完成";
        case BatchItemStatus::Failed:    return "失败";
        case BatchItemStatus::Cancelled: return "已取消";
        case BatchItemStatus::Skipped:   return "已跳过";
    }
    return "未知";
}

bool BatchQcRunner::ExtensionMatches(const std::string& path,
                                     const std::vector<std::string>& extensions) {
    if (extensions.empty()) return true;
    const std::string ext = ExtensionOf(path);
    if (ext.empty()) return false;
    for (const auto& candidate : extensions) {
        if (LowerWithoutDot(candidate) == ext) return true;
    }
    return false;
}

std::vector<BatchQcItem> BatchQcRunner::Discover(const std::string& root,
                                                 bool recursive,
                                                 const std::vector<std::string>& extensions) {
    std::vector<BatchQcItem> items;
    std::error_code ec;

    auto visit = [&items, &extensions](const std::filesystem::directory_entry& entry,
                                       std::error_code& error) {
        if (!entry.is_regular_file(error)) return;
        const std::string path = entry.path().string();
        if (!ExtensionMatches(path, extensions)) return;
        BatchQcItem item;
        item.path = path;
        const auto size = entry.file_size(error);
        item.file_size_bytes = error ? 0 : static_cast<int64_t>(size);
        items.push_back(std::move(item));
    };

    if (std::filesystem::is_regular_file(root, ec) && !ec) {
        visit(std::filesystem::directory_entry(root, ec), ec);
    } else if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            visit(*it, ec);
        }
    } else {
        for (std::filesystem::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            visit(*it, ec);
        }
    }

    std::sort(items.begin(), items.end(),
              [](const BatchQcItem& a, const BatchQcItem& b) { return a.path < b.path; });
    return items;
}

BatchQcRun BatchQcRunner::Run(const std::vector<BatchQcItem>& items,
                              const BatchQcOptions& options,
                              const QcAnalyzeFn& analyze,
                              const BatchQcCallbacks& callbacks) {
    BatchQcRun run;
    const int total = static_cast<int>(items.size());
    run.summary.total = total;
    run.items.resize(items.size());

    for (std::size_t i = 0; i < items.size(); ++i) {
        run.items[i].path = items[i].path;
        run.items[i].status = BatchItemStatus::Pending;
    }

    if (total == 0 || analyze == nullptr) {
        run.summary.completed = true;
        return run;
    }

    const auto started_at = std::chrono::steady_clock::now();

    running_.store(true, std::memory_order_release);
    cancel_.store(IsCancelled(callbacks), std::memory_order_release);

    // 结果槽位本身不需要锁（下标互不重叠），但 finished 计数与回调调用要串行化，
    // 否则 UI 侧会看到错乱的进度。
    std::mutex callback_mutex;
    std::atomic<int> finished{0};
    std::atomic<std::size_t> next_index{0};

    const int worker_count = std::clamp(options.max_parallel, 1,
                                        std::min(kMaxParallelLimit, total));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
        workers.emplace_back([this, &run, &items, &options, &analyze, &callbacks,
                              &callback_mutex, &finished, &next_index]() {
            active_workers_.fetch_add(1, std::memory_order_acq_rel);
            while (true) {
                if (IsCancelled(callbacks)) break;
                const std::size_t index = next_index.fetch_add(1, std::memory_order_acq_rel);
                if (index >= items.size()) break;

                BatchQcItemResult item;
                item.path = items[index].path;

                if (options.max_file_size_bytes > 0 &&
                    items[index].file_size_bytes > options.max_file_size_bytes) {
                    item.status = BatchItemStatus::Skipped;
                    item.error = "文件超过大小上限，已跳过";
                } else if (IsCancelled(callbacks)) {
                    item.status = BatchItemStatus::Cancelled;
                } else {
                    const auto item_started_at = std::chrono::steady_clock::now();
                    QcAnalyzeRequest request;
                    request.path = item.path;
                    // 把外部取消源（UI 停止按钮）最新的状态同步进内部标记，
                    // 这样正在跑的单文件分析也能通过 request.cancel 即时感知到取消。
                    cancel_.store(IsCancelled(callbacks), std::memory_order_release);
                    request.cancel = &cancel_;

                    try {
                        const QcRunResult result = analyze(request);
                        if (options.output_path_factory) {
                            item.output_path = options.output_path_factory(item.path);
                        }
                        item.elapsed_ms =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - item_started_at).count();
                        item.report = result.report;
                        item.critical_count = result.report.CountBySeverity(model::IssueSeverity::Critical);
                        item.error_count = result.report.CountBySeverity(model::IssueSeverity::Error);
                        item.warning_count = result.report.CountBySeverity(model::IssueSeverity::Warning);
                        item.info_count = result.report.CountBySeverity(model::IssueSeverity::Info);
                        item.score = result.report.score;
                        item.verdict = result.report.verdict;
                        if (!options.keep_reports) {
                            item.report = model::QcReport{};  // 已经导出过了，不必驻留内存
                            item.report.score = item.score;
                            item.report.verdict = item.verdict;
                        }

                        if (result.ok) {
                            item.status = BatchItemStatus::Succeeded;
                        } else if (result.error == "分析被取消") {
                            item.status = BatchItemStatus::Cancelled;
                            item.error = result.error;
                        } else {
                            item.status = BatchItemStatus::Failed;
                            item.error = result.error.empty() ? "分析失败" : result.error;
                        }
                    } catch (const std::exception& e) {
                        item.status = BatchItemStatus::Failed;
                        item.error = std::string("分析过程抛出异常: ") + e.what();
                        LOG_ERROR("批量分析异常: " + item.error);
                    }
                }

                run.items[index] = item;
                const int done = finished.fetch_add(1, std::memory_order_acq_rel) + 1;

                std::lock_guard<std::mutex> lock(callback_mutex);
                if (callbacks.item_finished) callbacks.item_finished(item);
                if (callbacks.progress) callbacks.progress(done, static_cast<int>(items.size()));
            }
            active_workers_.fetch_sub(1, std::memory_order_acq_rel);
        });
    }

    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    running_.store(false, std::memory_order_release);

    // 取消/异常导致没跑到的一律落成 Cancelled，避免 UI 里出现永远停留在"等待中"的行。
    for (auto& item : run.items) {
        if (item.status == BatchItemStatus::Pending) item.status = BatchItemStatus::Cancelled;
    }

    BatchQcSummary& summary = run.summary;
    for (const auto& item : run.items) {
        switch (item.status) {
            case BatchItemStatus::Succeeded: ++summary.succeeded; break;
            case BatchItemStatus::Failed:    ++summary.failed;    break;
            case BatchItemStatus::Cancelled: ++summary.cancelled; break;
            case BatchItemStatus::Skipped:   ++summary.skipped;   break;
            case BatchItemStatus::Pending:
            case BatchItemStatus::Running:   break;
        }
        summary.critical_count += item.critical_count;
        summary.error_count += item.error_count;
        summary.warning_count += item.warning_count;
        summary.info_count += item.info_count;
    }
    summary.elapsed_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started_at).count();
    summary.completed = (summary.cancelled == 0);

    return run;
}

}  // namespace qc
}  // namespace videoeye
