#pragma once

// 实际存活的**分析任务**的统一登记处（评审 P2-3）。
//
// 以前"并发上限"只统计批量调度自己的 worker 数：一项分析超预算返回后 worker 会立刻去
// 处理下一个文件，而前一项内部的分析线程可能还在退出过程中（第三方 IO / 解码没及时
// 响应取消）。于是 max_parallel=4 时真正的分析线程可能是 4 + N 条，且这 N 条只能靠
// 日志里"分析线程退出"那一行去数 —— 没有占用统计，也没有回收登记。
//
// 现在登记的是**分析任务本身**（QcRunner 起的那个线程体），额度在**线程体真正退出**
// 时才归还，而不是 analyze() 返回时：
//   * 正常路径：analyze() 内部 join 完才返回，额度随之归还；
//   * 放弃路径（detach）：额度由被放弃的线程体在自己退出时归还 —— 期间它占着额度，
//     派发侧因此看得见"还有 N 条在退"，可以据此背压。
//
// 两条额度分开计数：running（正常跑）与 exiting（已判定放弃、正在退出）。派发侧对
// 后者单独设上限 —— 否则一堆退不干净的任务会把并发额度吃光，而它们其实已经不算工作了。
//
// 本文件只吃 std::，不碰 Qt / FFmpeg / AnalysisEngine，可脱离媒体直接单测。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace videoeye {
namespace qc {

class AnalysisTaskRegistry;

// 一次分析任务的占用凭证。
//
// 所有权规则（这是"统一登记"的关键）：
//   * 由派发侧（BatchQcRunner）在派发前 Acquire 出来，随请求交给分析函数；
//   * 分析函数（QcRunner）的线程体在**开始**时 Adopt() 认领，退出时（含被 detach 的
//     那条路径）经析构/作用域守卫归还 —— 额度因此跟线程体的真实寿命对齐；
//   * 若分析函数根本不认领（单测里的假实现、未来换执行器），派发侧在 analyze() 返回
//     后兜底归还（IsAdopted() 为假时才还），既不泄漏额度也不会提前释放。
class AnalysisTaskSlot {
public:
    AnalysisTaskSlot() = default;
    ~AnalysisTaskSlot();
    AnalysisTaskSlot(const AnalysisTaskSlot&) = delete;
    AnalysisTaskSlot& operator=(const AnalysisTaskSlot&) = delete;

    // 分析线程体认领这个额度（幂等）。
    void Adopt();
    bool IsAdopted() const;
    // 标记"已经判定放弃、正在退出"：仍占额度，但计入 exiting 而不是 running。
    void MarkExiting();
    bool IsExiting() const;
    // 归还额度（幂等；之后再 MarkExiting / Adopt 都不再生效）。
    void Release();
    bool IsReleased() const;

    std::int64_t id() const {
        return id_;
    }

private:
    friend class AnalysisTaskRegistry;
    std::int64_t id_ = 0;
    // 是否由登记处发出（RegisterLocked 里置位）。统计只认登记过的凭证 —— 直接
    // make_shared 出来的裸凭证（单测里常见）不该把计数减成负数。
    bool registered_ = false;
    std::atomic<bool> adopted_{false};
    std::atomic<bool> exiting_{false};
    std::atomic<bool> released_{false};
};

using AnalysisTaskSlotPtr = std::shared_ptr<AnalysisTaskSlot>;

class AnalysisTaskRegistry {
public:
    static AnalysisTaskRegistry& Instance();

    // 占一个分析额度。额度不足时**背压等待**（不是直接放行，也不是失败）：
    //   limit          —— 同时存活（running + exiting）的分析任务上限；<=0 表示不限
    //   max_exiting    —— 正在退出的任务数上限；<=0 表示不限
    //   backpressure_ms—— 最长等待（毫秒）；0 = 不等待，拿不到就返回 nullptr
    //   cancelled      —— 取消谓词：取消时立刻停止等待并返回 nullptr
    // 返回 nullptr 表示没拿到（已取消 / 背压超时 / 参数为 0 且不等待）。
    AnalysisTaskSlotPtr Acquire(int limit, int max_exiting, int backpressure_ms = 0,
                                const std::function<bool()>& cancelled = nullptr);

    // 不限量的登记（单文件分析等没有调度上限的场景）：只为可观测，不做背压。
    AnalysisTaskSlotPtr AcquireUnbounded();

    void MarkExiting(AnalysisTaskSlot& slot);
    void Release(AnalysisTaskSlot& slot);

    int RunningCount() const;
    int ExitingCount() const;
    int TotalCount() const;
    // 历史峰值（诊断"并发到底冲到多高"用，不需要额外埋点）
    int PeakTotalCount() const;

private:
    AnalysisTaskRegistry() = default;
    AnalysisTaskSlotPtr RegisterLocked();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int running_ = 0;
    int exiting_ = 0;
    int peak_total_ = 0;
    std::int64_t next_id_ = 1;
};

} // namespace qc
} // namespace videoeye
