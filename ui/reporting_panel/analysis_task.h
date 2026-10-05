#pragma once

// 报告页后台任务的明确所有权载体：线程、任务身份、取消令牌、生命周期守卫四者绑在一起。
//
// 这是 P0 修复（报告页线程生命周期）的核心数据结构，并在随后的 P1 整改里把"取消"和
// "终态"交回统一协议：原先这里另起了一颗 std::atomic<bool> cancelled，与
// core/domain/task/TaskProtocol.h 里那颗取消令牌并存 —— 上层置这颗、引擎看那颗（或反之），
// 谁置位了、谁没置位，全靠读代码的人自己对齐。现在 AnalysisTask 只留报告页特有的
// 两样（线程本体、alive 标志），取消一律走 task::CancelToken，身份/终态一律走
// task::TaskHandle + TaskManager。
//
// 本头文件不依赖 Qt，可被单测直接引用。

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "core/domain/task/TaskProtocol.h"

namespace videoeye {
namespace ui {

struct AnalysisTask {
    std::thread thread;
    // 唯一的取消来源：宿主置位（OnCancelBatch），任务体与 QcRunner 轮询同一颗标志。
    task::CancelToken cancel;
    // 任务身份与归还终态的凭证。报告页不自己编号，id 由 TaskManager 在 slot 里分配。
    task::TaskHandle handle;
    // 任务存续期间为 true；析构/回收时置 false 阻止回调触碰 this。
    // 这是**面板**的存活标志（不是任务的），所以仍留在这一层。
    std::atomic<bool> alive{true};
    // 线程体是否已经返回。回收必须靠它给 join 设上界 ——
    // std::thread 没有带超时的 join，不知道"跑完了没有"就只能干等着。
    std::atomic<bool> body_done{false};
};

// 回收预算（见 ReportingPanel::RetireTask）。
inline constexpr int kDefaultRecycleBudgetMs = 5000;

// 等一个任务体在预算内返回。返回 false 表示预算耗尽它还没回来。
//
// 为什么不直接 join：join 没有超时，任务体卡在非协作的第三方调用里（FFmpeg 读一个
// 无响应设备）时，关界面会永久挂住 —— 那正是评审 P1-3 里点名要去掉的"析构路径无限等待"。
// 这里改成轮询/睡眠等待 body_done，超预算就交由调用方决定后续（报告页是 join 而不是
// detach，理由见 RetireTask）。
//
// 入参可为空（默认构造未启动的任务），此时直接返回 true（无事可做）。
inline bool WaitTaskBody(const std::shared_ptr<AnalysisTask>& task,
                         int budget_ms = kDefaultRecycleBudgetMs) {
    if (!task)
        return true;
    // 从未启动过的线程没有可等的东西 —— 不判这一条的话，一个"构造好还没起线程"的
    // 任务会被这里白等满整个预算（默认 5s），析构路径上就是实打实的 5 秒卡顿。
    if (!task->thread.joinable())
        return true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budget_ms);
    while (!task->body_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// 带预算地回收一个任务：预算内等线程体返回 → join 收句柄 → 置空 shared_ptr。
//
// 退出码语义与原来的实现一致（先 join 再 reset，所以对仍 joinable 的线程赋值会
// terminate 的毛病不会再出现），区别只是这个 join 有上界。
//
// 调用方负责**在此之前**把任务取消掉并置好终态归还 —— 那三件事（取消来源、终态写入者、
// alive 标志）属于宿主，不属于这里。
inline void RecycleTask(std::shared_ptr<AnalysisTask>& task,
                        int budget_ms = kDefaultRecycleBudgetMs) {
    if (!task)
        return;
    WaitTaskBody(task, budget_ms);
    // 只有线程体在预算内回来过、或者这颗线程压根没启动，才轮得到 join。
    // 报告页的任务体捕获了 this（要往面板上刷结果），所以这里 join 而不是 detach：
    // detach 之后线程会往一个可能已经销毁的 QWidget 上排队消息。
    if (task->thread.joinable())
        task->thread.join();
    task.reset();
}

}  // namespace ui
}  // namespace videoeye
