#include "infrastructure/concurrency/TaskManager.h"

#include <chrono>
#include <future>
#include <utility>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace task {

bool IsTerminalState(TaskState state) {
    return state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Canceled;
}

// ---------------------------------------------------------------------------
// 内部类型
// ---------------------------------------------------------------------------

struct TaskManager::Slot {
    std::atomic<TaskId> current_id{0};
    std::atomic<bool> running{false};
    std::atomic<TaskState> state{TaskState::Idle};
    std::shared_ptr<std::atomic<bool>> cancel{std::make_shared<std::atomic<bool>>(false)};
    TaskKind kind = TaskKind::Cooperative;
    std::thread thread;        // 仅 Run() 使用
    std::future<void> done;    // 该线程真正跑完时置位
};

// 受管线程可能比 TaskManager 活得久(BlockingIo 任务被放弃时), 所以凡是线程结束后还要
// 碰的东西都必须装在这里, 由线程自己持 shared_ptr 保活。
struct TaskManager::Core {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string, std::unique_ptr<Slot>> slots;
    TaskId next_id = 0;
};

namespace {

// worker 线程结束时给 promise 置位。必须是 RAII: body 之后的 EndTask() 万一抛异常,
// 少置这一次位会让 WaitForAll 把"已经跑完"误判成"还在跑", 白白多等一整个预算。
class SignalOnExit {
public:
    explicit SignalOnExit(std::shared_ptr<std::promise<void>> p) : p_(std::move(p)) {}
    SignalOnExit(SignalOnExit&& other) noexcept : p_(std::move(other.p_)) {}
    SignalOnExit(const SignalOnExit&) = delete;
    SignalOnExit& operator=(const SignalOnExit&) = delete;
    ~SignalOnExit() {
        if (p_)
            p_->set_value();
    }

private:
    std::shared_ptr<std::promise<void>> p_;
};

} // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

TaskManager::TaskManager(std::size_t max_concurrent)
    : max_concurrent_(max_concurrent > 0 ? max_concurrent : 1),
      core_(std::make_shared<Core>()) {
}

TaskManager::~TaskManager() {
    CancelAll();
    // 等终态限时 5s: 析构期没有 UI 可响应, 但不能因为某个任务拒不退出就把整个退出流程卡死。
    // 之后的受管线程回收同样受这 5s 总预算约束 —— BlockingIo 任务超预算会被放弃(detach),
    // 它的依赖都装在 Core 里由自己的 shared_ptr 保活, 所以 TaskManager 先析构是安全的。
    WaitForAll(5000);
}

// ---------------------------------------------------------------------------
// 内部工具 (全部要求持锁调用)
// ---------------------------------------------------------------------------

TaskManager::Slot* TaskManager::FindOrCreateLocked(Core& st, const std::string& slot) {
    auto it = st.slots.find(slot);
    if (it != st.slots.end())
        return it->second.get();
    auto created = std::make_unique<Slot>();
    Slot* raw = created.get();
    st.slots.emplace(slot, std::move(created));
    return raw;
}

TaskManager::Slot* TaskManager::FindLocked(Core& st, const std::string& slot) {
    auto it = st.slots.find(slot);
    return it == st.slots.end() ? nullptr : it->second.get();
}

std::size_t TaskManager::RunningCountLocked(Core& st) {
    std::size_t n = 0;
    for (const auto& kv : st.slots) {
        if (kv.second->running.load(std::memory_order_acquire))
            ++n;
    }
    return n;
}

void TaskManager::EndTask(Core& st, const std::string& slot, TaskId id, TaskState terminal) {
    if (!IsTerminalState(terminal))
        return; // End 只接受终态, 过程态由 Begin/Run 写入
    std::lock_guard<std::mutex> lk(st.mutex);
    Slot* s = FindLocked(st, slot);
    if (!s)
        return;
    if (s->current_id.load(std::memory_order_acquire) != id)
        return; // 已被新任务取代
    s->state.store(terminal, std::memory_order_release);
    s->running.store(false, std::memory_order_release);
    st.cv.notify_all();
}

// ---------------------------------------------------------------------------
// 登记 / 执行
// ---------------------------------------------------------------------------

TaskId TaskManager::Begin(const std::string& slot, int wait_for_previous_ms, TaskKind kind) {
    Slot* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(core_->mutex);
        s = FindOrCreateLocked(*core_, slot);
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
        std::lock_guard<std::mutex> lk(core_->mutex);
        s = FindOrCreateLocked(*core_, slot);

        // 本 slot 即将被新任务接管, 统计并发时先把它扣掉
        std::size_t running = RunningCountLocked(*core_);
        if (s->running.load(std::memory_order_acquire) && running > 0)
            --running;
        if (running >= max_concurrent_) {
            LOG_WARN("后台任务并发已满(" + std::to_string(max_concurrent_) + "), 拒绝新任务: " + slot);
            return 0;
        }

        // 处理上一个受管线程: 已结束就地回收, 还在跑则转入待回收列表(关闭时统一处置)
        if (s->thread.joinable()) {
            if (!s->running.load(std::memory_order_acquire)) {
                stale = std::move(s->thread);
            } else {
                orphans_.push_back(OwnedThread{s->kind, std::move(s->thread), std::move(s->done),
                                              s->current_id.load(std::memory_order_acquire), slot});
            }
            s->done = std::future<void>();
        }

        s->kind = kind;
        s->cancel = std::make_shared<std::atomic<bool>>(false);
        s->current_id.store(++core_->next_id, std::memory_order_release);
        s->state.store(TaskState::Running, std::memory_order_release);
        s->running.store(true, std::memory_order_release);
    }

    if (stale.joinable())
        stale.join();
    return s->current_id.load(std::memory_order_acquire);
}

TaskId TaskManager::Run(const std::string& slot, std::function<void(TaskId, CancelToken)> body,
                        int wait_for_previous_ms, TaskKind kind) {
    const TaskId id = Begin(slot, wait_for_previous_ms, kind);
    if (id == 0 || !body)
        return id;

    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lk(core_->mutex);
        Slot* s = FindLocked(*core_, slot);
        if (!s)
            return id;
        cancel = s->cancel;
    }

    // done_sig 要**同时**留在父作用域和线程里, 不能 move 进 lambda:
    // std::thread 构造失败时临时 lambda 会连同它独占的那份 promise 一起销毁,
    // 下面 catch 里的 set_value() 就会解引用已经被 move 空的 shared_ptr。
    auto done_sig = std::make_shared<std::promise<void>>();
    std::future<void> finished = done_sig->get_future();
    try {
        // worker **不能**捕获 this: BlockingIo 任务在关闭时可能被放弃(detach),
        // 那种情况下 TaskManager 会先于线程析构, 任何对 this 的访问都是悬空的。
        // Core 用 shared_ptr 按值捕获, 由线程自己保活到跑完为止。
        std::thread worker([st = core_, slot, id, kind,
                            body = std::move(body),
                            cancel = std::move(cancel),
                            done_sig]() mutable {
            SignalOnExit signal(done_sig);
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
            EndTask(*st, slot, id, terminal);
        });

        std::lock_guard<std::mutex> lk(core_->mutex);
        Slot* s = FindLocked(*core_, slot);
        if (s && s->current_id.load(std::memory_order_acquire) == id) {
            s->thread = std::move(worker);
            s->done = std::move(finished);
        } else {
            // 极端情况: 刚启动就被取代了 —— 线程已经跑起来了, 只能转待回收
            orphans_.push_back(OwnedThread{kind, std::move(worker), std::move(finished), id, slot});
        }
    } catch (const std::exception& e) {
        LOG_ERROR("后台任务无法启动 [" + slot + "]: " + std::string(e.what()));
        EndTask(*core_, slot, id, TaskState::Failed);
        // 线程没能起来, promise 还留在自己手上: 不置位就会让 future 一直挂着
        done_sig->set_value();
    }
    return id;
}

TaskId TaskManager::RunBlockingIo(const std::string& slot,
                                  std::function<void(TaskId, CancelToken)> body,
                                  int wait_for_previous_ms) {
    return Run(slot, std::move(body), wait_for_previous_ms, TaskKind::BlockingIo);
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

CancelToken TaskManager::Token(const std::string& slot) const {
    std::lock_guard<std::mutex> lk(core_->mutex);
    Slot* s = FindLocked(*core_, slot);
    return s ? CancelToken(s->cancel) : CancelToken();
}

void TaskManager::End(const std::string& slot, TaskId id, TaskState terminal) {
    EndTask(*core_, slot, id, terminal);
}

bool TaskManager::IsCurrent(const std::string& slot, TaskId id) const {
    std::lock_guard<std::mutex> lk(core_->mutex);
    const Slot* s = FindLocked(*core_, slot);
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
    std::lock_guard<std::mutex> lk(core_->mutex);
    const Slot* s = FindLocked(*core_, slot);
    return s && s->running.load(std::memory_order_acquire);
}

TaskState TaskManager::State(const std::string& slot) const {
    std::lock_guard<std::mutex> lk(core_->mutex);
    const Slot* s = FindLocked(*core_, slot);
    return s ? s->state.load(std::memory_order_acquire) : TaskState::Idle;
}

std::size_t TaskManager::RunningCount() const {
    std::lock_guard<std::mutex> lk(core_->mutex);
    return RunningCountLocked(*core_);
}

std::size_t TaskManager::AbandonedCount() const {
    return abandoned_;
}

// ---------------------------------------------------------------------------
// 取消 / 等待
// ---------------------------------------------------------------------------

void TaskManager::Cancel(const std::string& slot) {
    std::lock_guard<std::mutex> lk(core_->mutex);
    Slot* s = FindLocked(*core_, slot);
    if (s)
        s->cancel->store(true, std::memory_order_release);
}

void TaskManager::CancelAll() {
    std::lock_guard<std::mutex> lk(core_->mutex);
    for (auto& kv : core_->slots) {
        kv.second->cancel->store(true, std::memory_order_release);
    }
}

bool TaskManager::WaitForIdle(const std::string& slot, int timeout_ms) {
    std::unique_lock<std::mutex> lk(core_->mutex);
    Slot* s = FindLocked(*core_, slot);
    if (!s)
        return true;

    auto idle = [s]() { return !s->running.load(std::memory_order_acquire); };
    if (timeout_ms < 0) {
        core_->cv.wait(lk, idle);
    } else if (!core_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), idle)) {
        return false;
    }

    // 任务已结束 -> 就地回收线程(在锁外 join, 避免长时间持锁)
    std::thread stale;
    if (s->thread.joinable() && !s->running.load(std::memory_order_acquire)) {
        stale = std::move(s->thread);
        s->done = std::future<void>();
    }
    lk.unlock();
    if (stale.joinable())
        stale.join();
    return true;
}

int TaskManager::RemainingMs(std::chrono::steady_clock::time_point deadline, bool bounded) {
    if (!bounded) return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    return left > 0 ? static_cast<int>(left) : 0;
}

void TaskManager::WaitForAll(int timeout_ms) {
    // timeout_ms 是**总预算**: 既不是每个 slot 各等一份, 也不是每个阶段各等一份,
    // 而是从进入到返回的全部时间。N 个 slot 最坏也只等 timeout_ms。
    const bool bounded = timeout_ms >= 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(bounded ? timeout_ms : 0);

    // --- 阶段一: 等终态 ---
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(core_->mutex);
        names.reserve(core_->slots.size());
        for (const auto& kv : core_->slots)
            names.push_back(kv.first);
    }
    for (const auto& name : names) {
        WaitForIdle(name, RemainingMs(deadline, bounded));
        if (bounded && RemainingMs(deadline, bounded) <= 0)
            break;   // 预算用完: 剩下的 slot 交给下面的强制收尾
    }

    // --- 阶段二: 回收受管线程 ---
    std::vector<OwnedThread> pending;
    bool forced = false;
    {
        std::lock_guard<std::mutex> lk(core_->mutex);
        pending.swap(orphans_);
        for (auto& kv : core_->slots) {
            Slot* s = kv.second.get();
            if (s->thread.joinable()) {
                pending.push_back(OwnedThread{s->kind, std::move(s->thread), std::move(s->done),
                                              s->current_id.load(std::memory_order_acquire), kv.first});
                s->done = std::future<void>();
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
        core_->cv.notify_all();

    abandoned_ = 0;
    std::vector<std::thread> leaked;
    std::vector<OwnedThread> gave_up;   // 被放弃的那些, 稍后给它们的 slot 补写终态
    for (auto& p : pending) {
        const int wait_ms = RemainingMs(deadline, bounded);   // bounded=false 时恒为 -1
        bool finished = true;
        if (wait_ms < 0) {
            finished = true;                       // 不限时: 直接 join
        } else if (p.done.valid()) {
            // wait_ms >= 0 时先用 promise 问一句"跑完了吗"(std::thread 没有带超时的 join):
            // 回答"跑完了"才 join(瞬间返回), 否则一分钱时间都不花在 join 上。
            // 预算哪怕已经归零也要用 0ms 轮询一次 —— 让恰好刚跑完的任务正常收尾,
            // 而不是被误判成"不响应取消"再打一条 ERROR 出来。
            try {
                finished = p.done.wait_for(std::chrono::milliseconds(wait_ms)) ==
                           std::future_status::ready;
            } catch (...) {
                // promise 处于异常状态(线程压根没起来): 按已结束处理, 下面 join 会跳过
                finished = true;
            }
        } else {
            finished = false;                      // 连 future 都没有, 只能按没跑完处理
        }

        if (finished) {
            if (p.thread.joinable())
                p.thread.join();
            continue;
        }

        if (p.kind == TaskKind::BlockingIo) {
            // 明说过可能卡在第三方 IO 上的任务: 放弃它。线程只持有 Core 的 shared_ptr,
            // 自己能跑到结束, 也不会再去碰 TaskManager(它马上就要析构了)。
            LOG_WARN("关闭超时, 放弃仍阻塞在 IO 上的受管线程 [" + p.slot + "]");
            leaked.push_back(std::move(p.thread));
            gave_up.push_back({TaskKind::BlockingIo, std::thread(), std::future<void>(), p.id, p.slot});
            ++abandoned_;
            continue;
        }

        // Cooperative 任务走到这一步说明任务体没轮询取消令牌 —— 这是调用方的 bug。
        // 仍然 join: detach 会放任它继续访问可能已经销毁的外部对象, 风险比多等更大。
        LOG_ERROR("协作式后台任务未在关闭预算内退出 [" + p.slot + "], 只能继续等待 (任务体应轮询取消令牌)");
        if (p.thread.joinable())
            p.thread.join();
    }

    // detach 放在最后: 先放掉所有能正常回收的, 免得中间抛异常把这批线程留在 pending 里析构
    for (auto& t : leaked) {
        if (t.joinable())
            t.detach();
    }

    // 被放弃的任务自己不会再按时回来写终态了。必须在这里替它补写 ——
    // 否则 slot 永远停在 Running, 之后每次 Begin 都要白等一整轮关闭预算。
    if (gave_up.empty())
        return;
    {
        std::lock_guard<std::mutex> lk(core_->mutex);
        for (const auto& g : gave_up) {
            Slot* s = FindLocked(*core_, g.slot);
            if (!s || s->current_id.load(std::memory_order_acquire) != g.id)
                continue;   // 期间被新任务接管了, 那是新任务的事
            s->state.store(TaskState::Canceled, std::memory_order_release);
            s->running.store(false, std::memory_order_release);
        }
    }
    core_->cv.notify_all();
}

} // namespace task
} // namespace videoeye
