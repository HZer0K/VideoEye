#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    // 返回 0 表示并发已满(调用方应把这次请求当作失败处理)。
    TaskId Begin(const std::string& slot, int wait_for_previous_ms = 0);

    // 便捷入口: 在受管 std::thread 上执行 body(id, token), 收尾自动写入终态。
    // body 里的异常会被捕获并记成 Failed(MSVC 的 std::thread 入口是 noexcept,
    // 异常逃逸会直接 terminate, 所以必须在线程内兜住)。
    TaskId Run(const std::string& slot, std::function<void(TaskId, CancelToken)> body, int wait_for_previous_ms = 0);

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
    bool WaitForIdle(const std::string& slot, int timeout_ms);
    // 等待所有 slot 空闲, 并回收所有受管线程。
    void WaitForAll(int timeout_ms);

private:
    struct Slot {
        std::atomic<TaskId> current_id{0};
        std::atomic<bool> running{false};
        std::atomic<TaskState> state{TaskState::Idle};
        std::shared_ptr<std::atomic<bool>> cancel{std::make_shared<std::atomic<bool>>(false)};
        std::thread thread; // 仅 Run() 使用
        std::atomic<bool> thread_needs_join{false};
    };

    Slot* FindOrCreateLocked(const std::string& slot);
    Slot* FindLocked(const std::string& slot) const;
    std::size_t RunningCountLocked() const;
    void JoinThreadLocked(Slot* s); // 仅回收已结束的线程, 不阻塞在仍运行的任务上

    const std::size_t max_concurrent_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::unique_ptr<Slot>> slots_;
    std::vector<std::thread> orphans_; // 被取代但仍未跑完的受管线程, 析构前统一回收
    TaskId next_id_ = 0;
};

} // namespace task
} // namespace videoeye
