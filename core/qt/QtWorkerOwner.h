#pragma once

// 自带 QThread 的后台 worker 的统一所有者。
//
// 背景 (评审指出的问题): TaskManager 只管理 std::thread，而抽帧导出 / 媒体导出的
// worker 是 QObject（要发进度信号），必须留在 QThread 上。这类线程**不可强制终止** ——
// FFmpeg 可能正阻塞在网络 IO 或某个驱动的解码调用里，quit() 只是请求事件循环退出，
// 卡在 Export() 内部时根本收不到。于是原来那套"裸指针 + wait(5s)"的写法有两个真坑：
//   1. 取消超时后把 QThread* 直接置空 -> 句柄丢失，宿主析构时线程还在跑，Qt 直接报
//      "QThread: Destroyed while thread is still running"（多数情况下就是崩溃）；
//   2. 超时后照常启动新导出 -> 新旧两个任务同时写同一个 .part 临时文件。
//
// 约定:
//   * 线程与 worker 一律由本对象持有（QThread 是本对象的 QObject 子对象），
//     调用方拿到的裸指针只用来连线，不许自己 delete；
//   * 请求停止后线程从"在跑"转入"待回收"，仍然被持有、仍然会被继续等待，
//     直到真的 finished 才交给 Qt 回收 —— 超时只是"不再阻塞调用方"，不是"放弃这个线程"；
//   * 析构时再给一轮限时等待；仍然退不出来的线程主动断开全部连接并脱管
//     (setParent(nullptr))：泄漏一个卡死的线程，好过让它在宿主对象销毁后继续跑。
//
// 线程约定: 所有方法只应在"所有者所在线程"（通常是 UI 线程）调用；
// finished 回调经 Qt 排队回到该线程，因此内部容器不需要加锁。

#include <chrono>
#include <functional>
#include <vector>

#include <QObject>
#include <QThread>

namespace videoeye {
namespace qt {

class QtWorkerOwner : public QObject {
    Q_OBJECT

public:
    explicit QtWorkerOwner(QObject* parent = nullptr);
    ~QtWorkerOwner() override;

    QtWorkerOwner(const QtWorkerOwner&) = delete;
    QtWorkerOwner& operator=(const QtWorkerOwner&) = delete;

    // 起一个归本对象所有的 worker 线程。
    //   worker       —— 会被 moveToThread，生命周期归本对象
    //   body         —— 线程启动后在 worker 线程执行（通常就是 worker->Export(...)）
    //   request_stop —— 请求停止时先调它（置 worker 自己的取消标志），再 quit
    //   on_finished  —— 线程终态时在本对象线程执行的清理回调（如清"当前任务"指针）。
    //                   签名为 void(QThread*)，由本方法把已创建的 thread 传入，
    //                   回调里直接用形参比较即可，无需捕获尚未声明的本地 thread 变量。
    //                   必须在 thread->start() 之前由本方法连接，否则线程若秒级完成、
    //                   调用方还没连上 finished，清理回调永不执行、成员指针保留失效对象。
    //   on_error     —— body 抛异常时的失败回调，签名 void(QThread*, const std::string&)。
    //                   在**线程**上下文执行（与 body 同级），调用方需要跨线程投递。
    //                   这是导出任务写入 Failed 终态、发错误信号的入口；不传则只记日志。
    // 返回线程指针仅供连线；所有权仍在本对象。
    QThread* StartWorker(QObject* worker, std::function<void()> body,
                         std::function<void()> request_stop = {},
                         std::function<void(QThread*)> on_finished = {},
                         std::function<void(QThread*, const std::string&)> on_error = {});

    // 请求停止某个线程: 置取消标志 -> quit -> 限时 wait。
    // 返回 true = 线程已结束（可以放心遗忘）；false = 超时仍在跑。
    // 注意: 返回 false **不代表句柄被丢弃** —— 线程已转入待回收列表，
    // 仍会在 StopAll()/析构时继续等待，句柄绝不外泄给"没人管"的状态。
    bool StopWorker(QThread* thread, int wait_ms);

    // 请求停止全部线程（含之前没停下来的）。timeout_ms 是**总预算**，
    // 不是每个线程各等一份：先统一发出停止请求让它们并行退出，再逐个收尾。
    // 返回 true = 全部已结束；false = 仍有线程在跑（会被继续持有）。
    bool StopAll(int timeout_ms);

    int ActiveCount() const;
    int RetiringCount() const;

    // 该线程是否仍归本对象持有且尚未结束（含"已请求停止但还卡在任务体里"的待回收线程）。
    //
    // 为什么需要它: 抽帧导出这类任务把产物写到**调用方指定的同一个目录**，文件名又只由
    // 帧序号决定。旧线程超时没退出来时若照常启动新任务，两个线程会写同名的 frame_*.jpg
    // 互相覆盖。调用方拿本方法确认旧线程真的结束了，再决定"直接启动"还是"排队等它"。
    // 注意判定是保守的: finished 信号经队列投递，实际结束到 RemoveEntry 生效之间会有
    // 极短暂的窗口仍返回 true —— 只会让调用方多排一次队，不会造成误判为"已结束"。
    bool IsActive(QThread* thread) const;

private:
    struct Entry {
        QThread* thread = nullptr;
        QObject* worker = nullptr;
        std::function<void()> request_stop;
        bool retiring = false;
    };

    Entry* FindEntry(QThread* thread);
    void RemoveEntry(QThread* thread);
    // 还剩多少等待预算（ms）；timeout_ms < 0 表示不限时，恒返回 -1。
    static int RemainingMs(std::chrono::steady_clock::time_point deadline, bool bounded);

    std::vector<Entry> entries_;
};

} // namespace qt
} // namespace videoeye
