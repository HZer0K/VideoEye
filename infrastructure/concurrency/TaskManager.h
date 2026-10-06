#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/domain/task/TaskProtocol.h"

namespace videoeye {
namespace task {

// 协议类型(TaskId / TaskState / TaskKind / CancelToken / TaskHandle)住在
// core/domain/task/TaskProtocol.h —— 分层上只允许 infrastructure 依赖 core/*,
// 若把协议留在本文件, 想接入这套约定的 core/qc、core/qt 就**看不见**它, 统一无从谈起。
// 这里只承载唯一的调度实现。

// 后台任务调度约定: 每个逻辑位置(slot)同时只允许一个任务在跑,
// 并统一提供「任务 ID / 取消标志 / 终态结果 / 过期结果丢弃」。
//
// 为什么不统一执行引擎: 抽帧与媒体导出的 worker 是 QObject(要发进度信号),
// 必须留在 QThread 上; 容器结构分析是纯计算, 用受管 std::thread 就够。
// 统一的是"生命周期契约"(Begin/End/Cancel/IsCurrent), 不是线程种类。
//
// 线程安全: Begin / Run* / End / Token / IsCurrent / CurrentId / IsRunning / State /
// RunningCount / Cancel* / WaitForIdle 都可以从**任意线程并发调用** —— 内部状态(含孤儿
// 线程表)统一由 core_->mutex 串行化, 不同 slot 之间不会互相干扰。
// 例外是关闭路径: WaitForAll() 与读它的 AbandonedCount() 之间没有同步, 按约定只在关闭
// 阶段由单一线程调用(典型现场是析构函数)。
//
// 用法(受管线程):
//     const auto id = tasks_.Run("container-structure", [&](TaskId id, CancelToken token) {
//         ... 长耗时计算 ...
//         if (token.IsCanceled() || !tasks_.IsCurrent("container-structure", id)) return;  // 结果作废
//         投递结果;
//     });
//
// 用法(自带 QThread 的 worker):
//     const auto id = tasks_.Begin("media-export", 30000);
//     ... 起 QThread ...; 结束信号里 tasks_.End("media-export", id, TaskState::Succeeded);
class TaskManager {
public:
    explicit TaskManager(std::size_t max_concurrent = 4);
    ~TaskManager();

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    // 在 slot 上登记一个新任务并接管它。若同 slot 上旧任务仍在跑:
    //   先置其取消标志, 再按 wait_for_previous_ms 限时等待其退出。
    //   超时也放行 —— 旧任务继续在后台跑完, 但 IsCurrent() 会判它过期, 结果不会覆盖新结果。
    // wait_for_previous_ms: 0=不等待, <0=等到它结束为止。
    // kind: 见 TaskKind 注释, 决定关闭时这个任务的线程能不能被放弃。
    // 返回 0 表示并发已满(调用方应把这次请求当作失败处理)。
    TaskId Begin(const std::string& slot, int wait_for_previous_ms = 0,
                 TaskKind kind = TaskKind::Cooperative);

    // 便捷入口: 在受管 std::thread 上执行 body(id, token), 收尾自动写入终态。
    // body 里的异常会被捕获并记成 Failed(MSVC 的 std::thread 入口是 noexcept,
    // 异常逃逸会直接 terminate, 所以必须在线程内兜住)。
    //
    // 注意: 正常返回一律按 Succeeded 记。任务体若自己知道"这趟没成", 请用下面的
    // RunWithResult 显式返回终态 —— 靠 UI 回调补写 End() 是不行的: 那条消息排队到
    // UI 线程时才落地, 届时 slot 可能已经记过一次终态(终态一次性, 后到的会被丢弃),
    // 也可能已经换了新任务。
    TaskId Run(const std::string& slot, std::function<void(TaskId, CancelToken)> body,
               int wait_for_previous_ms = 0, TaskKind kind = TaskKind::Cooperative);

    // Run() 的"任务体自己声明终态"版本: body 的返回值就是要写入 slot 的终态。
    // 返回值是过程态(Idle/Running)时按 Failed 记 —— 那是任务体违约, 不能让它把
    // slot 挂在 Running 上。异常同样记 Failed; 被取消时一律记 Canceled。
    TaskId RunWithResult(const std::string& slot, std::function<TaskState(TaskId, CancelToken)> body,
                         int wait_for_previous_ms = 0, TaskKind kind = TaskKind::Cooperative);

    // Run() 的 TaskKind::BlockingIo 版本。执行语义完全一样, 只有关闭策略不同。
    // 单独起个名字而不是塞个默认参数: 这是为了让调用点自己把"这里会阻塞在第三方 IO 上"
    // 这件事写出来, 顺带把上面那条生命周期约定钉在调用点旁边。
    TaskId RunBlockingIo(const std::string& slot, std::function<void(TaskId, CancelToken)> body,
                         int wait_for_previous_ms = 0);

    // RunWithResult() 的 TaskKind::BlockingIo 版本。
    TaskId RunBlockingIoWithResult(const std::string& slot,
                                   std::function<TaskState(TaskId, CancelToken)> body,
                                   int wait_for_previous_ms = 0);

    // 取该 slot 当前任务的取消令牌(供自带线程的任务体轮询)。
    CancelToken Token(const std::string& slot) const;

    // 与 Begin() 同一语义, 但把取消令牌一并返回 —— 调用方自带线程(QThread / 自建
    // std::thread)时用它就不必再走一次 Token(slot), 顺便也把"这是一次完整任务"这件事
    // 在调用点写清楚。handle 是这份任务在宿主里的唯一凭证, 任务体认的是它, 不是调用方
    // 自己另起的那套标志。
    //
    // handle 的所有字段(id / cancel / kind / slot)在**同一把锁内**一次生成, 所以
    // handle.cancel 一定属于 handle.id 那条任务。不存在"先 Begin() 拿 id、再
    // Token(slot) 拿令牌"那种两次取锁的写法 —— 那样写时, 两次取锁之间若同 slot 已被
    // 新任务接管, 拿到的取消令牌就是下一个任务的, 本任务从此不可取消。
    // Begin() 就是本函数的 id 版本(id == 0 即并发已满被拒, 此时其余字段也一律为空)。
    TaskHandle BeginHandle(const std::string& slot, int wait_for_previous_ms = 0,
                            TaskKind kind = TaskKind::Cooperative);

    // 任务体收尾时用手柄归还终态(与 End(slot, id, terminal) 同语义; id 对不上 slot 当前
    // 任务时静默丢弃 —— 也就是"这个任务已经被取代"或"句柄已经失效")。
    void EndHandle(const TaskHandle& handle, TaskState terminal);

    // 任务体收尾时上报终态。
    //
    // 写入是**一次性**的: 只允许 Running -> 终态。已经写过终态的 slot 再收到 End()
    // (Succeeded -> Failed、Canceled -> Succeeded 这类覆盖)一律丢弃 —— 终态一旦写入
    // 就不再变化, 否则"worker 正常返回记成功 + 排队到 UI 的失败消息后到"会互相改写。
    // id 与 slot 当前任务不匹配时同样忽略(说明已被新任务取代)。
    void End(const std::string& slot, TaskId id, TaskState terminal);

    // 结果是否仍然新鲜: 已被取消 / 已被同 slot 的新任务取代 -> false, 任务体应丢弃结果。
    bool IsCurrent(const std::string& slot, TaskId id) const;

    // slot 上当前任务的 id(没有则 0)。
    //
    // 与 IsCurrent 的区别: IsCurrent 额外要求"状态仍是 Running", 所以任务一收尾它就
    // 变 false —— 这让它没法用来判定"排队到 UI 线程的收尾消息是不是过期的"
    // (那种消息落地时 slot 必然已经终态)。那种场景请用这个 id 比对。
    TaskId CurrentId(const std::string& slot) const;

    bool IsRunning(const std::string& slot) const;
    TaskState State(const std::string& slot) const;
    std::size_t RunningCount() const;

    // 只置取消标志, 不等待; 要等退出请再调 WaitForIdle()。
    void Cancel(const std::string& slot);
    void CancelAll();

    // 等待 slot 上的任务进入终态; Run() 起的任务会顺带 join。
    // timeout_ms: <0=不限时。返回 true 表示已空闲, false 表示超时。
    // 注意: 本类只管"任务状态", 管不了调用方自管的线程 —— 那种线程卡住了就是卡住了。
    bool WaitForIdle(const std::string& slot, int timeout_ms);

    // 等待所有 slot 空闲并回收线程。timeout_ms >= 0 时它是 **整个函数** 的总预算,
    // 而不是"每个阶段各一份"或"每个 slot 各一份"。分三段, 语义不同, 别混为一谈:
    //
    //   1) 等终态          —— 有超时保证, 消耗的是 timeout_ms 预算;
    //      超时仍没报终态的调用方自管线程(QThread 等)被强制写成 Canceled,
    //      免得 slot 永远卡在 Running 让后续 Begin 每次白等一轮。
    //   2) 回收受管线程    —— 同样消耗 timeout_ms 的剩余预算。因为 std::thread 没有
    //      带超时的 join, 这里用一个 promise 判断"线程是否真的跑完", 只在判定为跑完时
    //      才 join(瞬间返回), 预算耗尽则按 kind 分类处置:
    //        * TaskKind::BlockingIo  -> detach 放弃。TaskManager 会先于线程析构,
    //          线程只持有 Core 的 shared_ptr, 所以它是自洽的, 不会踩到已销毁的对象。
    //        * TaskKind::Cooperative -> 仍然 join。契约上它必须响应取消, 走到这一步说明
    //          任务体坏了; 此时 detach 等于放任它访问可能已销毁的对象, 所以宁可多等。
    //   3) timeout_ms < 0  —— 不限时, 与旧行为一致(全部 join)。
    //
    // 所以这条函数的耗时保证是: **timeout_ms 有界, 除非存在违反契约的 Cooperative 任务。**
    // 这也是为什么可能阻塞在第三方 IO 上的任务必须声明成 BlockingIo —— 那是唯一肯承认
    // "它可能不响应取消"的种类。
    void WaitForAll(int timeout_ms);

    // 上一次 WaitForAll() 放弃掉的线程数(BlockingIo 任务超预算时才会 > 0)。
    // 正常业务代码不需要看它; 关闭耗时回归测试用它确认"确实放弃了而不是 join 到底"。
    std::size_t AbandonedCount() const;

private:
    // 受管线程可能比 TaskManager 活得久(被放弃时), 所以它依赖的东西必须装在一个
    // shared_ptr 里由线程自己保活 —— 这就是为什么 Slot / mutex / cv 不直接是本类成员。
    // Slot 与 Core 的定义都在 .cpp 里: 调用方只需要拿到不透明的对象, 不需要看见内部字段。
    struct Slot;
    struct Core;

    // 被取代但仍未跑完的受管线程, 下次 Begin / WaitForAll 时回收。
    struct OwnedThread {
        TaskKind kind = TaskKind::Cooperative;
        std::thread thread;
        std::future<void> done; // 线程真正跑完时置位
        TaskId id = 0;          // 该线程对应的任务 id (放弃后据此给 slot 写终态)
        std::string slot;       // 仅用于日志
    };

    static int RemainingMs(std::chrono::steady_clock::time_point deadline, bool bounded);

    // 做成静态成员而不是普通成员: worker 线程手上只有 Core, 没有 this, 而这组函数
    // 本来也只依赖传进来的 Core。
    static void EndTask(Core& st, const std::string& slot, TaskId id, TaskState terminal);
    static Slot* FindOrCreateLocked(Core& st, const std::string& slot);
    static Slot* FindLocked(Core& st, const std::string& slot);
    static std::size_t RunningCountLocked(Core& st);

    const std::size_t max_concurrent_;
    const std::shared_ptr<Core> core_;
    // 受 core_->mutex 保护 —— **包括扩容**。登记路径上"扩容"必须与"插入"在同一把锁内
    // 完成: 以前把 reserve() 挪到锁外(想把它提前到线程启动之前), 于是它和别的线程锁内
    // 的 push_back 并发访问同一个 vector, "受保护"就成了空话。
    std::vector<OwnedThread> orphans_;
    std::size_t abandoned_ = 0;
};

} // namespace task
} // namespace videoeye
