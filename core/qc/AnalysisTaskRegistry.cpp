#include "core/qc/AnalysisTaskRegistry.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace qc {

AnalysisTaskSlot::~AnalysisTaskSlot() {
    // 析构即归还：持有者（分析线程体 / 派发侧）无论走哪条路径退出，额度都不会滞留。
    // 这条兜底是"统一登记"能成立的前提 —— 否则 detach 出去的线程体万一漏调 Release，
    // 派发侧的背压会永久卡住。
    Release();
}

// ❗❗ 状态转换与计数更新由**登记处**在同一把锁里完成，本类只做转发。
//
// 以前这里是"先改原子标志、再加锁改计数"的两段式：MarkExiting 把 exiting_ 置起来之后、
// 还没进临界区的那一瞬，另一条线程的 Release 就能插进来 —— 它看到 exiting_ 已置位，
// 于是去减一个**从未加过**的退出数（还有可能顺手把它清零），随后 MarkExiting 的计数
// 才姗姗来迟地 ++exiting_。一张凭证就此留下一个永远还不清的退出计数，背压从此偏严。
// 谁都能触发这个窗口：detach 路径上派发侧 MarkExiting 与分析线程体退出时的 Release
// 打的就是同一张凭证。
void AnalysisTaskSlot::Adopt() {
    AnalysisTaskRegistry::Instance().Adopt(*this);
}

bool AnalysisTaskSlot::IsAdopted() const {
    return adopted_.load(std::memory_order_acquire);
}

void AnalysisTaskSlot::MarkExiting() {
    AnalysisTaskRegistry::Instance().MarkExiting(*this);
}

bool AnalysisTaskSlot::IsExiting() const {
    return exiting_.load(std::memory_order_acquire);
}

void AnalysisTaskSlot::Release() {
    AnalysisTaskRegistry::Instance().Release(*this);
}

bool AnalysisTaskSlot::IsReleased() const {
    return released_.load(std::memory_order_acquire);
}

AnalysisTaskRegistry& AnalysisTaskRegistry::Instance() {
    static AnalysisTaskRegistry registry;
    return registry;
}

std::shared_ptr<AnalysisTaskSlot> AnalysisTaskRegistry::RegisterLocked() {
    auto slot = std::make_shared<AnalysisTaskSlot>();
    slot->id_ = next_id_++;
    slot->registered_ = true;
    slot->booked_ = AnalysisTaskSlot::Booked::Running;
    ++running_;
    peak_total_ = std::max(peak_total_, running_ + exiting_);
    return slot;
}

std::shared_ptr<AnalysisTaskSlot> AnalysisTaskRegistry::Acquire(int limit, int max_exiting, int backpressure_ms,
                                                                const std::function<bool()>& cancelled) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (limit <= 0 && max_exiting <= 0)
        return RegisterLocked();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(backpressure_ms);
    while (true) {
        auto fits = [&]() {
            if (limit > 0 && (running_ + exiting_) >= limit)
                return false;
            if (max_exiting > 0 && exiting_ >= max_exiting)
                return false;
            return true;
        };
        if (fits())
            return RegisterLocked();
        if (backpressure_ms <= 0)
            return nullptr; // 不背压：拿不到就走（调用方自行决定放行还是跳过）
        if (cancelled && cancelled())
            return nullptr;
        if (std::chrono::steady_clock::now() >= deadline)
            return nullptr;
        // 短片等待：退出中的任务随时可能还额度，等太久反而让整批看起来卡住。
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        lock.lock();
    }
}

std::shared_ptr<AnalysisTaskSlot> AnalysisTaskRegistry::AcquireUntilFit(int limit, int max_exiting,
                                                                        const std::function<bool()>& cancelled) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto fits = [&]() {
        if (limit > 0 && (running_ + exiting_) >= limit)
            return false;
        if (max_exiting > 0 && exiting_ >= max_exiting)
            return false;
        return true;
    };
    while (!fits()) {
        if (cancelled && cancelled())
            return nullptr; // 取消是这条队列唯一的出口
        // 条件变量之外再留一个 20ms 的兜底：任何一路归还都会 notify，但万一将来
        // 加了不通知的退出路径，也不会把派发侧永久挂住（最坏只是多转两圈）。
        cv_.wait_for(lock, std::chrono::milliseconds(20));
    }
    return RegisterLocked();
}

std::shared_ptr<AnalysisTaskSlot> AnalysisTaskRegistry::AcquireUnbounded() {
    std::lock_guard<std::mutex> lock(mutex_);
    return RegisterLocked();
}

// 三条转换都遵循同一条规则：**先判状态，再动计数，全程同一把锁**。
// 合法转换只有 运行 -> 退出中 -> 已释放（以及 运行 -> 已释放），反向与重复一律幂等忽略。
void AnalysisTaskRegistry::Adopt(AnalysisTaskSlot& slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 已归还 / 已判定退出的额度不能再被认领：那会让派发侧以为它还在被占用。
    if (slot.released_.load(std::memory_order_acquire) || slot.exiting_.load(std::memory_order_acquire))
        return;
    slot.adopted_.store(true, std::memory_order_release);
}

void AnalysisTaskRegistry::MarkExiting(AnalysisTaskSlot& slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot.released_.load(std::memory_order_acquire))
        return; // 已经归还：额度都已交回，不能再改它的账
    if (slot.exiting_.load(std::memory_order_acquire))
        return; // 幂等：detach 路径上的派发侧与线程体退出时会撞在一起
    slot.exiting_.store(true, std::memory_order_release);
    // 只搬登记处发出、且被分析线程体认领过的凭证的账 —— 未登记（直接 make_shared 出来
    // 的）与未被认领（假分析函数，由派发侧兜底 Release）的都不在这里动，否则同一条任务
    // 会被计两次。
    if (slot.booked_ != AnalysisTaskSlot::Booked::Running)
        return;
    if (!slot.adopted_.load(std::memory_order_acquire))
        return;
    slot.booked_ = AnalysisTaskSlot::Booked::Exiting;
    --running_;
    ++exiting_;
    peak_total_ = std::max(peak_total_, running_ + exiting_);
    cv_.notify_all();
}

void AnalysisTaskRegistry::Release(AnalysisTaskSlot& slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot.released_.load(std::memory_order_acquire))
        return; // 幂等：派发侧兜底归还与线程体退出时的归还会撞在一起
    slot.released_.store(true, std::memory_order_release);
    // 状态与计数在同一临界区里结算：账本写着哪边就减哪边，不存在"减没加过的那一边"。
    switch (slot.booked_) {
    case AnalysisTaskSlot::Booked::Running:
        if (running_ > 0)
            --running_;
        break;
    case AnalysisTaskSlot::Booked::Exiting:
        if (exiting_ > 0)
            --exiting_;
        break;
    case AnalysisTaskSlot::Booked::None:
    case AnalysisTaskSlot::Booked::Released:
        break;
    }
    slot.booked_ = AnalysisTaskSlot::Booked::Released;
    slot.exiting_.store(false, std::memory_order_release);
    cv_.notify_all();
}

int AnalysisTaskRegistry::RunningCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

int AnalysisTaskRegistry::ExitingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return exiting_;
}

int AnalysisTaskRegistry::TotalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ + exiting_;
}

int AnalysisTaskRegistry::PeakTotalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_total_;
}

} // namespace qc
} // namespace videoeye
