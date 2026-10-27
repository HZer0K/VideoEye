#pragma once

// 单次分析的**三项等待策略**（以前只有一个 join_budget_ms，三件事被叠在一起）。
//
// 背景（评审 P1-1）：报告页把"关停回收预算" kDefaultRecycleBudgetMs(5000) 当成
// AnalyzeFile 的等待预算传下去，于是任何**正常运行超过 5 秒**的分析（长视频、多音轨、
// 深度档）都会被请求取消并按超时收尾；批量侧没指定预算时又退回另一个默认值 30 秒。
// 同一个参数既当"允许跑多久"又当"关停等多久"，两个口径互相污染 —— 调大常量只会
// 让关停变慢，调小又会误杀长分析。
//
// 现在三件事各自独立，语义互不覆盖：
//   * analysis_budget_ms —— "允许这次分析跑多久"。业务配置决定，默认 0 = 不限时。
//     正常运行不该因为"跑得久"被判超时；想给上限就显式给（批量 = per_item_timeout_ms）。
//   * io_idle_timeout_ms —— "多久没有任何动静就算卡住"。判据是"距上一次进度/结算"，
//     与总时长无关 —— 于是"跑了 10 分钟但一直在出结果"不会被误判，而"打开后再无回音"
//     能被及时放弃。
//   * cancel_wait_ms     —— "判定放弃之后最多再等线程体多久"。它只在**已经决定放弃**
//     （调用方取消 / 超预算 / IO 无响应）之后才开始计时，不影响正常运行；到期仍不退出
//     就放弃这条线程（detach），让调用方能返回。关停路径因此有明确上界。
//
// 本文件只有纯逻辑（无 Qt / 无 FFmpeg / 无 IO），等待决策可被单测直接驱动 ——
// "正常分析超过 5 秒不该被取消"这条回归因此不需要真实媒体文件也能钉住。

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace videoeye {
namespace qc {

// 判定放弃之后给线程体自己收尾的等待上限（毫秒）。
// 3 秒的依据：取消链一路通到 FFmpeg 中断回调（见 AnalysisInputSession），正常收尾是
// 毫秒级；给到秒级是为了容忍"第三方解码器正在收一个大块"这种尾部延迟。
inline constexpr int kDefaultCancelWaitMs = 3000;

// 等待循环的默认轮询间隔（毫秒）：既要及时把"调用方取消"转成引擎可见的取消源，
// 又不能让等待线程空转。
inline constexpr int kDefaultWaitPollMs = 100;

struct QcRunBudgets {
    // "允许这次分析跑多久"。0 = 不限时。
    int analysis_budget_ms = 0;
    // "多久没有动静就算卡住"。0 = 不判空闲。
    int io_idle_timeout_ms = 0;
    // "判定放弃之后最多再等线程体多久"。恒为正，见 kDefaultCancelWaitMs 的注释。
    int cancel_wait_ms = kDefaultCancelWaitMs;

    bool HasAnalysisBudget() const {
        return analysis_budget_ms > 0;
    }
    bool HasIdleTimeout() const {
        return io_idle_timeout_ms > 0;
    }

    // 正常分析：不给总时长上限，只有取消与空闲兜底。
    static QcRunBudgets Unlimited() {
        return QcRunBudgets{};
    }
    static QcRunBudgets WithAnalysisBudget(int ms) {
        QcRunBudgets budgets;
        budgets.analysis_budget_ms = ms;
        return budgets;
    }
};

// 为什么放弃这次分析（决定收尾文案与 timed_out 标记）
enum class QcAbandonReason {
    None = 0,   // 还在正常运行
    Cancelled,  // 调用方请求取消（批量"停止" / 面板回收）
    OverBudget, // 超过 analysis_budget_ms
    IoStalled,  // 静默超过 io_idle_timeout_ms
};

// 等待循环的观测状态（由执行器每轮刷新）
struct QcWaitState {
    int elapsed_ms = 0; // 开跑到现在的时长
    int idle_ms = 0;    // 距上一次进度/结算的时长
    QcAbandonReason reason = QcAbandonReason::None;
    int abandon_elapsed_ms = 0; // 判定放弃之后又过了多久

    bool abandoning() const {
        return reason != QcAbandonReason::None;
    }
};

enum class QcWaitDecision {
    Continue,      // 继续等
    RequestCancel, // 判定放弃：请求取消（引擎可见），并在 cancel_wait_ms 内继续等它收尾
    Abandon,       // 放弃这条线程（detach），本次按放弃原因收尾
};

// 等待决策（纯函数）：
//   * 还没判定放弃时，只有"超预算"与"IO 无响应"能触发放弃；
//     analysis_budget_ms = 0 时第一条永不触发 —— 这就是"正常分析不限时"。
//   * 一旦判定放弃，只看 cancel_wait_ms 到了没有。
inline QcWaitDecision DecideWait(const QcRunBudgets& budgets, const QcWaitState& state) {
    if (state.abandoning()) {
        return state.abandon_elapsed_ms >= budgets.cancel_wait_ms ? QcWaitDecision::Abandon : QcWaitDecision::Continue;
    }
    if (budgets.analysis_budget_ms > 0 && state.elapsed_ms >= budgets.analysis_budget_ms)
        return QcWaitDecision::RequestCancel;
    if (budgets.io_idle_timeout_ms > 0 && state.idle_ms >= budgets.io_idle_timeout_ms)
        return QcWaitDecision::RequestCancel;
    return QcWaitDecision::Continue;
}

// 下一次 wait_for 的时长。必须夹到"下一个判定点"，否则 100ms 的轮询粒度会把
// "预算 1 毫秒"拖成"实际等满 100 毫秒"—— 短预算用例就失去确定性了。
inline std::chrono::milliseconds NextWaitSlice(const QcRunBudgets& budgets, const QcWaitState& state,
                                               int poll_ms = kDefaultWaitPollMs) {
    int slice = std::max(poll_ms, 1);
    if (state.abandoning()) {
        slice = std::min(slice, std::max(budgets.cancel_wait_ms - state.abandon_elapsed_ms, 1));
        return std::chrono::milliseconds(slice);
    }
    if (budgets.analysis_budget_ms > 0)
        slice = std::min(slice, std::max(budgets.analysis_budget_ms - state.elapsed_ms, 1));
    if (budgets.io_idle_timeout_ms > 0)
        slice = std::min(slice, std::max(budgets.io_idle_timeout_ms - state.idle_ms, 1));
    return std::chrono::milliseconds(slice);
}

} // namespace qc
} // namespace videoeye
