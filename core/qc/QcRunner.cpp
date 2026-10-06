#include "core/qc/QcRunner.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "core/analysis/orchestration/AnalysisEngine.h"

namespace videoeye {
namespace qc {
namespace {

constexpr std::chrono::milliseconds kCancelPollInterval{100};

}  // namespace

bool IsAnalyzableFile(const std::string& path, std::string& reason) {
    std::error_code ec;
    if (path.empty()) {
        reason = "路径为空";
        return false;
    }
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
        reason = "文件不存在: " + path;
        return false;
    }
    if (std::filesystem::is_directory(status)) {
        reason = "路径是目录，单文件分析请给出具体文件: " + path;
        return false;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size == 0) {
        reason = "文件为空: " + path;
        return false;
    }
    return true;
}

QcRunResult QcRunner::AnalyzeFile(const std::string& path,
                                  const QcProfile& profile,
                                  videoeye::AnalysisOptions options,
                                  const QcRunCallbacks& callbacks,
                                  int join_budget_ms) {
    QcRunResult output;
    output.profile_id = profile.id;

    std::string reason;
    if (!IsAnalyzableFile(path, reason)) {
        output.ok = false;
        output.error = reason;
        return output;
    }

    const auto started_at = std::chrono::steady_clock::now();
    videoeye::QcRuleEngine rules_engine(BuildRulesForProfile(profile));

    // worker 与等待线程之间的交接区。std::promise 也能做, 但需要额外处理
    // "进度回调要持续转发"这件事, 条件变量版本更直观。
    //
    // 盒子必须**自己持有**引擎(shared_ptr, 不再是裸指针指向 AnalyzeFile 的局部变量):
    // 超预算 detach 之后 AnalyzeFile 的栈已经走了, 若 Box 里还是裸指针, 后台线程读到的
    // 就是已经析构的引擎 —— 这正是评审点名的悬空访问。现在"谁保活"只有一个答案: 盒子。
    // 路径 / 选项 / 回调也全在盒子里, worker 只按值捕获盒子的 shared_ptr, 不碰任何栈帧。
    struct Box {
        bool settled = false;
        bool timed_out = false;
        bool failed = false;
        std::string error;
        model::AnalysisResult result;
        std::mutex mutex;
        std::condition_variable cv;
        std::shared_ptr<videoeye::AnalysisEngine> engine;  // 自持有: detach 后线程仍安全
        std::shared_ptr<std::atomic<bool>> cancel_flag;    // 本次分析的取消源
        std::string path;
        videoeye::AnalysisOptions options;
        std::function<void(double, const std::string&)> progress;  // 调用方的进度回调
    };
    auto box = std::make_shared<Box>();
    // 引擎含 atomic 成员、不可移动, 只能按 shared_ptr 交给 worker —— 盒子里放 shared_ptr
    // 而不是 .get() 出来的裸指针, 就是"任务只携带自持有的 shared state"的形态。
    box->engine = std::make_shared<videoeye::AnalysisEngine>();
    box->cancel_flag = std::make_shared<std::atomic<bool>>(false);
    box->path = path;
    box->options = std::move(options);

    // 以前这里要通过 QtAnalysisController 绕一圈 Qt 信号（还要挂 QObject 上下文），
    // 现在直接用 AnalysisEngine 的回调 —— QC 批处理不需要任何 Qt 事件循环。
    //
    // 回调**在 worker 栈上构造**、按值捕获盒子: 盒子自己绝不持有这个回调。
    // 若把回调存进盒子(回调又捕获盒子), 就是一个 shared_ptr 环 —— 每次分析都留下
    // 一个永不释放的盒子(结果数据还在里面), ASan/LSan 必报泄漏。回调只可能在
    // Run() 期间触发, 那时盒子必然活着(worker 自己就持有一份强引用)。
    if (callbacks.progress) box->progress = callbacks.progress;

    // 取消源用盒子自持的那颗原子标志: 调用方的 should_cancel 在等待循环里被转成它的置位,
    // 引擎的所有取消轮询点（含 FFmpeg 中断回调）都看它 —— 取消只有这一颗, 不再有两套。
    std::thread worker([box]() {
        videoeye::AnalysisCallbacks effective;
        effective.on_finished = [box](bool, const model::AnalysisResult& result) {
            {
                std::lock_guard<std::mutex> lock(box->mutex);
                box->result = result;
                box->settled = true;
            }
            box->cv.notify_all();
        };
        effective.on_failed = [box](const std::string& message) {
            {
                std::lock_guard<std::mutex> lock(box->mutex);
                box->failed = true;
                box->error = message;
                box->settled = true;
            }
            box->cv.notify_all();
        };
        if (box->progress) effective.on_progress = box->progress;
        box->engine->Run(box->path, box->options, effective, nullptr, box->cancel_flag);
    });

    // 等结果: 带总预算。到点还没结算 -> 说明引擎既没回调、也没响应取消,
    // 按失败收尾并把线程转受控回收 —— worker 只持盒子(含引擎本体), detach 后自洽。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(join_budget_ms);
    {
        std::unique_lock<std::mutex> lock(box->mutex);
        while (!box->settled) {
            // 预算检查放在等待**之前**: join_budget_ms=0 的语义是"一秒都不等"。
            // 先 wait_for 再判预算的话, 0 预算在 worker 恰好快速结算时会滑进正常路径,
            // "超时 detach"这条回归路径就失去确定性(评审要求的 join_budget_ms=0 用例
            // 正是冲它来的)。现有调用方最小预算是 500ms, 提前判预算不会改变它们的语义。
            if (std::chrono::steady_clock::now() >= deadline) {
                box->timed_out = true;
                break;
            }
            if (box->cv.wait_for(lock, kCancelPollInterval) == std::cv_status::timeout) {
                // 调用方的取消请求(批处理任务的取消按钮/面板回收)在这一刻转成引擎可见的
                // 取消源置位 —— 引擎卡在 FFmpeg 网络 IO 上也能被它打断。
                if (callbacks.should_cancel && callbacks.should_cancel()) {
                    box->cancel_flag->store(true, std::memory_order_release);
                }
            }
        }
    }

    if (!box->timed_out) {
        // 已经结算, join 只是收句柄(瞬间返回)。
        worker.join();
    } else if (worker.joinable()) {
        // 超时: 这条线程只持有盒子(shared_ptr 自己保活, 含引擎本体), 于是可以脱离 ——
        // 不会再有谁 join 它, 也不会读到已析构的引擎。
        // 以前这里只能无限等或制造悬空访问: worker 持引擎裸指针、且依赖调用方栈上的
        // analysis/path/options 引用, "不能 detach"和"不能挂死"当时二选一。
        worker.detach();
    }

    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    output.elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

    if (box->timed_out) {
        output.ok = false;
        output.error = "分析在等待预算内没有结束";
        output.analysis.scan_status = model::AnalysisStatus::Failed;
        output.analysis.error_message = output.error;
        return output;
    }

    if (box->failed) {
        output.ok = false;
        output.error = box->error;
        // 把扫描失败的状态也带出来，便于上层区分
        output.analysis.scan_status = model::AnalysisStatus::Failed;
        if (!box->error.empty()) output.analysis.error_message = box->error;
        return output;
    }

    output.analysis = std::move(box->result);
    output.report = rules_engine.Evaluate(output.analysis);

    // 区分终态：取消 / 失败都算没跑完；抽样(命中包数上限)是部分结果但可算成功
    switch (output.analysis.scan_status) {
        case model::AnalysisStatus::Cancelled:
            output.ok = false;
            output.error = "分析被取消";
            return output;
        case model::AnalysisStatus::Failed:
            output.ok = false;
            output.error = output.analysis.error_message.empty()
                              ? "分析失败" : output.analysis.error_message;
            return output;
        case model::AnalysisStatus::Complete:
        case model::AnalysisStatus::Sampled:
            break;  // 到达终态，继续判定
    }

    if (!output.analysis.error_message.empty()) {
        output.ok = false;
        output.error = output.analysis.error_message;
        return output;
    }

    output.ok = true;
    return output;
}

QcAnalyzeFn QcRunner::MakeAnalyzeFunction(const QcProfile& profile,
                                          videoeye::AnalysisOptions options) {
    // profile / options 在闭包之间共享（只读），cancel 状态每次调用由 request 带入。
    auto shared = std::make_shared<std::pair<QcProfile, videoeye::AnalysisOptions>>(
        profile, options);

    return [shared](const QcAnalyzeRequest& request) -> QcRunResult {
        QcRunner runner;
        QcRunCallbacks callbacks;
        callbacks.should_cancel = [&request]() {
            return request.cancel != nullptr && request.cancel->load(std::memory_order_acquire);
        };
        return runner.AnalyzeFile(request.path, shared->first, shared->second, callbacks);
    };
}

}  // namespace qc
}  // namespace videoeye