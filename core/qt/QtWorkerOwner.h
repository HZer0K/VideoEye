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
    // 返回线程指针仅供连线；所有权仍在本对象。
    QThread* StartWorker(QObject* worker, std::function<void()> body,
                         std::function<void()> request_stop = {});

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
