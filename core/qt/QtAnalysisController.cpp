#include "core/qt/QtAnalysisController.h"

#include <cstdio>
#include <utility>

#include <QMetaType>
#include <QPointer>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace qt {

QtAnalysisController::QtAnalysisController(QObject* parent)
    : QObject(parent), alive_(std::make_shared<std::atomic<bool>>(true)) {
    std::fprintf(stderr, "[TF] controller ctor enter this=%p\n", static_cast<const void*>(this)); std::fflush(stderr);
    // AnalysisResult 要跨线程走队列连接，必须先在元类型系统里注册，
    // 否则排队时会报 "QObject::connect: Cannot queue arguments" 而槽永远不被调用。
    qRegisterMetaType<model::AnalysisResult>("videoeye::model::AnalysisResult");
    qRegisterMetaType<model::AnalysisResult>("AnalysisResult");

    // 工作线程体返回后通知回本对象所在线程，用来接手排队请求。
    // 显式声明 QueuedConnection: 发送方是纯 std::thread，接收方在 UI 线程，
    // 必须排队（顺便也避免任何人以后把本对象 moveToThread 之后语义悄悄变直连）。
    connect(this, &QtAnalysisController::WorkerExited,
            this, &QtAnalysisController::OnWorkerExited, Qt::QueuedConnection);
    std::fprintf(stderr, "[TF] controller ctor leave\n"); std::fflush(stderr);
}

QtAnalysisController::~QtAnalysisController() {
    std::fprintf(stderr, "[TF] controller dtor enter this=%p\n", static_cast<const void*>(this)); std::fflush(stderr);
    // 析构路径要设总预算：此时已经没有界面要响应，但也不接受无限等下去。
    shutting_down_ = true;
    if (alive_) alive_->store(false, std::memory_order_release);
    Shutdown();
    std::fprintf(stderr, "[TF] controller dtor leave\n"); std::fflush(stderr);
}

quint64 QtAnalysisController::StartAnalysis(const std::string& file_path,
                                            const analyzer::AnalysisOptions& options) {
    Cancel();
    // 代际号**立刻**分配并返回：调用方（诊断页）拿它做结果过滤。排队也不改变语义 ——
    // 这次请求将来完成时发出的仍是这个号，旧任务迟到的回包照样被丢弃。
    //
    // 归属 TaskManager 的好处: id 由它在锁内存好、锁内返回(见 TaskManager::Begin 的注释),
    // 所以这里拿到的号一定是"这次 Begin 自己那格", 不会被并发的另一个 Begin 顶掉。
    if (!WorkerBodyReturned()) {
        // 排队请求已经占着一个 handle 了（连着点了两次"重新扫描"）：先把上一次那个
        // 的终态还回去 —— BeginHandle 登记过就得有人还，否则那个 id 永远停在 Running，
        // 而这个 slot 又只有一个，后会拖得前一起不来。
        if (pending_) {
            tasks_.EndHandle(pending_->handle, task::TaskState::Canceled);
            pending_.reset();
        }
        // 旧线程**真的**还在跑（引擎卡在不可中断的调用里退不出来）。
        // **绝不在这里 join**：那会在 UI 线程上等，正是"点取消/重新扫描界面冻住"的根源。
        // 改成排队，等旧线程退出的 WorkerExited 到达后再启动。
        //
        // 但 id 得先占下来(与旧行为一致): 这个号将来完成时发的就是它,
        // 而且 Cancel() 要靠它把排队任务从 slot 上摘掉。
        const task::TaskHandle handle = tasks_.BeginHandle(kSlot, 0, task::TaskKind::Cooperative);
        if (!handle.valid())
            return 0;  // 并发已满：交给调用方按失败处理（旧实现是永远排在 pending_ 里）
        pending_ = PendingRequest{file_path, options, handle};
        return handle.id;
    }

    // 走到这里有两种情况:
    //   a) 没有旧线程；
    //   b) 有盒子但**线程体已经返回**（上一轮的 WorkerExited 早已被处理，那时没有排队
    //      请求，于是它什么也没做就结束了）。
    // (b) 是必须在这里处理的关键路径: 那条通知已经被消费掉，没人会再来叫醒排队请求，
    // 若还按 (a) 之外的逻辑排队，第二次分析就会**永久卡在 pending_**（界面上表现为
    // 只能成功分析一次的"扫描中"）。这里 join 只是收句柄 —— 线程体已返回，不会阻塞。
    ReapWorker();
    const task::TaskHandle handle = tasks_.BeginHandle(kSlot, 0, task::TaskKind::Cooperative);
    if (!handle.valid())
        return 0;
    Launch(file_path, options, handle);
    return handle.id;
}

void QtAnalysisController::Launch(const std::string& file_path, const analyzer::AnalysisOptions& options,
                                  const task::TaskHandle& handle) {
    // 上一条工作线程一定已经退出了（到这里之前都判过 worker_finished_），
    // join 只是收句柄。这里再兜一次，免得将来有人漏判一步就把线程赋值成 joinable 的
    // 旧线程 —— 那会直接 std::terminate。
    ReapWorker();

    worker_id_ = handle.id;

    // 引擎与这次分析的数据一起装进盒子。AnalysisEngine 有 atomic 成员、不可移动，
    // 所以只能原地构造在盒子里，不能先建好再搬。
    auto box = std::make_shared<Box>();
    box->handle = handle;
    box->options = options;
    box->path = file_path;
    box->alive = alive_;

    // 回调统一带上 handle.id —— 这就是"代际"。它和 slot 的当前任务是同一个号，
    // 所以 facade 那边 `generation != generation()` 的比较等价于
    // TaskManager::IsCurrent 的语义，但不需要再维护第二套编号。
    //
    // alive 是从宿主那儿借来的一颗标志（shared_ptr 保活）：关闭超预算放弃线程时
    // 宿主会先把它置 false，任务体此后就不再 emit —— 否则它会在一个已经销毁的
    // QObject 上排队消息。
    // QPointer 而不是裸 this: 关闭超预算放弃线程时宿主可能已经析构了，
    // QPointer 会正好失效 —— 这就是"任务体还在 emit"这件事唯一的可靠挡箭牌。
    const QPointer<QtAnalysisController> self(this);
    std::weak_ptr<Box> weak_box = box;
    // 线程体收尾时把"活干完了"这件事报回宿主线程。它是在**工作线程**里调用的，
    // 所以只做 emit（信号走队列，实际处理仍在宿主线程），不做任何判断之外的动作。
    box->on_body_done = [self, weak_box](task::TaskId id) {
        // 只判空、不碰对象 —— 宿主已析构时这段代码仍会被调用，一旦 deref 就是悬空访问。
        if (!self)
            return;
        emit self->WorkerExited(id);
    };

    analyzer::AnalysisCallbacks& callbacks = box->callbacks;
    callbacks.on_progress = [self, alive = alive_, id = handle.id](double percent, const std::string& stage) {
        if (!self || !alive->load(std::memory_order_acquire)) return;
        emit self->ProgressReported(id, percent, QString::fromStdString(stage));
    };
    callbacks.on_failed = [self, alive = alive_, id = handle.id](const std::string& message) {
        if (!self || !alive->load(std::memory_order_acquire)) return;
        emit self->AnalysisFailed(id, QString::fromStdString(message));
    };
    callbacks.on_finished = [self, alive = alive_, id = handle.id](bool completed,
                                                                  const model::AnalysisResult& result) {
        if (!self || !alive->load(std::memory_order_acquire)) return;
        emit self->AnalysisFinished(id, completed, result);
    };

    // 引擎的取消源就是这颗令牌 —— 整个控制器现在只有这一处取消来源，
    // Cancel() 置位 slot 令牌，下面的线程体与逐包扫描/清单解析都看它。
    const task::CancelToken token = handle.cancel;
    // 在工作线程启动之前把盒子挂上，这样"线程体有没有回来"从一开始就有地方查。
    worker_box_ = box;
    // 线程体**只按值捕获盒子与令牌**，不碰 this：关闭预算耗尽时它会被 detach，
    // 那时宿主比它先死，任何对 this 的访问都是悬空的。
    // 顺序也要注意：box->on_body_done 必须在起线程之前填好，线程一起来就可能立刻走到收尾。
    // 工作线程**不碰 tasks_**：关预算耗尽时它会被 detach，那时 tasks_ 比它先析构，
    // 从线程里调 EndHandle 就是悬空访问。终态一律由宿主在宿主线程里归还
    //（见 SettleTask），工作线程只负责"我回来了"这一件事。
    worker_ = std::thread([box, token]() {
        box->engine.Run(box->path, box->options, box->callbacks, nullptr, token.flag());
        // 先立"线程体已返回"标志，再投递通知。顺序不能反: StartAnalysis 可能在这条
        // 通知被处理**之前**就被调用，那时它靠这个标志判断句柄可以就地回收并直接开
        // 新任务，而不是傻等一条已经被消费掉的通知。
        box->body_done.store(true, std::memory_order_release);
        // 线程体结束：通知 UI 线程收拾本代际的句柄并接手排队请求。
        // 信号是排队投递的，所以 AnalysisFinished 一定先被处理，顺序不会乱。
        if (box->on_body_done)
            box->on_body_done(box->handle.id);
    });
}

bool QtAnalysisController::Alive() const {
    return alive_ && alive_->load(std::memory_order_acquire);
}

bool QtAnalysisController::WorkerBodyReturned() const {
    // 没有盒子（从没起过线程 / 已经回收）就当作"没有旧线程"。
    if (!worker_box_)
        return true;
    return worker_box_->body_done.load(std::memory_order_acquire);
}

bool QtAnalysisController::AwaitWorker(int budget_ms) {
    // 拷贝一份 shared_ptr：盒子由宿主与工作线程共同持有，等的就是盒子里那颗标志。
    const std::shared_ptr<Box> box = worker_box_;
    if (!box)
        return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!box->body_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void QtAnalysisController::ReapWorker() {
    // 线程体已返回时 join 只是收句柄，不会长时间阻塞。
    if (worker_.joinable()) worker_.join();
    // 盒子只被这里与工作线程握着；work 线程一结束就没别人了，可以放掉。
    // 注意别在 join 之前 reset —— 那样工作线程收尾时访问的盒子就只剩它自己那一份，
    // 虽然还活着（shared_ptr 保活）但语义上等于在线程自己手上有对象时被回收。
    worker_box_.reset();
    worker_id_ = 0;
}

void QtAnalysisController::SettleTask(const task::TaskHandle& handle, task::TaskState terminal) {
    // EndHandle 内部会先比 id：只有"这个任务还是 slot 的当前任务"才写得进去 ——
    // 被新任务取代、或者自己那次已经被 Cancel 掉，都写不进去，不会出现"旧任务把
    // 新任务的终态顶掉"。终态一次性，所以并发下重复调用也只会有一个生效。
    tasks_.EndHandle(handle, terminal);
}

void QtAnalysisController::OnWorkerExited(task::TaskId exited_id) {
    // 迟到 / 过期的通知直接忽略（比如它已被更晚的一次启动取代）。
    // 注意这里**不能顺手 join**：该代际的句柄已经被新的 worker_ 顶替，那个是真在跑的。
    if (exited_id != worker_id_ || !worker_box_) return;

    // 无条件回收本代际的句柄（线程体此刻已返回）。以前是"有排队请求才 join"，
    // 于是没有 pending 的那次会留下一个 joinable 但早已结束的句柄，让下一次
    // StartAnalysis 误判成"旧线程还活着"。
    const task::TaskHandle handle = worker_box_->handle;
    const bool body_returned = WorkerBodyReturned();
    if (body_returned) {
        // 判取消用的是这个任务自己的令牌，不是 slot 上"当前"那颗 ——
        // 期间若又 Begin 过，slot 上那颗已经是新任务的了，拿它判会把新任务误记成取消。
        const bool cancelled = handle.cancel.IsCanceled();
        SettleTask(handle, cancelled ? task::TaskState::Canceled : task::TaskState::Succeeded);
        ReapWorker();
    }

    if (shutting_down_ || !pending_.has_value()) return;

    PendingRequest req = std::move(*pending_);
    pending_.reset();
    Launch(req.file_path, req.options, req.handle);
}

void QtAnalysisController::Cancel() {
    std::fprintf(stderr, "[TF] cancel enter\n"); std::fflush(stderr);
    // 排队中的请求一并作废：用户点了取消就不该再"自作主张"开始下一轮扫描。
    // 它已经占了一个 id（BeginHandle 时登记过），必须把终态还回去，否则那个 slot
    // 会一直停在 Running。
    std::fprintf(stderr, "[TF] cancel check pending\n"); std::fflush(stderr);
    if (pending_) {
        std::fprintf(stderr, "[TF] cancel pending engaged id=%llu slot=%s\n", (unsigned long long)pending_->handle.id, pending_->handle.slot.c_str()); std::fflush(stderr);
        tasks_.EndHandle(pending_->handle, task::TaskState::Canceled);
        pending_.reset();
    }
    // 置位 slot 上的取消令牌。工作线程里的引擎（Run 的 cancel_source）与
    // TaskManager 的终止判定看的是同一颗标志 —— 以前是 Cancel() 直接调 engine_.Cancel()，
    // 引擎那一颗与 slot 令牌互不相干，等于两套取消来源。
    std::fprintf(stderr, "[TF] cancel before tasks_.Cancel\n"); std::fflush(stderr);
    tasks_.Cancel(kSlot);
    std::fprintf(stderr, "[TF] cancel leave\n"); std::fflush(stderr);
}

bool QtAnalysisController::IsRunning() const {
    return tasks_.IsRunning(kSlot) || pending_.has_value();
}

void QtAnalysisController::Shutdown() {
    std::fprintf(stderr, "[TF] shutdown enter\n"); std::fflush(stderr);
    // 先作废排队请求与取消令牌：任务体（引擎）看的就是这颗令牌。
    Cancel();
    std::fprintf(stderr, "[TF] shutdown before AwaitWorker\n"); std::fflush(stderr);
    const bool body_returned = AwaitWorker(kJoinBudgetMs);
    std::fprintf(stderr, "[TF] shutdown before handle\n"); std::fflush(stderr);
    const task::TaskHandle handle = worker_box_ ? worker_box_->handle : task::TaskHandle{};

    std::fprintf(stderr, "[TF] shutdown before joinable\n"); std::fflush(stderr);
    if (worker_.joinable()) {
        if (body_returned) {
            ReapWorker();  // 线程体已返回，收句柄
            return;
        }
        // 预算耗尽。任务体明明是协作式的（引擎会在取消令牌置位后收尾），走到这里说明它
        // 卡在 FFmpeg 里没把令牌看一眼 —— 这种时候再等下去就是"关界面关不掉"。
        // 只能放弃：所有 emit 都被 Alive() 挡着（上面已置 false），最坏是丢一次回包，
        // 而不会让签发线程永远挂住退出流程。
        LOG_WARN("分析工作线程在 " + std::to_string(kJoinBudgetMs) +
                 "ms 内未响应取消，关闭时放弃该线程（任务体应轮询取消令牌）");
        worker_.detach();   // 线程只握着盒子，盒子自己保活到跑完
        worker_id_ = 0;
        // 放弃之后它自己不会再回来写终态了，这里补一个 —— 否则 slot 永远停在 Running。
        // （交由 SettleTask：终态一次性，真跑回来写过的那次会保留。）
        if (handle.valid())
            SettleTask(handle, task::TaskState::Canceled);
        return;
    }

    // 线程体早已返回（OnWorkerExited / 上一条分支里收过），终态已经还过了 ——
    // SettleTask 是终态一次性的，这里什么都不用做。
    std::fprintf(stderr, "[TF] shutdown before LOG_INFO\n"); std::fflush(stderr);
    LOG_INFO("分析工作线程已退出，关闭收尾完成");
}

} // namespace qt
} // namespace videoeye
