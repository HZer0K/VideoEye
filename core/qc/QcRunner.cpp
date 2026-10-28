#include "core/qc/QcRunner.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "core/analysis/orchestration/AnalysisEngine.h"
#include "core/analysis/orchestration/AnalysisTerminalState.h"
#include "core/domain/task/ThreadBody.h"
#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace qc {

MediaInput ClassifyMediaInput(const std::string& uri) {
    MediaInput input;
    input.uri = uri;
    // 只有 "scheme://..." 才是 URI。本地路径（含 Windows 的 "C:\..."）没有 "://"，
    // 一律按本地文件处理。
    const std::size_t sep = uri.find("://");
    if (sep == std::string::npos || sep == 0) {
        input.kind = MediaInput::Kind::LocalFile;
        return input;
    }
    std::string scheme;
    scheme.reserve(sep);
    for (std::size_t i = 0; i < sep; ++i) {
        const char c = uri[i];
        scheme += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    // "file://" 指向本地文件系统，仍按本地文件处理；其余 scheme 一律视为网络源
    // （http/https/rtmp/rtmps/rtsp/rtsps/rtp/udp/tcp/srt/... 都在此列）。
    input.kind = (scheme == "file") ? MediaInput::Kind::LocalFile : MediaInput::Kind::NetworkUri;
    return input;
}

bool IsNetworkUri(const std::string& uri) {
    return ClassifyMediaInput(uri).kind == MediaInput::Kind::NetworkUri;
}

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

bool IsAnalyzableInput(const MediaInput& input, std::string& reason) {
    if (input.kind == MediaInput::Kind::NetworkUri) {
        // 网络源不做本地存在性校验：连不连得上交给 FFmpeg，由等待预算兜底。
        if (input.uri.empty()) {
            reason = "网络源地址为空";
            return false;
        }
        return true;
    }
    return IsAnalyzableFile(input.uri, reason);
}

// 放弃原因 -> 收尾文案。三条各有各的处置方式，所以文案不能并成一句"分析超时"：
//   * 调用方取消（Cancelled）必须与引擎自己报的取消同文案 —— 批量侧按它落成
//     BatchItemStatus::Cancelled，混进"超时"会让"用户点了停止"看起来像文件有问题；
//   * 超预算 / IO 无响应才是真正需要去看文件/网络的两类。
std::string AbandonErrorText(QcAbandonReason reason) {
    switch (reason) {
    case QcAbandonReason::Cancelled:
        return "分析被取消";
    case QcAbandonReason::OverBudget:
        return "分析超出允许的运行时长";
    case QcAbandonReason::IoStalled:
        return "分析输入长时间无响应";
    case QcAbandonReason::None:
        break;
    }
    return "分析在等待预算内没有结束";
}

QcRunResult QcRunner::AnalyzeFile(const MediaInput& input, const QcProfile& profile, videoeye::AnalysisOptions options,
                                  const QcRunCallbacks& callbacks, const QcRunBudgets& budgets,
                                  const std::shared_ptr<AnalysisTaskSlot>& slot) {
    QcRunResult output;
    output.profile_id = profile.id;

    // 输入校验分两条口径：本地文件要存在且非空；网络源只要求地址非空 —— 以前这里
    // 统一走 IsAnalyzableFile，于是 http/rtmp/rtsp 一律被判成"文件不存在"。
    std::string reason;
    if (!IsAnalyzableInput(input, reason)) {
        output.ok = false;
        output.error = reason;
        return output;
    }
    const std::string& path = input.uri;

    const auto started_at = std::chrono::steady_clock::now();
    videoeye::qc::QcRuleEngine rules_engine(BuildRulesForProfile(profile));

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
        std::shared_ptr<videoeye::AnalysisEngine> engine; // 自持有: detach 后线程仍安全
        std::shared_ptr<std::atomic<bool>> cancel_flag;   // 本次分析的取消源
        std::string path;
        videoeye::AnalysisOptions options;
        std::function<void(double, const std::string&)> progress; // 调用方的进度回调
        // "距开始多久"的毫秒数，进度回调与结算都会刷新它 —— io_idle_timeout_ms 判的是
        // "距上一次动静"，与总时长无关（跑了 10 分钟但一直在出结果不算卡住）。
        std::atomic<long long> activity_ms{0};
        std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
        // 放弃这次分析的原因（决定收尾文案与 timed_out 标记）
        QcAbandonReason abandon_reason = QcAbandonReason::None;
        // 并发额度：由**分析线程体**在真正退出时归还（见 AnalysisTaskRegistry）。
        std::shared_ptr<AnalysisTaskSlot> slot;
    };
    auto box = std::make_shared<Box>();
    // 额度要么由调用方（批量调度）占好带进来，要么在这里登记一条 —— 两种情况下
    // "当前实际存活多少条分析"都可观测，且都在线程体退出时才归还。
    box->slot = slot ? slot : AnalysisTaskRegistry::Instance().AcquireUnbounded();
    // 引擎含 atomic 成员、不可移动, 只能按 shared_ptr 交给 worker —— 盒子里放 shared_ptr
    // 而不是 .get() 出来的裸指针, 就是"任务只携带自持有的 shared state"的形态。
    box->engine = std::make_shared<videoeye::AnalysisEngine>();
    box->cancel_flag = std::make_shared<std::atomic<bool>>(false);
    box->path = path;
    box->options = std::move(options);
    // 计时基准：放弃判定的 elapsed / idle 都相对它（盒子自带默认值，这里与本次开跑对齐）
    box->started_at = started_at;

    // 以前这里要通过 QtAnalysisController 绕一圈 Qt 信号（还要挂 QObject 上下文），
    // 现在直接用 AnalysisEngine 的回调 —— QC 批处理不需要任何 Qt 事件循环。
    //
    // 回调**在 worker 栈上构造**、按值捕获盒子: 盒子自己绝不持有这个回调。
    // 若把回调存进盒子(回调又捕获盒子), 就是一个 shared_ptr 环 —— 每次分析都留下
    // 一个永不释放的盒子(结果数据还在里面), ASan/LSan 必报泄漏。回调只可能在
    // Run() 期间触发, 那时盒子必然活着(worker 自己就持有一份强引用)。
    if (callbacks.progress)
        box->progress = callbacks.progress;

    // 取消源用盒子自持的那颗原子标志: 调用方的 should_cancel 在等待循环里被转成它的置位,
    // 引擎的所有取消轮询点（含 FFmpeg 中断回调）都看它 —— 取消只有这一颗, 不再有两套。
    std::thread worker([box]() {
        // 额度认领 + 退出时归还（含被 detach 的那条路径）：
        // "分析任务数"统计的是线程体的真实寿命，不是 analyze() 的返回时点。
        // MarkExiting 在归还之前跑，让派发侧看得见"这条已经在退了"。
        if (box->slot)
            box->slot->Adopt();
        const auto release_slot = task::MakeScopeExit([box]() {
            if (!box->slot)
                return;
            box->slot->MarkExiting();
            box->slot->Release();
        });

        // 进度/结算都算"有动静"：io_idle_timeout_ms 判的是"距上一次动静"。
        const auto touch = [box]() {
            const auto elapsed = std::chrono::steady_clock::now() - box->started_at;
            box->activity_ms.store(
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
        };
        videoeye::AnalysisCallbacks effective;
        effective.on_finished = [box, touch](bool, const model::AnalysisResult& result) {
            touch();
            {
                std::lock_guard<std::mutex> lock(box->mutex);
                box->result = result;
                box->settled = true;
            }
            box->cv.notify_all();
        };
        effective.on_failed = [box, touch](const std::string& message) {
            touch();
            {
                std::lock_guard<std::mutex> lock(box->mutex);
                box->failed = true;
                box->error = message;
                box->settled = true;
            }
            box->cv.notify_all();
        };
        // 进度回调**无条件**安装：它先刷新"距上一次动静"的活动时间，再按需转发给外部
        // 订阅者。
        //
        // 以前只有 box->progress 非空时才装 —— 于是批量入口（MakeAnalyzeFunction 没有
        // 进度回调）那条路上，即使引擎一直在出进度，也没人刷新 activity_ms；一旦配了
        // io_idle_timeout_ms，一个正常运行很久的分析会被判成"输入无响应"而放弃。
        // 空闲超时是执行器自己的判据，不该依赖调用方有没有订阅进度。
        effective.on_progress = [box, touch](double percent, const std::string& stage) {
            touch();
            if (box->progress)
                box->progress(percent, stage);
        };
        // 异常必须就地收口：这是裸 std::thread 的线程体，任何逃逸的异常都会直接
        // std::terminate()（连等待线程一起带走），任务永远等不到结算 —— 分析器 /
        // 解析器抛异常就从"进程没了"降级成"这条任务失败"。
        // 收口模板与其它线程入口共用同一份（core/domain/task/ThreadBody.h），
        // 不再在这里抄一遍 try/catch —— 抄的那份迟早会漏或会跑偏。
        const std::optional<std::string> failure =
            task::GuardBody([&]() { box->engine->Run(box->path, box->options, effective, nullptr, box->cancel_flag); });
        if (failure) {
            NotifyFailed(effective, std::string("分析异常: ") + *failure);
        }
        // 资源回收见证: 正常结算时这行紧跟返回；超时 detach 场景下它是"被放弃的后台线程
        // 终于退出、FFmpeg / 内存 / 磁盘 / CPU 归还"的唯一可观测信号（盒子由线程自持,
        // AnalyzeFile 早已返回, 没有别的出口能报这件事）。
        LOG_DEBUG("QcRunner: 分析线程退出 (" + box->path + ")");
    });

    // 等结果：三项策略分别判（见 QcRunBudgets.h）。
    //
    // ❗ 这里**不**用"总时长"当唯一的放弃条件：analysis_budget_ms = 0 表示不限时，
    // 正常运行多久都不该被判超时（评审 P1-1 的根因就是"跑过 5 秒被误杀"）。
    // 放弃只可能来自：调用方取消 / 超预算 / IO 长时间无动静。
    QcWaitState wait_state;
    QcAbandonReason abandon_reason = QcAbandonReason::None;
    auto abandon_at = std::chrono::steady_clock::now();
    // 首次触发收尾的原因在此**定格**（只有第一次算数）。之后引擎无论自己结算成什么
    // （完成 / 取消 / 失败），业务分类都按它来 —— 线程是否需要脱离只决定回收方式。
    // 写盒子而不仅是局部变量：等待循环结束之后还要靠它收尾（见下面的结算段）。
    // 全程持有 box->mutex（等待循环就在这把锁下跑），工作线程不碰这个字段。
    const auto record_abandon = [&box](QcAbandonReason reason) {
        if (reason != QcAbandonReason::None && box->abandon_reason == QcAbandonReason::None)
            box->abandon_reason = reason;
    };
    {
        std::unique_lock<std::mutex> lock(box->mutex);
        while (!box->settled) {
            const auto now = std::chrono::steady_clock::now();
            wait_state.elapsed_ms =
                static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - box->started_at).count());
            wait_state.idle_ms = wait_state.elapsed_ms - static_cast<int>(box->activity_ms.load());
            wait_state.reason = abandon_reason;
            if (abandon_reason != QcAbandonReason::None) {
                wait_state.abandon_elapsed_ms =
                    static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - abandon_at).count());
            }
            // 调用方的取消请求(批处理任务的取消按钮/面板回收)在这一刻转成"判定放弃"，
            // 并开始计 cancel_wait_ms —— 引擎卡在 FFmpeg 网络 IO 上时也能被它打断。
            if (abandon_reason == QcAbandonReason::None && callbacks.should_cancel && callbacks.should_cancel()) {
                abandon_reason = QcAbandonReason::Cancelled;
                abandon_at = now;
                record_abandon(abandon_reason);
            }
            const QcWaitDecision decision = DecideWait(budgets, wait_state);
            if (decision == QcWaitDecision::Abandon) {
                box->timed_out = true;
                record_abandon(abandon_reason);
                break;
            }
            if (decision == QcWaitDecision::RequestCancel) {
                // 由预算/空闲判出的放弃：区分是因为处置方式不同（超预算去看文件/网络，
                // IO 无响应去看 IO/超时配置）。
                abandon_reason = (budgets.analysis_budget_ms > 0 && wait_state.elapsed_ms >= budgets.analysis_budget_ms)
                                     ? QcAbandonReason::OverBudget
                                     : QcAbandonReason::IoStalled;
                abandon_at = now;
                // ❗ 判定即定格：引擎可能就在这之后、取消生效之前自己结算回来，
                // 那时不能把"超预算"改写成"用户取消"（批量统计会失真）。
                record_abandon(abandon_reason);
                // 立刻复核而不是先睡一个轮询片：cancel_wait_ms = 0 的语义是"判定即放弃"，
                // 多等 1ms 就可能让引擎抢在前面结算，把"放弃"变成"取消" —— 放弃原因与
                // timed_out 标记就此失真（批量侧会把超预算的条目记成用户取消）。
                continue;
            }
            if (abandon_reason != QcAbandonReason::None) {
                box->cancel_flag->store(true, std::memory_order_release);
                box->engine->Cancel();
            }
            box->cv.wait_for(lock, NextWaitSlice(budgets, wait_state));
        }
    }

    if (!box->timed_out) {
        // 已经结算, join 只是收句柄(瞬间返回)。
        worker.join();
    } else if (worker.joinable()) {
        // 放弃这条线程: detach **之前**必须先强制取消。
        // 等待循环可能一次都没进（预算极小时第一轮就命中 Abandon），上面那条
        // should_cancel -> cancel_flag 的转换因此根本没机会执行。不取消就 detach，
        // 后台 AnalysisEngine 会继续跑到自然结束，FFmpeg / 内存 / 磁盘 / CPU 继续被
        // 占用 —— 批量任务取消后会积累大量脱离的后台分析线程。
        //
        // 这里只持盒子(shared_ptr 自持, 含引擎本体), detach 后线程自洽, 不会读到已析构
        // 的引擎；取消则保证它尽快自己收尾。额度由线程体自己归还（见上面的 ScopeExit）。
        //
        // ❗ 先把它标成"正在退出"再 detach：从这一刻起到线程体真正退出，这条任务仍占着
        // FFmpeg / 内存 / 磁盘，派发侧必须看得见（评审 P2-3）—— 否则 max_parallel 只
        // 约束了"已返回的任务数"，退不干净的那批会悄悄超额。
        if (box->slot)
            box->slot->MarkExiting();
        box->cancel_flag->store(true, std::memory_order_release);
        box->engine->Cancel();
        LOG_WARN("QcRunner 判定放弃后台分析 (" + box->path + "), 原因=" + AbandonErrorText(abandon_reason) +
                 ", 线程转入受控回收");
        worker.detach();
    }

    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    output.elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

    // 收尾分类只看**首次触发收尾的原因**（评审 P1-1/P2 遗留）。
    //
    // 以前这里看的是 box->timed_out（"线程有没有被脱离"），于是同一个超预算事件：
    // 引擎退得慢 -> detach -> 记超时；引擎退得快 -> 自己结算回来 -> 记"用户取消"。
    // 回收方式与业务口径被混成了一个量，批量汇总里的 timed_out / cancelled 随之失真。
    // 现在 detach 只影响"线程怎么回收"，分类由 abandon_reason 说了算。
    QcAbandonReason final_reason = QcAbandonReason::None;
    {
        std::lock_guard<std::mutex> lock(box->mutex);
        final_reason = box->abandon_reason;
    }
    const QcAbandonSettlement settlement = SettleAbandon(final_reason, AbandonErrorText(final_reason));
    if (settlement.applies) {
        output.ok = false;
        output.timed_out = settlement.timed_out;
        output.error = settlement.error;
        output.analysis.scan_status = model::AnalysisStatus::Failed;
        output.analysis.error_message = output.error;
        return output;
    }

    if (box->failed) {
        output.ok = false;
        output.error = box->error;
        // 把扫描失败的状态也带出来，便于上层区分
        output.analysis.scan_status = model::AnalysisStatus::Failed;
        if (!box->error.empty())
            output.analysis.error_message = box->error;
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
        output.error = output.analysis.error_message.empty() ? "分析失败" : output.analysis.error_message;
        return output;
    case model::AnalysisStatus::Complete:
    case model::AnalysisStatus::Sampled:
        break; // 到达终态，继续判定
    }

    if (!output.analysis.error_message.empty()) {
        output.ok = false;
        output.error = output.analysis.error_message;
        return output;
    }

    output.ok = true;
    return output;
}

QcAnalyzeFn QcRunner::MakeAnalyzeFunction(const QcProfile& profile, videoeye::AnalysisOptions options) {
    // profile / options 在闭包之间共享（只读），cancel 状态每次调用由 request 带入。
    auto shared = std::make_shared<std::pair<QcProfile, videoeye::AnalysisOptions>>(profile, options);

    return [shared](const QcAnalyzeRequest& request) -> QcRunResult {
        QcRunner runner;
        QcRunCallbacks callbacks;
        callbacks.should_cancel = [&request]() {
            return request.cancel != nullptr && request.cancel->load(std::memory_order_acquire);
        };
        // 三项等待策略随请求带入（批量调度从 BatchQcOptions 透传）；并发额度同样随请求
        // 交给执行器，由分析线程体在真正退出时归还。
        return runner.AnalyzeFile(request.path, shared->first, shared->second, callbacks, request.budgets,
                                  request.slot);
    };
}

} // namespace qc
} // namespace videoeye