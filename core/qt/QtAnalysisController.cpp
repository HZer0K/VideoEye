#include "core/qt/QtAnalysisController.h"

#include <utility>

#include <QMetaType>

namespace videoeye {
namespace qt {

QtAnalysisController::QtAnalysisController(QObject* parent) : QObject(parent) {
    // AnalysisResult 要跨线程走队列连接，必须先在元类型系统里注册，
    // 否则排队时会报 "QObject::connect: Cannot queue arguments" 而槽永远不被调用。
    qRegisterMetaType<analyzer::AnalysisResult>("videoeye::analyzer::AnalysisResult");
    qRegisterMetaType<analyzer::AnalysisResult>("AnalysisResult");

    // 工作线程体返回后通知回本对象所在线程，用来接手排队请求。
    // 显式声明 QueuedConnection: 发送方是纯 std::thread，接收方在 UI 线程，
    // 必须排队（顺便也避免任何人以后把本对象 moveToThread 之后语义悄悄变直连）。
    connect(this, &QtAnalysisController::WorkerExited,
            this, &QtAnalysisController::OnWorkerExited, Qt::QueuedConnection);
}

QtAnalysisController::~QtAnalysisController() {
    // 析构路径可以等（此时已经没有界面要响应），但仍不接受新的排队请求。
    shutting_down_ = true;
    pending_.reset();
    Cancel();
    if (worker_.joinable()) worker_.join();
}

quint64 QtAnalysisController::StartAnalysis(const std::string& file_path,
                                            const analyzer::AnalysisOptions& options) {
    Cancel();
    // 代际号**立刻**分配并返回：调用方（诊断页）拿它做结果过滤。排队也不改变语义 ——
    // 这次请求将来完成时发出的仍是这个号，旧任务迟到的回包照样被丢弃。
    const quint64 gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (worker_.joinable() && !worker_finished_.load(std::memory_order_acquire)) {
        // 旧线程**真的**还在跑（引擎卡在不可中断的调用里退不出来）。
        // **绝不在这里 join**：那会在 UI 线程上等，正是"点取消/重新扫描界面冻住"的根源。
        // 改成排队，等旧线程退出的 WorkerExited 到达后再启动。
        pending_ = PendingRequest{file_path, options, gen};
        // 对 UI 而言"本次分析已在途"：否则 IsRunning() 会在旧线程收尾的瞬间闪成 false，
        // 表现成按钮状态抖一下。
        running_.store(true, std::memory_order_release);
        return gen;
    }

    // 走到这里有两种情况:
    //   a) 没有旧线程；
    //   b) 有句柄但**线程体已经返回**（上一轮的 WorkerExited 早已被处理，那时没有排队
    //      请求，于是它什么也没做就结束了）。
    // (b) 是必须在这里处理的关键路径: 那条通知已经被消费掉，没人会再来叫醒排队请求，
    // 若还按 (a) 之外的逻辑排队，第二次分析就会**永久卡在 pending_**（界面上表现为
    // 只能成功分析一次的"扫描中"）。这里 join 只是收句柄 —— 线程体已返回，不会阻塞。
    ReapFinishedWorker();
    Launch(file_path, options, gen);
    return gen;
}

void QtAnalysisController::Launch(const std::string& file_path, const analyzer::AnalysisOptions& options,
                                  quint64 gen) {
    engine_.Reset();
    running_.store(true, std::memory_order_release);
    worker_generation_.store(gen, std::memory_order_release);

    analyzer::AnalysisCallbacks callbacks;
    callbacks.on_progress = [this, gen](double percent, const std::string& stage) {
        emit ProgressReported(gen, percent, QString::fromStdString(stage));
    };
    callbacks.on_failed = [this, gen](const std::string& message) {
        running_.store(false, std::memory_order_release);
        emit AnalysisFailed(gen, QString::fromStdString(message));
    };
    callbacks.on_finished = [this, gen](bool completed, const analyzer::AnalysisResult& result) {
        running_.store(false, std::memory_order_release);
        emit AnalysisFinished(gen, completed, result);
    };

    // 必须在 start() 之前置 false: 线程一起来就可能立刻结束，晚一步写就会看到
    // "线程已死但标志还是 false"，把一次可以直接启动的请求误排进队列。
    worker_finished_.store(false, std::memory_order_release);
    worker_ = std::thread([this, gen, file_path, options, callbacks]() {
        engine_.Run(file_path, options, callbacks);
        // 先立"线程体已返回"标志，再投递通知。顺序不能反: StartAnalysis 可能在这条
        // 通知被处理**之前**就被调用，那时它靠这个标志判断句柄可以就地回收并直接开
        // 新任务，而不是傻等一条已经被消费掉的通知。
        worker_finished_.store(true, std::memory_order_release);
        // 线程体结束：通知 UI 线程收拾本代际的句柄并接手排队请求。
        // 信号是排队投递的，所以 AnalysisFinished 一定先被处理，顺序不会乱。
        emit WorkerExited(gen);
    });
}

void QtAnalysisController::ReapFinishedWorker() {
    // 线程体已返回时 join 只是收句柄，不会长时间阻塞。
    if (worker_.joinable()) worker_.join();
}

void QtAnalysisController::OnWorkerExited(quint64 finished_generation) {
    // 迟到 / 过期的通知直接忽略（比如它已被更晚的一次启动取代）。
    // 注意这里**不能顺手 join**: 该代际的句柄已经被新的 worker_ 顶替，那个是真在跑的。
    if (finished_generation != worker_generation_.load(std::memory_order_acquire)) return;

    // 无条件回收本代际的句柄（线程体此刻已返回）。以前是"有排队请求才 join"，
    // 于是没有 pending 的那次会留下一个 joinable 但早已结束的句柄，让下一次
    // StartAnalysis 误判成"旧线程还活着"。
    if (worker_finished_.load(std::memory_order_acquire)) ReapFinishedWorker();

    if (shutting_down_ || !pending_.has_value()) return;

    const PendingRequest req = *pending_;
    pending_.reset();
    Launch(req.file_path, req.options, req.generation);
}

void QtAnalysisController::Cancel() {
    // 排队中的请求一并作废：用户点了取消就不该再"自作主张"开始下一轮扫描。
    pending_.reset();
    engine_.Cancel();
}

bool QtAnalysisController::IsRunning() const {
    return running_.load(std::memory_order_acquire) || pending_.has_value();
}

} // namespace qt
} // namespace videoeye
