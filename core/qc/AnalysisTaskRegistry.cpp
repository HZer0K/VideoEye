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

void AnalysisTaskSlot::Adopt() {
    // 已归还的额度不能再被认领：那会使派发侧以为它还在被占用，背压计算失真。
    if (released_.load(std::memory_order_acquire))
        return;
    adopted_.store(true, std::memory_order_release);
}

bool AnalysisTaskSlot::IsAdopted() const {
    return adopted_.load(std::memory_order_acquire);
}

void AnalysisTaskSlot::MarkExiting() {
    if (released_.load(std::memory_order_acquire))
        return;
    if (exiting_.exchange(true, std::memory_order_acq_rel))
        return; // 已经标过
    AnalysisTaskRegistry::Instance().MarkExiting(*this);
}

bool AnalysisTaskSlot::IsExiting() const {
    return exiting_.load(std::memory_order_acquire);
}

void AnalysisTaskSlot::Release() {
    if (released_.exchange(true, std::memory_order_acq_rel))
        return; // 幂等：派发侧兜底归还与线程体退出时的归还会撞在一起
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

std::shared_ptr<AnalysisTaskSlot> AnalysisTaskRegistry::AcquireUnbounded() {
    std::lock_guard<std::mutex> lock(mutex_);
    return RegisterLocked();
}

void AnalysisTaskRegistry::MarkExiting(AnalysisTaskSlot& slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 只统计登记处发出的凭证；未登记（直接 make_shared 出来的）一律忽略，
    // 否则计数会被同一条任务减两次。
    if (!slot.registered_)
        return;
    if (!slot.adopted_.load(std::memory_order_acquire))
        return;
    if (running_ > 0)
        --running_;
    ++exiting_;
    peak_total_ = std::max(peak_total_, running_ + exiting_);
    cv_.notify_all();
}

void AnalysisTaskRegistry::Release(AnalysisTaskSlot& slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!slot.registered_)
        return;
    if (slot.exiting_.load(std::memory_order_acquire)) {
        if (exiting_ > 0)
            --exiting_;
        slot.exiting_.store(false, std::memory_order_release);
    } else if (running_ > 0) {
        --running_;
    }
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
