#pragma once

// 一次修复的共享执行预算（R10）。
//
// max_seconds 在领域模型里声明的是「单次修复的总时间预算」，但旧实现只在
// RepairExecutor 写完封装头、进入逐包循环之后才开始计时 —— 诊断、盒结构解析、
// 样本表遍历、参考段读取/拼接、打开与探测、输出解码验证全都在预算之外。
// 结果是界面上设了 60 秒，任务可能跑几分钟。
//
// 这里把「取消标志 + steady_clock 绝对截止时间」合成一个只读值对象，由引擎在
// 入口创建，贯穿诊断 / 规划 / 执行 / 验证 / 发布。所有 CPU 循环用 Checkpoint()，
// 所有阻塞式 FFmpeg IO 用 RemainingUs() 重新武装中断回调的截止时间。
//
// ⚠️ 预算不是硬实时：单次 avcodec_send_packet/receive_frame、单次 read 系统调用
// 这类不可抢占区间仍可能越过截止点，报告里必须如实写明这个限制。

#include <atomic>
#include <chrono>
#include <cstdint>

namespace videoeye {
namespace repair {

class RepairRunBudget {
public:
    using Clock = std::chrono::steady_clock;

    RepairRunBudget() = default;

    RepairRunBudget(const std::atomic<bool>* cancel, int max_seconds) : cancel_(cancel) {
        if (max_seconds > 0) {
            has_deadline_ = true;
            deadline_ = Clock::now() + std::chrono::seconds(static_cast<int64_t>(max_seconds));
        }
    }

    bool Canceled() const {
        return cancel_ != nullptr && cancel_->load(std::memory_order_acquire);
    }

    bool Expired() const {
        return has_deadline_ && Clock::now() >= deadline_;
    }

    // 循环内统一检查点：取消或超预算都返回 true。
    bool Checkpoint() const {
        return Canceled() || Expired();
    }

    bool HasDeadline() const {
        return has_deadline_;
    }

    Clock::time_point Deadline() const {
        return deadline_;
    }

    const std::atomic<bool>* CancelFlag() const {
        return cancel_;
    }

    // 剩余微秒数；-1 = 没有设置截止时间。超预算后返回 0。
    int64_t RemainingUs() const {
        if (!has_deadline_)
            return -1;
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline_ - Clock::now()).count();
        return left > 0 ? static_cast<int64_t>(left) : 0;
    }

private:
    const std::atomic<bool>* cancel_ = nullptr;
    bool has_deadline_ = false;
    Clock::time_point deadline_{};
};

} // namespace repair
} // namespace videoeye
