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

namespace videoeye {
namespace task {

using TaskId = std::uint64_t;

// 任务终态。Idle/Running 是过程态, 其余三种是一旦写入就不再变化的终态。
enum class TaskState {
    Idle,      // 该 slot 上当前没有任务
    Running,   // 运行中
    Succeeded, // 终态: 成功
    Failed,    // 终态: 出错(含异常逃逸)
    Canceled,  // 终态: 被取消 / 被同 slot 的新任务取代
};

bool IsTerminalState(TaskState state);

// 关闭阶段能不能"放弃"这个任务。这个区分是 WaitForAll() 能否给出**整个函数**的耗时
// 上限的前提 —— 详见 WaitForAll 的注释。默认 Cooperative, 即沿用"必须 join"的保守策略。
enum class TaskKind {
    // 协作式可取消任务: 任务体会轮询取消令牌, 且不会长时间停在第三方阻塞调用里。
    // 关闭时 TaskManager **一定会** join 它 —— join 是唯一能证明它已经不再访问外部对象的
    // 手段, 所以契约被破坏时宁可多等, 也不冒悬空访问的风险。
    Cooperative,

    // 可能卡在第三方阻塞调用上的任务: FFmpeg 的网络 IO、异常设备读取等。这类调用即使
    // 装了 AVIOInterruptCB 也不保证一定响应 —— 比如回调只在下一个网络包到来时才被检查。
    // 关闭时在预算内等不到它就 **放弃** 这个线程(detach), 不再拖住整个关闭流程。
    //
    // 代价是一条更严格的生命周期约定, 调用得遵守, 否则 detach 之后就是悬空访问:
    //   * 任务体只能按值 / shared_ptr 捕获依赖, 不得持有任何可能在它结束前销毁的裸引用;
    //   * 不得依赖"任务一定会被 join"这件事做任何清理;
    //   * 结果发布必须自己判过期(TaskManager::IsCurrent / QPointer 之类的存活判定)。
    BlockingIo,
};

// 取消令牌: 由任务体在循环里轮询。
//
// 令牌内部持有一份 shared_ptr 的取消标志, 因此即使 TaskManager 先于任务线程析构,
// 任务体调用 IsCanceled() 仍然是安全的(析构前会把所有标志置位)。
class CancelToken {
public:
    CancelToken() = default;
    explicit CancelToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {
    }

    bool IsCanceled() const {
        return flag_ && flag_->load(std::memory_order_acquire);
    }
    explicit operator bool() const {
        return static_cast<bool>(flag_);
    }

    // 暴露底层取消标志（shared_ptr 保活）。供调用方把指针交给 FFmpeg 的
    // AVIOInterruptCB.opaque，使 avformat_open_input 等阻塞 IO 在 CancelAll()
    // 之后能及时中断（否则关闭流程会在 join 受管线程时挂死）。无令牌时返回空。
    std::shared_ptr<std::atomic<bool>> flag() const {
        return flag_;
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// 后台任务调度约定: 每个逻辑位置(slot)同时只允许一个任务在跑,
// 并统一提供「任务 ID / 取消标志 / 终态结果 / 过期结果丢弃」。
//
// 为什么不统一执行引擎: 抽帧与媒体导出的 worker 是 QObject(要发进度信号),
// 必须留在 QThread 上; 容器结构分析是纯计算, 用受管 std::thread 就够。
// 统一的是"生命周期契约"(Begin/End/Cancel/IsCurrent), 不是线程种类。
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
    TaskId Run(const std::string& slot, std::function<void(TaskId, CancelToken)> body,
               int wait_for_previous_ms = 0, TaskKind kind = TaskKind::Cooperative);

    // Run() 的 TaskKind::BlockingIo 版本。执行语义完全一样, 只有关闭策略不同。
    // 单独起个名字而不是塞个默认参数: 这是为了让调用点自己把"这里会阻塞在第三方 IO 上"
    // 这件事写出来, 顺带把上面那条生命周期约定钉在调用点旁边。
    TaskId RunBlockingIo(const std::string& slot, std::function<void(TaskId, CancelToken)> body,
                         int wait_for_previous_ms = 0);

    // 取该 slot 当前任务的取消令牌(供自带线程的任务体轮询)。
    CancelToken Token(const std::string& slot) const;

    // 任务体收尾时上报终态。id 与 slot 当前任务不匹配时忽略(说明已被新任务取代)。
    void End(const std::string& slot, TaskId id, TaskState terminal);

    // 结果是否仍然新鲜: 已被取消 / 已被同 slot 的新任务取代 -> false, 任务体应丢弃结果。
    bool IsCurrent(const std::string& slot, TaskId id) const;

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
    std::vector<OwnedThread> orphans_; // 受 mutex_ 保护
    std::size_t abandoned_ = 0;
};

} // namespace task
} // namespace videoeye
