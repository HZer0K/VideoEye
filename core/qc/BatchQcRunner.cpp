#include "core/qc/BatchQcRunner.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace qc {
namespace {

constexpr int kMaxParallelLimit = 16;

std::string LowerWithoutDot(const std::string& extension) {
    std::string result;
    result.reserve(extension.size());
    for (char c : extension) {
        if (c == '.')
            continue;
        if (c >= 'A' && c <= 'Z') {
            result += static_cast<char>(c - 'A' + 'a');
        } else {
            result += c;
        }
    }
    return result;
}

// 派发前的并发额度（评审 P2-3）。
//
// 关键：额度统计的是**分析任务**而不是调度 worker。一个条目超预算返回后，它那条分析
// 线程还要跑一会儿才退出 —— 那段时间它仍占着 FFmpeg / 内存 / 磁盘，也必须占着额度，
// 否则 max_parallel=4 时真正跑着的分析可能是 4 + N 条。
//
// ❗❗ 拿不到额度时**不再**退化成"不限量派发" —— 那样 max_parallel 与 max_exiting_tasks
// 就只是"建议值"：旧分析线程迟迟不退出时后续任务会持续叠加实际资源占用，登记处只
// 提高了可观测性，却没有约束力。现在背压超时之后**继续排队**（AcquireUntilFit），
// 直到腾出额度或整批被取消 —— 上限因此是硬的，代价只是这一项多等一会儿。
//
// 唯一的出口是取消：返回 nullptr 让派发侧立刻停止，还没取走的文件留在队列里
// （末尾统一落成 Cancelled），既不静默漏文件，也不悄悄超额。
std::shared_ptr<AnalysisTaskSlot> AcquireAnalysisSlot(const BatchQcOptions& options,
                                                      const std::function<bool()>& cancelled) {
    const int limit = std::clamp(options.max_parallel, 1, kMaxParallelLimit);
    const int max_exiting = options.max_exiting_tasks > 0 ? options.max_exiting_tasks : limit;
    AnalysisTaskRegistry& registry = AnalysisTaskRegistry::Instance();
    auto slot = registry.Acquire(limit, max_exiting, options.backpressure_wait_ms, cancelled);
    if (slot)
        return slot;
    if (cancelled && cancelled())
        return nullptr; // 取消：不再派发（不占额度、不留后台任务）
    LOG_WARN("批量分析并发额度不足（存活 " + std::to_string(registry.TotalCount()) + " 条，其中退出中 " +
             std::to_string(registry.ExitingCount()) + " 条），等待 " + std::to_string(options.backpressure_wait_ms) +
             "ms 后继续排队 —— 不突破 max_parallel");
    return registry.AcquireUntilFit(limit, max_exiting, cancelled);
}

std::string ExtensionOf(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return std::string();
    return LowerWithoutDot(path.substr(dot + 1));
}

} // namespace

bool BatchQcRunner::ExtensionMatches(const std::string& path, const std::vector<std::string>& extensions) {
    if (extensions.empty())
        return true;
    const std::string ext = ExtensionOf(path);
    if (ext.empty())
        return false;
    for (const auto& candidate : extensions) {
        if (LowerWithoutDot(candidate) == ext)
            return true;
    }
    return false;
}

std::vector<BatchQcItem> BatchQcRunner::Discover(const std::string& root, bool recursive,
                                                 const std::vector<std::string>& extensions) {
    std::vector<BatchQcItem> items;
    std::error_code ec;

    auto visit = [&items, &extensions](const std::filesystem::directory_entry& entry, std::error_code& error) {
        if (!entry.is_regular_file(error))
            return;
        const std::string path = entry.path().string();
        if (!ExtensionMatches(path, extensions))
            return;
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
            if (ec)
                break;
            visit(*it, ec);
        }
    } else {
        for (std::filesystem::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec)
                break;
            visit(*it, ec);
        }
    }

    std::sort(items.begin(), items.end(), [](const BatchQcItem& a, const BatchQcItem& b) { return a.path < b.path; });
    return items;
}

BatchQcRun BatchQcRunner::Run(const std::vector<BatchQcItem>& items, const BatchQcOptions& options,
                              const QcAnalyzeFn& analyze, const BatchQcCallbacks& callbacks) {
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

    const int worker_count = std::clamp(options.max_parallel, 1, std::min(kMaxParallelLimit, total));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (int worker_id = 0; worker_id < worker_count; ++worker_id) {
        workers.emplace_back([this, &run, &items, &options, &analyze, &callbacks, &callback_mutex, &finished,
                              &next_index]() {
            active_workers_.fetch_add(1, std::memory_order_acq_rel);
            while (true) {
                if (IsCancelled(callbacks)) {
                    // 取消必须在这里**立刻立起来**, 不能只是自己退出: 每支 worker 手上正在
                    // 跑的那个 analyze 只认 request.cancel(它指向_cancel_), 而它下次查看
                    // 自己的结束条件要等到当前这一项整个跑完。统一置位之后, 其它 worker
                    // 才能在各自分析**过程中**感知到 —— 这既是"点停止后多久能真正停"的关键,
                    // 也是下面那段 join 唯一的耗时上界来源: 跑得最慢的那一项能被中断, join
                    // 就不可能拖到它自然跑完。
                    cancel_.store(true, std::memory_order_release);
                    break;
                }
                const std::size_t index = next_index.fetch_add(1, std::memory_order_acq_rel);
                if (index >= items.size())
                    break;

                BatchQcItemResult item;
                item.path = items[index].path;

                if (options.max_file_size_bytes > 0 && items[index].file_size_bytes > options.max_file_size_bytes) {
                    item.status = BatchItemStatus::Skipped;
                    item.error = "文件超过大小上限，已跳过";
                } else if (IsCancelled(callbacks)) {
                    item.status = BatchItemStatus::Cancelled;
                } else {
                    const auto item_started_at = std::chrono::steady_clock::now();
                    QcAnalyzeRequest request;
                    request.path = item.path;
                    // 三项等待策略随请求下发给分析函数（QcRunner 直接消费）；
                    // 超预算 / IO 无响应的条目会带着 timed_out 回来。
                    request.budgets =
                        QcRunBudgets{options.per_item_timeout_ms, options.io_idle_timeout_ms, options.cancel_wait_ms};
                    // 把外部取消源（UI 停止按钮）最新的状态同步进内部标记，
                    // 这样正在跑的单文件分析也能通过 request.cancel 即时感知到取消。
                    cancel_.store(IsCancelled(callbacks), std::memory_order_release);
                    request.cancel = &cancel_;
                    // 派发前先领一个分析额度（评审 P2-3）：额度不足时先背压等待、
                    // 之后继续排队，于是"实际存活的分析任务数"不会超过 max_parallel
                    // —— 包括那些已经返回但线程体还在退出的条目。
                    // 取消源要连本 runner 的 cancel_ 与调用方的 is_cancelled：只接后者时
                    // "点了停止"之后排队中的这一项要等到背压超时才肯退出。
                    const std::function<bool()> cancelled = [this, &callbacks]() {
                        return cancel_.load(std::memory_order_acquire) ||
                               (callbacks.is_cancelled && callbacks.is_cancelled());
                    };
                    request.slot = AcquireAnalysisSlot(options, cancelled);
                    if (!request.slot)
                        break; // 取消：停止派发，本项与队列里剩下的留给末尾统一落成 Cancelled

                    try {
                        const QcRunResult result = analyze(request);
                        // 分析函数没认领额度（假实现 / 换执行器）时兜底归还；
                        // 认领了的由线程体在自己退出时归还，这里**不能**提前还 ——
                        // 那会让"线程还在退"这段时间从统计里消失。
                        if (request.slot && !request.slot->IsAdopted())
                            request.slot->Release();
                        // 显示路径以实际落盘路径为准（与导出共用同一组路径）；
                        // 未导出时退回工厂给出的路径，避免表格路径与实际文件不一致。
                        if (!result.output_path.empty()) {
                            item.output_path = result.output_path;
                        } else if (options.output_path_factory) {
                            item.output_path = options.output_path_factory(item.path);
                        }
                        item.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                                    item_started_at)
                                              .count();
                        item.report = result.report;
                        item.critical_count = result.report.CountBySeverity(model::IssueSeverity::Critical);
                        item.error_count = result.report.CountBySeverity(model::IssueSeverity::Error);
                        item.warning_count = result.report.CountBySeverity(model::IssueSeverity::Warning);
                        item.info_count = result.report.CountBySeverity(model::IssueSeverity::Info);
                        item.score = result.report.score;
                        item.verdict = result.report.verdict;
                        if (!options.keep_reports) {
                            item.report = model::QcReport{}; // 已经导出过了，不必驻留内存
                            item.report.score = item.score;
                            item.report.verdict = item.verdict;
                        }

                        if (result.ok) {
                            item.status =
                                result.export_failed ? BatchItemStatus::ExportFailed : BatchItemStatus::Succeeded;
                        } else if (result.timed_out) {
                            // 超预算：本函数照常返回，但被放弃的分析线程仍在后台收尾。
                            // 单独记 TimedOut，别与"引擎自己报失败"混成一个 Failed —— 两者的
                            // 处置方式不同（超时值得去看是不是网络源/大文件）。
                            item.status = BatchItemStatus::TimedOut;
                            item.error = result.error.empty() ? "分析超时" : result.error;
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
                if (callbacks.item_finished)
                    callbacks.item_finished(item);
                if (callbacks.progress)
                    callbacks.progress(done, static_cast<int>(items.size()));
            }
            active_workers_.fetch_sub(1, std::memory_order_acq_rel);
        });
    }

    // 这里**故意**是无上界的 join, 与 QcRunner / QtAnalysisController / ReportingPanel
    // 那三处"预算内等待"不同 —— 区别不在偷懒, 而在契约:
    //   * Run() 是**同步 API**: 调用方拿到返回值时, run.items 里每一项都必须是最终状态。
    //     换成 detach 或超时放行, 就会出现"函数已返回、结果还在被别的线程写"的竞态。
    //   * worker 体按引用捕获了 this 与 run / items / callbacks 等**栈上对象**, 一旦 detach
    //     之后它们会随 Run() 返回而消失 —— 放弃线程等于批准悬空访问, 比多等危险得多。
    //
    // 所以它的上界来自**取消**而不是超时: 上面那个"取消一进来就置 cancel_"的传播让最慢的
    // 一项也能被打断, 每支 worker 都必然在下一个 item 边界退出。真正卡住的场景是某一项
    // 的 analyze() 完全不看取消 —— 那时多等 = 让 UI 察觉"它卡住了", 而 detach = 静默地
    // 把成果丢掉, 这里选前者。
    for (auto& worker : workers) {
        if (worker.joinable())
            worker.join();
    }
    running_.store(false, std::memory_order_release);

    // 取消/异常导致没跑到的一律落成 Cancelled，避免 UI 里出现永远停留在"等待中"的行。
    for (auto& item : run.items) {
        if (item.status == BatchItemStatus::Pending)
            item.status = BatchItemStatus::Cancelled;
    }

    BatchQcSummary& summary = run.summary;
    for (const auto& item : run.items) {
        switch (item.status) {
        case BatchItemStatus::Succeeded:
            ++summary.succeeded;
            break;
        case BatchItemStatus::Failed:
            ++summary.failed;
            break;
        case BatchItemStatus::ExportFailed:
            ++summary.failed;
            break; // 当作需要处理的问题
        case BatchItemStatus::TimedOut:
            ++summary.timed_out;
            break; // 单列，不计入 failed
        case BatchItemStatus::Cancelled:
            ++summary.cancelled;
            break;
        case BatchItemStatus::Skipped:
            ++summary.skipped;
            break;
        case BatchItemStatus::Pending:
        case BatchItemStatus::Running:
            break;
        }
        summary.critical_count += item.critical_count;
        summary.error_count += item.error_count;
        summary.warning_count += item.warning_count;
        summary.info_count += item.info_count;
    }
    summary.elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at).count();
    summary.completed = (summary.cancelled == 0);

    return run;
}

} // namespace qc
} // namespace videoeye
