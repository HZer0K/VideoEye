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
                                  analyzer::AnalysisOptions options,
                                  const QcRunCallbacks& callbacks) {
    QcRunResult output;
    output.profile_id = profile.id;

    std::string reason;
    if (!IsAnalyzableFile(path, reason)) {
        output.ok = false;
        output.error = reason;
        return output;
    }

    const auto started_at = std::chrono::steady_clock::now();
    analyzer::QcRuleEngine engine(BuildRulesForProfile(profile));
    analyzer::AnalysisEngine analysis;

    // worker 线程与等待线程之间的交接区。std::promise 也能做，但需要额外处理
    // "进度回调要持续转发"这件事，条件变量版本更直观。
    struct Outcome {
        bool settled = false;
        bool failed = false;
        std::string error;
        analyzer::AnalysisResult result;
    };
    Outcome outcome;
    std::mutex mutex;
    std::condition_variable cv;

    auto settle = [&outcome, &mutex, &cv](Outcome next) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            outcome = std::move(next);
            outcome.settled = true;
        }
        cv.notify_all();
    };

    // 以前这里要通过 AnalysisCoordinator 绕一圈 Qt 信号（还要挂 QObject 上下文），
    // 现在直接用 AnalysisEngine 的回调 —— QC 批处理不需要任何 Qt 事件循环。
    analyzer::AnalysisCallbacks engine_callbacks;
    engine_callbacks.on_finished = [&settle](bool, const analyzer::AnalysisResult& result) {
        Outcome next;
        next.result = result;
        settle(std::move(next));
    };
    engine_callbacks.on_failed = [&settle](const std::string& message) {
        Outcome next;
        next.failed = true;
        next.error = message;
        settle(std::move(next));
    };
    if (callbacks.progress) {
        engine_callbacks.on_progress = [&callbacks](double percent, const std::string& stage) {
            callbacks.progress(percent, stage);
        };
    }

    auto merged_should_cancel = [this, &callbacks]() {
        if (cancel_.load(std::memory_order_acquire)) return true;
        return callbacks.should_cancel ? callbacks.should_cancel() : false;
    };

    std::thread worker([&analysis, &path, &options, &engine_callbacks]() {
        analysis.Run(path, options, engine_callbacks);
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!outcome.settled) {
            if (cv.wait_for(lock, kCancelPollInterval) == std::cv_status::timeout) {
                if (merged_should_cancel()) analysis.Cancel();
            }
        }
    }
    worker.join();

    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    output.elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

    if (outcome.failed) {
        output.ok = false;
        output.error = outcome.error;
        // 把扫描失败的状态也带出来，便于上层区分
        output.analysis.scan_status = analyzer::AnalysisStatus::Failed;
        if (!outcome.error.empty()) output.analysis.error_message = outcome.error;
        return output;
    }

    output.analysis = std::move(outcome.result);
    output.report = engine.Evaluate(output.analysis);

    // 区分终态：取消 / 失败都算没跑完；抽样(命中包数上限)是部分结果但可算成功
    switch (output.analysis.scan_status) {
        case analyzer::AnalysisStatus::Cancelled:
            output.ok = false;
            output.error = "分析被取消";
            return output;
        case analyzer::AnalysisStatus::Failed:
            output.ok = false;
            output.error = output.analysis.error_message.empty()
                              ? "分析失败" : output.analysis.error_message;
            return output;
        case analyzer::AnalysisStatus::Complete:
        case analyzer::AnalysisStatus::Sampled:
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
                                          analyzer::AnalysisOptions options) {
    // profile / options 在闭包之间共享（只读），cancel 状态每次调用由 request 带入。
    auto shared = std::make_shared<std::pair<QcProfile, analyzer::AnalysisOptions>>(
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
