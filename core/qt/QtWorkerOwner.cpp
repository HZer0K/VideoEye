#include "core/qt/QtWorkerOwner.h"

#include <algorithm>
#include <string>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace qt {

namespace {
// 析构时的最后等待预算: 析构路径上没有 UI 可响应，卡太久会让整个退出流程假死。
constexpr int kDestructorWaitMs = 3000;
} // namespace

QtWorkerOwner::QtWorkerOwner(QObject* parent) : QObject(parent) {}

QtWorkerOwner::~QtWorkerOwner() {
    StopAll(kDestructorWaitMs);

    // 走到这里还没退出来的线程（典型场景: FFmpeg 卡在网络 IO / 驱动调用里，
    // quit() 根本传不进去），绝不能让它跟着本对象一起被销毁 —— QThread 对象被析构
    // 而线程仍在跑，Qt 会直接报 "QThread: Destroyed while thread is still running"。
    //
    // 处理办法: 断开全部连接（避免回调打到已经析构的宿主）后脱管，
    // 把这个线程连同 worker 一起泄漏掉。泄漏一个卡死的线程（进程退出时由 OS 回收）
    // 的代价，远小于析构期崩溃。
    for (Entry& e : entries_) {
        if (!e.thread) continue;
        LOG_ERROR("后台 worker 线程超时未退出, 已脱管(避免析构期崩溃)");
        if (e.worker) e.worker->disconnect();
        e.thread->disconnect();
        e.thread->setParent(nullptr);  // 从子对象列表摘掉 -> QObject 析构不会 delete 它
    }
    entries_.clear();
}

QThread* QtWorkerOwner::StartWorker(QObject* worker, std::function<void()> body,
                                    std::function<void()> request_stop,
                                    std::function<void(QThread*)> on_finished,
                                    std::function<void(QThread*, const std::string&)> on_error) {
    // 不接协议：句柄留空，本方法退回到"只当线程持有者"，什么都不碰 TaskManager。
    return StartWorker(worker, std::move(body), task::TaskHandle{}, std::move(request_stop),
                       std::move(on_finished), std::move(on_error));
}

QThread* QtWorkerOwner::StartWorker(QObject* worker, std::function<void()> body,
                                    const task::TaskHandle& handle,
                                    std::function<void()> request_stop,
                                    std::function<void(QThread*)> on_finished,
                                    std::function<void(QThread*, const std::string&)> on_error) {
    if (!worker) return nullptr;

    // 句柄是调用方**已经 Begin 过**的那一个，直接沿用：slot 由调用方持有，
    // 本既不 Begin 也不 End，终态写入者永远只有一个。空句柄（id == 0）时，
    // 下面所有协议动作都被 valid() 短路掉。
    //
    // 这里只做 Owner 侧最小的一件事：把这个手柄记下来，好在线程退出的那一刻还回去。
    auto* thread = new QThread(this);
    // worker 不挂成 QThread 的子对象: 它住在新线程里，由 deleteLater 在该线程回收，
    // 挂父子关系反而会让 QThread 析构时从错误的线程 delete 它。
    worker->setParent(nullptr);
    worker->moveToThread(thread);

    Entry entry;
    entry.thread = thread;
    entry.worker = worker;
    entry.request_stop = std::move(request_stop);
    entry.handle = handle;
    entries_.push_back(std::move(entry));

    // body 在 worker 线程执行（接收者上下文是 worker，Qt 会自动判成直连）。
    // 任务体一返回就退出事件循环: 否则线程会一直挂在 exec() 上不结束 ——
    // 以前是靠调用方把 worker 的每个终态信号都连到 thread->quit，漏连一个就漏退一次。
    QObject::connect(thread, &QThread::started, worker,
                     [thread, body = std::move(body), on_error]() {
                         // quit() 必须与任务体结果无关 —— 成功、body 抛异常、on_error 自己
                         // 再抛，三条路都得退出事件循环，否则线程永远挂在 exec() 上。
                         // 手写quit() 意味着每加一条路径就漏一次，所以交给析构函数兜底：
                         // 就算当前正在栈展开（异常穿透出 lambda），作用域退出照样执行。
                         // 注意不能写成 finally 风格：这里没有异常规范保护的析构会吞掉
                         // 「新异常替换旧异常」的路径，RAII 是唯一稳的做法。
                         struct QuitOnScopeExit {
                             QThread* thread;
                             ~QuitOnScopeExit() { thread->quit(); }
                         } quit_guard{thread};

                         // 异常必须就地吃掉: Qt 的线程入口（QThreadPrivate::start 里
                         // 调 run()）不设 try/catch，异常一路逃逸出去就没人接，
                         // 进程直接 std::terminate() 退出 —— 一个 worker 里的
                         // std::bad_alloc / 断言失败就把整个程序带走了。
                         // 同时对应的 TaskManager 任务会永远停在 Running，没终态。
                         try {
                             body();
                         } catch (...) {
                             // 先归一化消息再重新抛：单一 catch(...) 之后没法再按类型拆，
                             // 也避免 ex.what() 这条路径自己的内存申请再抛一次。
                             std::string what;
                             try {
                                 throw;
                             } catch (const std::exception& ex) {
                                 what = std::string("worker 任务体抛出 std::exception: ") + ex.what();
                             } catch (...) {
                                 what = "worker 任务体抛出未知异常(非 std::exception)";
                             }

                             // 任务体失败的终态必须走出去(cancel 之外的唯一 Failed 来源)，
                             // 但 on_error 回调本身也可能抛 —— 上面那条 catch 正在处理异常，
                             // 没法再嵌套一层捕获。不在这里接住，异常会从这里穿过 Qt 线程入口。
                             if (on_error) {
                                 try {
                                     on_error(thread, what);
                                 } catch (const std::exception& ex) {
                                     LOG_ERROR(std::string("on_error 回调抛出 std::exception: ") + ex.what());
                                 } catch (...) {
                                     LOG_ERROR("on_error 回调抛出未知异常(非 std::exception)");
                                 }
                             }
                         }
                     });
    // 终态清理: 排队回所有者线程，worker 与 QThread 各自在正确的线程被回收。
    //
    // 这里**不** EndHandle：句柄是调用方 BeginHandle 出来的，TaskManager 也在调用方
    // 手里（本类不持调度器），本方法既没有它的实例可调，补一个 EndHandle 更是造出
    // 第二个终态写入者去抢同一个 slot —— 谁先落地算谁，这正是评审要收敛掉的那种
    // 多套体系。终态一律由调用方在它自己的三条路线上落（终态信号 / on_error /
    // 线程结束时的兜底），写入者从头到尾只有一个。
    //
    // 取消则与调用方共用同一颗令牌（见 StopWorker），"取消"不在这里另起一套。
    QObject::connect(thread, &QThread::finished, this, [this, thread, worker]() {
        worker->deleteLater();
        thread->deleteLater();
        RemoveEntry(thread);
    });

    // 调用方自己的终态清理回调（如清"当前任务"指针）。必须在 start() 之前连好:
    // 否则线程若秒级完成，finished 已发出而本回调还没连上，清理永不执行，
    // frame_exporter_/media_exporter_ 等成员会保留失效对象（下一次取消/启动即越界访问）。
    if (on_finished) {
        // 把已创建的 thread 传给回调，回调里直接拿形参比较，无需捕获本地变量。
        QObject::connect(thread, &QThread::finished, this,
                         [on_finished, thread]() { on_finished(thread); });
    }

    thread->start();
    return thread;
}

bool QtWorkerOwner::StopWorker(QThread* thread, int wait_ms) {
    if (!thread) return true;
    Entry* entry = FindEntry(thread);
    if (!entry) return true;  // 不是本对象持有的线程，不管

    // 两条动作都发，不互相替代：先置协议令牌（这是全项目"取消"的唯一来源，
    // 任务体/Exit 循环轮询的就是它），再调 request_stop —— 那是 worker 自己那层的
    // 一步（多数实现就是置 worker 自己的标志，让已经进到 Export() 内部的循环也能看到）。
    // 顺序上令牌在前：worker 的 Cancel() 内部多半也会去看这颗令牌。
    if (entry->handle.valid())
        entry->handle.cancel.RequestCancel();
    if (entry->request_stop)
        entry->request_stop();
    thread->quit();

    bool finished = false;
    if (thread == QThread::currentThread()) {
        // 从 worker 线程自己调进来时不能 wait(): 等自己退出 = 直接死锁
        finished = thread->isFinished();
    } else {
        finished = thread->wait(wait_ms);
    }

    if (finished) {
        RemoveEntry(thread);  // deleteLater 已由 finished 回调排队，对象交给 Qt 回收
        return true;
    }
    entry->retiring = true;  // 仍在跑: 转入待回收，句柄继续由本对象持有
    LOG_WARN("后台 worker 线程在 " + std::to_string(wait_ms) +
             "ms 内未退出, 转入待回收列表(句柄不丢弃)");
    return false;
}

int QtWorkerOwner::RemainingMs(std::chrono::steady_clock::time_point deadline, bool bounded) {
    if (!bounded) return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    return left > 0 ? static_cast<int>(left) : 0;
}

bool QtWorkerOwner::StopAll(int timeout_ms) {
    const bool bounded = timeout_ms >= 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(bounded ? timeout_ms : 0);

    // 先把停止请求发给所有线程，让它们并行退出；
    // 逐个"请求 + 等待"会把总耗时累加成 N * timeout。
    std::vector<QThread*> targets;
    targets.reserve(entries_.size());
    for (const Entry& e : entries_) {
        if (!e.thread) continue;
        // 同 StopWorker：令牌 + worker 各自那层，都置一遍。
        if (e.handle.valid())
            e.handle.cancel.RequestCancel();
        if (e.request_stop) e.request_stop();
        e.thread->quit();
        targets.push_back(e.thread);
    }

    bool all_stopped = true;
    for (QThread* thread : targets) {
        // StopWorker 内部会再发一次 request_stop / quit，二者都是幂等的
        if (!StopWorker(thread, RemainingMs(deadline, bounded))) all_stopped = false;
    }
    return all_stopped;
}

QtWorkerOwner::Entry* QtWorkerOwner::FindEntry(QThread* thread) {
    for (Entry& e : entries_) {
        if (e.thread == thread) return &e;
    }
    return nullptr;
}

void QtWorkerOwner::RemoveEntry(QThread* thread) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [thread](const Entry& e) { return e.thread == thread; }),
                   entries_.end());
}

bool QtWorkerOwner::IsActive(QThread* thread) const {
    if (!thread) return false;
    for (const Entry& e : entries_) {
        if (e.thread == thread) return true;
    }
    return false;
}

int QtWorkerOwner::ActiveCount() const {
    int n = 0;
    for (const Entry& e : entries_) {
        if (!e.retiring) ++n;
    }
    return n;
}

int QtWorkerOwner::RetiringCount() const {
    int n = 0;
    for (const Entry& e : entries_) {
        if (e.retiring) ++n;
    }
    return n;
}

} // namespace qt
} // namespace videoeye
