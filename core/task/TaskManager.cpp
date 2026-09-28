#include "core/task/TaskManager.h"

#include <chrono>
#include <utility>

#include "utils/Logger.h"

namespace videoeye {
namespace task {

bool IsTerminalState(TaskState state) {
    return state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Canceled;
}

TaskManager::TaskManager(std::size_t max_concurrent) : max_concurrent_(max_concurrent > 0 ? max_concurrent : 1) {
}

TaskManager::~TaskManager() {
    CancelAll();
    // 限时 5s: 析构期没有 UI 可响应, 但不能因为某个任务拒不退出就把整个退出流程卡死。
    // 受管线程(Run)无论如何都会被 join —— 它的 body 一定走到 End, 不会真的永远挂着。
    WaitForAll(5000);
}

TaskManager::Slot* TaskManager::FindOrCreateLocked(const std::string& slot) {
    auto it = slots_.find(slot);
    if (it != slots_.end())
        return it->second.get();
    auto created = std::make_unique<Slot>();
    Slot* raw = created.get();
    slots_.emplace(slot, std::move(created));
    return raw;
}

TaskManager::Slot* TaskManager::FindLocked(const std::string& slot) const {
    auto it = slots_.find(slot);
    return it == slots_.end() ? nullptr : it->second.get();
}

std::size_t TaskManager::RunningCountLocked() const {
    std::size_t n = 0;
    for (const auto& kv : slots_) {
        if (kv.second->running.load(std::memory_order_acquire))
            ++n;
    }
    return n;
}

TaskId TaskManager::Begin(const std::string& slot, int wait_for_previous_ms) {
    Slot* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        s = FindOrCreateLocked(slot);
        if (s->running.load(std::memory_order_acquire)) {
            // 请求旧任务退出。它是否真的会停下来取决于任务体有没有轮询令牌,
            // 但无论停不停, 它的结果都会被 IsCurrent() 判为过期而丢弃。
            s->cancel->store(true, std::memory_order_release);
        }
    }

    if (wait_for_previous_ms != 0)
        WaitForIdle(slot, wait_for_previous_ms);

    std::thread stale;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        s = FindOrCreateLocked(slot);

        // 本 slot 即将被新任务接管, 统计并发时先把它扣掉
        std::size_t running = RunningCountLocked();
        if (s->running.load(std::memory_order_acquire) && running > 0)
            --running;
        if (running >= max_concurrent_) {
            LOG_WARN("后台任务并发已满(" + std::to_string(max_concurrent_) + "), 拒绝新任务: " + slot);
            return 0;
        }

        // 处理上一个受管线程: 已结束就地回收, 还在跑则转入待回收列表(析构前统一 join)
        if (s->thread.joinable()) {
            if (!s->running.load(std::memory_order_acquire)) {
                stale = std::move(s->thread);
            } else {
                orphans_.push_back(std::move(s->thread));
            }
            s->thread_needs_join.store(false, std::memory_order_release);
        }

        s->cancel = std::make_shared<std::atomic<bool>>(false);
        s->current_id.store(++next_id_, std::memory_order_release);
        s->state.store(TaskState::Running, std::memory_order_release);
        s->running.store(true, std::memory_order_release);
    }

    if (stale.joinable())
        stale.join();
    return s->current_id.load(std::memory_order_acquire);
}

TaskId TaskManager::Run(const std::string& slot, std::function<void(TaskId, CancelToken)> body,
                        int wait_for_previous_ms) {
    const TaskId id = Begin(slot, wait_for_previous_ms);
    if (id == 0 || !body)
        return id;

    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        Slot* s = FindLocked(slot);
        if (!s)
            return id;
        cancel = s->cancel;
    }

    try {
        std::thread worker([this, slot, id, body = std::move(body), cancel = std::move(cancel)]() {
            TaskState terminal = TaskState::Succeeded;
            try {
                body(id, CancelToken(cancel));
            } catch (const std::exception& e) {
                LOG_ERROR("后台任务异常终止 [" + slot + "]: " + std::string(e.what()));
                terminal = TaskState::Failed;
            } catch (...) {
                LOG_ERROR("后台任务异常终止 [" + slot + "]: 未知异常");
                terminal = TaskState::Failed;
            }
            // 已被取消 / 被新任务取代时, 终态按 Canceled 记, 避免把"被丢弃"报成成功
            if (terminal == TaskState::Succeeded && cancel && cancel->load(std::memory_order_acquire)) {
                terminal = TaskState::Canceled;
            }
            End(slot, id, terminal);
        });

        std::lock_guard<std::mutex> lk(mutex_);
        Slot* s = FindLocked(slot);
        if (s && s->current_id.load(std::memory_order_acquire) == id) {
            s->thread = std::move(worker);
            s->thread_needs_join.store(true, std::memory_order_release);
        } else {
            // 极端情况: 刚启动就被取代了 —— 线程已经跑起来了, 只能转待回收
            orphans_.push_back(std::move(worker));
        }
    } catch (const std::exception& e) {
        LOG_ERROR("后台任务无法启动 [" + slot + "]: " + std::string(e.what()));
        End(slot, id, TaskState::Failed);
    }
    return id;
}

CancelToken TaskManager::Token(const std::string& slot) const {
    std::lock_guard<std::mutex> lk(mutex_);
    Slot* s = FindLocked(slot);
    return s ? CancelToken(s->cancel) : CancelToken();
}

void TaskManager::End(const std::string& slot, TaskId id, TaskState terminal) {
    if (!IsTerminalState(terminal))
        return; // End 只接受终态, 过程态由 Begin/Run 写入
    std::lock_guard<std::mutex> lk(mutex_);
    Slot* s = FindLocked(slot);
    if (!s)
        return;
    if (s->current_id.load(std::memory_order_acquire) != id)
        return; // 已被新任务取代
    s->state.store(terminal, std::memory_order_release);
    s->running.store(false, std::memory_order_release);
    cv_.notify_all();
}

bool TaskManager::IsCurrent(const std::string& slot, TaskId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const Slot* s = FindLocked(slot);
    if (!s)
        return false;
    if (s->current_id.load(std::memory_order_acquire) != id)
        return false;
    if (!s->running.load(std::memory_order_acquire))
        return false;
    if (s->state.load(std::memory_order_acquire) != TaskState::Running)
        return false;
    return !s->cancel->load(std::memory_order_acquire);
}

bool TaskManager::IsRunning(const std::string& slot) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const Slot* s = FindLocked(slot);
    return s && s->running.load(std::memory_order_acquire);
}

TaskState TaskManager::State(const std::string& slot) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const Slot* s = FindLocked(slot);
    return s ? s->state.load(std::memory_order_acquire) : TaskState::Idle;
}

std::size_t TaskManager::RunningCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return RunningCountLocked();
}

void TaskManager::Cancel(const std::string& slot) {
    std::lock_guard<std::mutex> lk(mutex_);
    Slot* s = FindLocked(slot);
    if (s)
        s->cancel->store(true, std::memory_order_release);
}

void TaskManager::CancelAll() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& kv : slots_) {
        kv.second->cancel->store(true, std::memory_order_release);
    }
}

bool TaskManager::WaitForIdle(const std::string& slot, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);
    Slot* s = FindLocked(slot);
    if (!s)
        return true;

    auto idle = [s]() { return !s->running.load(std::memory_order_acquire); };
    if (timeout_ms < 0) {
        cv_.wait(lk, idle);
    } else if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), idle)) {
        return false;
    }

    // 任务已结束 -> 就地回收线程(在锁外 join, 避免长时间持锁)
    std::thread stale;
    if (s->thread.joinable() && !s->running.load(std::memory_order_acquire)) {
        s->thread_needs_join.store(false, std::memory_order_release);
        stale = std::move(s->thread);
    }
    lk.unlock();
    if (stale.joinable())
        stale.join();
    return true;
}

void TaskManager::WaitForAll(int timeout_ms) {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        names.reserve(slots_.size());
        for (const auto& kv : slots_)
            names.push_back(kv.first);
    }
    for (const auto& name : names)
        WaitForIdle(name, timeout_ms);

    std::vector<std::thread> pending;
    bool forced = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending.swap(orphans_);
        for (auto& kv : slots_) {
            Slot* s = kv.second.get();
            if (s->thread.joinable()) {
                // 受管线程: 必须 join, 不能 detach(进程退出时还在跑会崩)
                pending.push_back(std::move(s->thread));
                s->thread_needs_join.store(false, std::memory_order_release);
            } else if (s->running.load(std::memory_order_acquire)) {
                // 调用方自管线程(QThread 等)超时没报终态: 强制收尾。
                // 否则 slot 永远卡在 Running, 之后 Begin 每次都要白等一轮超时。
                s->state.store(TaskState::Canceled, std::memory_order_release);
                s->running.store(false, std::memory_order_release);
                forced = true;
            }
        }
    }
    if (forced)
        cv_.notify_all();
    for (auto& t : pending) {
        if (t.joinable())
            t.join();
    }
}

} // namespace task
} // namespace videoeye
