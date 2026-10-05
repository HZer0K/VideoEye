#include "core/qc/QcRunner.h"

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
    videoeye::QcRuleEngine engine(BuildRulesForProfile(profile));

    // 登记到统一协议: 这条分析从此有一个 TaskId、一份取消令牌, 终态也写回同一条 slot。
    // 调用方自带线程(这里是自建 std::thread)时走 BeginHandle 而不是 Run —— 执行方式照旧,
    // 但"身份 / 取消 / 终态"不再各写一套。
    const task::TaskHandle handle = tasks_.BeginHandle(kSlot, 0, task::TaskKind::BlockingIo);
    const bool admitted = handle.valid();
    // 引擎含 atomic 成员、不可移动, 只能按 shared_ptr 交给 worker —— 这正好是
    // "任务只携带自持有的 shared state" 的形态。
    auto engine_ptr = std::make_shared<videoeye::AnalysisEngine>();

    // worker 与等待线程之间的交接区。std::promise 也能做, 但需要额外处理
    // "进度回调要持续转发"这件事, 条件变量版本更直观。
    //
    // 整个交接区 + 引擎 + 路径 + 选项都装进这一个盒子, 由 worker **按值捕获 shared_ptr**
    // 持有。以前 worker 直接捕获 analysis/path/options/engine_callbacks 的**栈引用** ——
    // 那是"这条线程不能 detach"的真正原因: 一旦放弃, 这些栈帧就没了, 下次读就是悬空访问。
    // 现在它脱离宿主生命周期也自洽, 超预算时能交给受控回收而不是卡死等待方。
    struct Box {
        bool settled = false;
        bool timed_out = false;
        bool failed = false;
        bool canceled = false;
        std::string error;
        model::AnalysisResult result;
        std::mutex mutex;
        std::condition_variable cv;
        videoeye::AnalysisEngine* engine = nullptr;
        std::string path;
        videoeye::AnalysisOptions options;
        videoeye::AnalysisCallbacks callbacks;
    };
    auto box = std::make_shared<Box>();
    box->engine = engine_ptr.get();
    box->path = path;
    box->options = std::move(options);

    // 以前这里要通过 QtAnalysisController 绕一圈 Qt 信号（还要挂 QObject 上下文），
    // 现在直接用 AnalysisEngine 的回调 —— QC 批处理不需要任何 Qt 事件循环。
    //
    // 回调捕获的是 box(自己持有的 shared state), 不是上面的栈变量 —— 这是 worker 能被
    // 放弃的前提; 反过来写就是一份迟早崩的悬空引用。
    box->callbacks.on_finished = [box](bool, const model::AnalysisResult& result) {
        {
            std::lock_guard<std::mutex> lock(box->mutex);
            box->result = result;
            box->settled = true;
        }
        box->cv.notify_all();
    };
    box->callbacks.on_failed = [box](const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(box->mutex);
            box->failed = true;
            box->error = message;
            box->settled = true;
        }
        box->cv.notify_all();
    };
    // 进度按值搬到盒子里, worker 就完全不碰调用方栈上的 callbacks 了。
    if (callbacks.progress) box->callbacks.on_progress = callbacks.progress;

    std::thread worker([box]() { box->engine->Run(box->path, box->options, box->callbacks); });

    // 双取消源统一成一条: TaskManager 的 slot 令牌(QcRunner::Cancel())与调用方自带的
    // should_cancel(批处理任务的取消请求)。
    const auto merged_should_cancel = [handle, &callbacks]() {
        if (handle.cancel.IsCanceled()) return true;
        return callbacks.should_cancel ? callbacks.should_cancel() : false;
    };

    // 等结果: 带总预算。到点还没结算 -> 说明引擎既没回调、也没响应取消,
    // 按失败收尾并把线程转受控回收(由 TaskManager 在关闭预算内处置), 不再无限等。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(join_budget_ms);
    {
        std::unique_lock<std::mutex> lock(box->mutex);
        while (!box->settled) {
            if (box->cv.wait_for(lock, kCancelPollInterval) == std::cv_status::timeout) {
                if (merged_should_cancel()) box->engine->Cancel();
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                box->timed_out = true;
                break;
            }
        }
    }

    if (!box->timed_out) {
        // 已经结算, join 只是收句柄(瞬间返回)。
        worker.join();
    } else if (worker.joinable()) {
        // 超时: 这条线程只持有 box(由 shared_ptr 自己保活), 于是可以脱离 ——
        // 由 TaskManager 在关闭预算内统一回收, 不会再有谁 join 它。
        // 以前这里只能无限等: worker 持有 analysis/path/options 的**栈引用**,
        // 一旦 detach 就是悬空访问, 所以"不能 detach"和"不能挂死"当时二选一。
        worker.detach();
    }

    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    output.elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

    if (!admitted) {
        output.ok = false;
        output.error = "后台任务并发已满";
        return output;
    }
    if (box->timed_out) {
        if (worker.joinable())
            worker.detach();   // 只持有 box, 交给 TaskManager 在关闭预算内受控回收
        tasks_.EndHandle(handle, task::TaskState::Failed);
        output.ok = false;
        output.error = "分析在等待预算内没有结束";
        output.analysis.scan_status = model::AnalysisStatus::Failed;
        output.analysis.error_message = output.error;
        return output;
    }
    tasks_.EndHandle(handle, box->failed ? task::TaskState::Failed : task::TaskState::Succeeded);

    if (box->failed) {
        output.ok = false;
        output.error = box->error;
        // 把扫描失败的状态也带出来，便于上层区分
        output.analysis.scan_status = model::AnalysisStatus::Failed;
        if (!box->error.empty()) output.analysis.error_message = box->error;
        return output;
    }

    output.analysis = std::move(box->result);
    output.report = engine.Evaluate(output.analysis);

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
