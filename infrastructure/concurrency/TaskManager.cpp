#include "infrastructure/concurrency/TaskManager.h"

#include <chrono>
#include <future>
#include <utility>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace task {

// IsTerminalState 是协议的一部分, 在 core/domain/task/TaskProtocol.h 里定义成 inline。

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
    // 被顶下来的孤儿线程数(与 TaskManager::orphans_ 同步递增)。并发预算必须把它算进去:
    // 孤儿不在任何 slot 里, 只数 slot 的话"旧任务超时 + 新任务立刻再来"就能一路超发线程。
    std::size_t orphan_count = 0;
};

namespace {

// 给"线程已跑完"这个 promise 置位, 幂等。
//
// 为什么必须幂等: 同一个 promise 置两次会抛 promise_already_satisfied, 而 set_value()
// 的两个调用点(线程内的 SignalOnExit、父作用域 catch 里的收尾)在"线程已启动且已跑到
// 收尾 + 父作用域登记失败"这条路径上会撞车。这个函数的调用点包括析构函数, 在那里抛
// 异常等于 std::terminate()。
void SetDoneOnce(std::shared_ptr<std::promise<void>>& p) {
    if (!p)
        return;
    try {
        p->set_value();
    } catch (...) {
        // 已经置过值了: 这正是本次调用想达到的状态, 不是错误。
    }
}

// worker 线程结束时给 promise 置位。必须是 RAII: body 之后的 EndTask() 万一抛异常,
// 少置这一次位会让 WaitForAll 把"已经跑完"误判成"还在跑", 白白多等一整个预算。
class SignalOnExit {
public:
    explicit SignalOnExit(std::shared_ptr<std::promise<void>> p) : p_(std::move(p)) {}
    SignalOnExit(SignalOnExit&& other) noexcept : p_(std::move(other.p_)) {}
    SignalOnExit(const SignalOnExit&) = delete;
    SignalOnExit& operator=(const SignalOnExit&) = delete;
    ~SignalOnExit() { SetDoneOnce(p_); }

private:
    std::shared_ptr<std::promise<void>> p_;
};

// 线程已经跑起来、但还没交到 slot / orphans_ 手里时的守卫。
//
// 为什么需要它: std::thread 带着 joinable 状态析构会直接 std::terminate()。登记阶段
// (加锁、查 slot、push_back 到 orphans_)每一步都可能因内存分配失败抛异常, 异常一出,
// 局部 std::thread 就会在进 catch 之前析构 —— 程序当场终止, 连日志都来不及打。
//
// 处置方式只能是 detach 而不是 join: 这里处在调用方线程(通常是 UI 线程), 而任务体
// 可能正卡在第三方 IO 上, join 会把调用方一起拖死。detach 是安全的 —— worker 只按值
// 捕获 Core 的 shared_ptr 与任务体, 脱离后仍能自己跑到结束, 不碰 TaskManager。
class ThreadGuard {
public:
    explicit ThreadGuard(std::thread t) : t_(std::move(t)) {}
    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;
    ~ThreadGuard() {
        if (t_.joinable())
            t_.detach();
    }
    // 登记成功: 交出所有权, 守卫不再处置它。
    std::thread Release() { return std::move(t_); }

private:
    std::thread t_;
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
    // orphan 是被新任务顶下来、还在后台跑完的旧线程: 它不在任何 slot 里, 不数进来的话
    // Begin 的并发判定只看得见当前 slot, 于是"旧任务超时 + 新任务立刻再来"就能一路超发。
    n += st.orphan_count;
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
    // 终态一次性: 只允许 Running -> 终态。已经写过终态的 slot 再收 End() 一律丢弃。
    //
    // 没有这层判断时, "任务体正常返回 -> Run 记 Succeeded" 与 "排到 UI 线程的失败
    // 消息后到 -> 回调里再记 Failed" 会互相覆盖, 终态取决于两条路径谁后跑完;
    // 更糟的是旧任务的失败消息可能在新任务开始之后才落地。终态一旦写入就不再变化,
    // 后到的那次必须被丢弃。
    if (IsTerminalState(s->state.load(std::memory_order_acquire)))
        return;
    s->state.store(terminal, std::memory_order_release);
    s->running.store(false, std::memory_order_release);
    st.cv.notify_all();
}

// ---------------------------------------------------------------------------
// 登记 / 执行
// ---------------------------------------------------------------------------

TaskId TaskManager::Begin(const std::string& slot, int wait_for_previous_ms, TaskKind kind) {
    // Begin 只要身份; 完整句柄(id + 取消令牌 + kind + slot)统一由 BeginHandle 在
    // **同一把锁内**一次生成。以前 Begin 与 BeginHandle 是两条各自取锁的路径,
    // "先 Begin 再 Token(slot)"之间那两次取锁就是缺陷所在(见 BeginHandle 注释)。
    return BeginHandle(slot, wait_for_previous_ms, kind).id;
}

TaskId TaskManager::Run(const std::string& slot, std::function<void(TaskId, CancelToken)> body,
                        int wait_for_previous_ms, TaskKind kind) {
    if (!body) {
        // 没有任务体却有 id: 直接登记再立刻记失败, 别把 slot 挂在 Running 上
        const TaskId id = Begin(slot, wait_for_previous_ms, kind);
        if (id != 0) EndTask(*core_, slot, id, TaskState::Failed);
        return id;
    }
    // 转交给带返回值的版本: 那边才是真正的实现, 免得两份线程启动逻辑各错各的。
    return RunWithResult(slot,
                         [body = std::move(body)](TaskId id, CancelToken token) {
                             body(id, token);
                             return TaskState::Succeeded;
                         },
                         wait_for_previous_ms, kind);
}

TaskId TaskManager::RunWithResult(const std::string& slot,
                                  std::function<TaskState(TaskId, CancelToken)> body,
                                  int wait_for_previous_ms, TaskKind kind) {
    // 一次锁内拿到完整句柄: id 与取消令牌必然属于同一个任务。
    const TaskHandle handle = BeginHandle(slot, wait_for_previous_ms, kind);
    const TaskId id = handle.id;
    if (id == 0 || !body) {
        if (id != 0) EndTask(*core_, slot, id, TaskState::Failed);
        return id;
    }

    // 取消令牌直接取句柄里的那一份, 不再"Begin 之后重新回读 slot 上的 cancel":
    // 两次取锁之间若同 slot 已被新任务接管, 回读到的就是**别人的**令牌 ——
    // 本任务的取消信号永远落不到自己身上, worker 却以为自己能被取消。
    const std::shared_ptr<std::atomic<bool>> cancel = handle.cancel.flag();

    // done_sig 要**同时**留在父作用域和线程里, 不能 move 进 lambda:
    // std::thread 构造失败时临时 lambda 会连同它独占的那份 promise 一起销毁,
    // 下面 catch 里的 set_value() 就会解引用已经被 move 空的 shared_ptr。
    auto done_sig = std::make_shared<std::promise<void>>();
    std::future<void> finished = done_sig->get_future();
    // 线程一旦起来, 后面任何一步抛异常都会让局部 std::thread 带着 joinable 状态析构,
    // 那等于直接 std::terminate()。登记阶段由此一律按"两阶段"写: 可能抛异常的动作
    // (容器扩容 / 字符串拷贝)全部在线程还握在 ThreadGuard 手里时做完, 最后一步才是
    // noexcept 的线程所有权转移。
    //
    // 以前这里改成"起线程之前先 reserve 一次 orphans_", 把最可能发生的那次分配提前到
    // 线程启动之前。那条路已经堵死了: reserve 在取 core_->mutex **之前**执行, 而
    // orphans_ 声称由这把锁保护 —— 它和别的线程锁内的 push_back 会并发访问同一个
    // vector。两阶段登记做完之后, 这次预留也不再有任何作用。
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
                terminal = body(id, CancelToken(cancel));
            } catch (const std::exception& e) {
                LOG_ERROR("后台任务异常终止 [" + slot + "]: " + std::string(e.what()));
                terminal = TaskState::Failed;
            } catch (...) {
                LOG_ERROR("后台任务异常终止 [" + slot + "]: 未知异常");
                terminal = TaskState::Failed;
            }
            // 任务体返回过程态属于违约(等于没给结论): 按失败记, 不能让 slot 挂在 Running 上。
            if (!IsTerminalState(terminal)) {
                LOG_ERROR("后台任务返回了非终态 [" + slot + "], 按失败处理");
                terminal = TaskState::Failed;
            }
            // 已被取消 / 被新任务取代时, 终态按 Canceled 记, 避免把"被丢弃"报成成功
            if (terminal == TaskState::Succeeded && cancel && cancel->load(std::memory_order_acquire)) {
                terminal = TaskState::Canceled;
            }
            EndTask(*st, slot, id, terminal);
        });
        // 从这一行起线程已经在跑了: 交给守卫, 保证任何异常路径都不会让它 joinable 着析构。
        ThreadGuard guard(std::move(worker));

        std::lock_guard<std::mutex> lk(core_->mutex);
        Slot* s = FindLocked(*core_, slot);
        if (s && s->current_id.load(std::memory_order_acquire) == id) {
            s->thread = guard.Release();
            s->done = std::move(finished);
        } else {
            // 极端情况: 刚启动就被取代了 —— 线程已经跑起来了, 只能转待回收。
            //
            // 两阶段登记: 先把**不含线程**的 OwnedThread 整个准备好并插进容器, 最后才把
            // 线程无异常地移动进去。顺序反过来的话(把 guard.Release() 写进构造实参),
            // push_back 扩容失败、或 slot 字符串拷贝失败, 都会让那个临时对象带着
            // joinable 的线程析构 —— std::thread 的析构直接 std::terminate(), 连日志都
            // 来不及打。现在前两步仍可能抛, 但那时线程还在 guard 手里(异常时 detach),
            // 容器与 orphan_count 也都还没被改动。
            OwnedThread orphan;                            // 不含线程
            orphan.kind = kind;
            orphan.id = id;
            orphan.slot = slot;                            // 可能抛(bad_alloc)
            orphan.done = std::move(finished);
            orphans_.push_back(std::move(orphan));          // 可能抛(扩容); 抛时元素未被改动
            orphans_.back().thread = guard.Release();       // noexcept: 交出所有权
            ++core_->orphan_count;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("后台任务无法启动 [" + slot + "]: " + std::string(e.what()));
        EndTask(*core_, slot, id, TaskState::Failed);
        // 线程已经跑起来时 promise 由线程内的 SignalOnExit 置位, 没起来时还留在自己手上
        // (不置位就会让 WaitForAll 里的 future 一直挂着)。两种情况都交给幂等版本处理。
        SetDoneOnce(done_sig);
    }
    return id;
}

TaskId TaskManager::RunBlockingIo(const std::string& slot,
                                  std::function<void(TaskId, CancelToken)> body,
                                  int wait_for_previous_ms) {
    return Run(slot, std::move(body), wait_for_previous_ms, TaskKind::BlockingIo);
}

TaskId TaskManager::RunBlockingIoWithResult(const std::string& slot,
                                            std::function<TaskState(TaskId, CancelToken)> body,
                                            int wait_for_previous_ms) {
    return RunWithResult(slot, std::move(body), wait_for_previous_ms, TaskKind::BlockingIo);
}

TaskHandle TaskManager::BeginHandle(const std::string& slot, int wait_for_previous_ms,
                                    TaskKind kind) {
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

    TaskHandle handle;
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
            // 空句柄: id=0 / 取消令牌为空 / slot 为空。id 是唯一的失败信号,
            // 但"空"必须在每个字段上都成立 —— 否则调用方可能拿半份句柄去 End。
            return TaskHandle{};
        }

        // 处理上一个受管线程: 已结束就地回收, 还在跑则转入待回收列表(关闭时统一处置)
        if (s->thread.joinable()) {
            if (!s->running.load(std::memory_order_acquire)) {
                stale = std::move(s->thread);
            } else {
                // 两阶段登记(与 RunWithResult 里那段同理, 详细理由见那里): 可能抛异常的
                // 部分(字符串拷贝 / 容器扩容)全部做完, 最后一步才把线程**无异常地**搬进来。
                // 这里比那边还要紧一点 —— 一旦在 s->thread 已被搬空之后抛异常, 这条线程
                // 就既不在 slot 上也不在孤儿表里, 临时对象析构时直接 std::terminate()。
                OwnedThread orphan;                            // 不含线程
                orphan.kind = s->kind;
                orphan.id = s->current_id.load(std::memory_order_acquire);
                orphan.slot = slot;                            // 可能抛(bad_alloc)
                orphan.done = std::move(s->done);
                orphans_.push_back(std::move(orphan));          // 可能抛(扩容)
                orphans_.back().thread = std::move(s->thread);  // noexcept: 交出所有权
                ++core_->orphan_count;
            }
            s->done = std::future<void>();
        }

        // 被取代即被取消（锁内收口）：上面第一把锁发出的取消，针对的是那一刻的 occupant；
        // 两个取锁之间它完全可能被另一个 BeginHandle 换掉 —— 于是"取代 A 的其实是更晚的 B,
        // 而 B 的第一把锁没看见 A"。此刻正被本句柄顶掉的任务，其令牌马上就会被覆盖，
        // 取消必须在这里补发：否则它永远等不到取消（旧任务从此不可取消，正是评审描述的
        // "句柄拿错令牌"终态之一）。测试见 ConcurrentBeginHandleKeepsIdAndCancelPaired。
        if (s->running.load(std::memory_order_acquire))
            s->cancel->store(true, std::memory_order_release);

        // 句柄的全部字段必须在**写入 slot 之前**构造完成: 字符串拷贝与 make_shared 都可能
        // 抛, 而 s->current_id / s->cancel 一旦写上, 这个任务就已经"存在于 slot 上"了 ——
        // 此时若抛异常, 调用方拿不到句柄, 却留下一个没有归属者的 Running 任务。
        handle.slot = slot;                                                       // 可能抛
        handle.kind = kind;
        handle.cancel = CancelToken(std::make_shared<std::atomic<bool>>(false));  // 可能抛
        handle.id = ++core_->next_id;   // 锁内取值: 解锁后别人会推进 current_id

        s->kind = kind;
        s->cancel = handle.cancel.flag();
        s->current_id.store(handle.id, std::memory_order_release);
        s->state.store(TaskState::Running, std::memory_order_release);
        s->running.store(true, std::memory_order_release);
    }

    if (stale.joinable())
        stale.join();

    // 这份句柄在**同一把锁内**完成"分配 id / 创建令牌 / 写入 slot", 所以 id 与 cancel
    // 必然同属一个任务 —— 以前"Begin() 拿 id 后再 Token(slot) 拿令牌"分两次取锁,
    // 中间被同 slot 的新任务接管时, 句柄里的取消令牌就变成下一个任务的(旧任务从此不可取消)。
    return handle;
}

void TaskManager::EndHandle(const TaskHandle& handle, TaskState terminal) {
    if (handle.id == 0 || handle.slot.empty())
        return;
    End(handle.slot, handle.id, terminal);
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

TaskId TaskManager::CurrentId(const std::string& slot) const {
    std::lock_guard<std::mutex> lk(core_->mutex);
    const Slot* s = FindLocked(*core_, slot);
    return s ? s->current_id.load(std::memory_order_acquire) : 0;
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
        // 这批孤儿已整批转入 pending, 从这一刻起它们要么被 join、要么被放弃,
        // 不再由 orphans_ 持有 -> 并发预算里的那一票在这里归还。
        core_->orphan_count = 0;
        for (auto& kv : core_->slots) {
            Slot* s = kv.second.get();
            if (s->thread.joinable()) {
                // 与登记路径同一套两阶段写法: 线程所有权最后一步才转移。写成
                // push_back(OwnedThread{..., std::move(s->thread), ...}) 的话, 扩容一失败
                // 那个临时对象就带着 joinable 的线程析构 —— 直接 std::terminate()。
                OwnedThread adopted;                        // 不含线程
                adopted.kind = s->kind;
                adopted.id = s->current_id.load(std::memory_order_acquire);
                adopted.slot = kv.first;                    // 可能抛(bad_alloc)
                adopted.done = std::move(s->done);
                pending.push_back(std::move(adopted));       // 可能抛(扩容)
                pending.back().thread = std::move(s->thread); // noexcept: 交出所有权
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
            // 终态一次性: 放弃之后它自己跑完并写了终态(可能是真的成功了)的话,
            // 这里再补一个 Canceled 就会把那个结论盖掉 —— 只在它还没收尾时才补。
            if (!s->running.load(std::memory_order_acquire))
                continue;
            s->state.store(TaskState::Canceled, std::memory_order_release);
            s->running.store(false, std::memory_order_release);
        }
    }
    core_->cv.notify_all();
}

} // namespace task
} // namespace videoeye
