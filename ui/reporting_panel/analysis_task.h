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
    // 线程体是否已经返回。回收路径靠它把"预期多久收回来"变成可观测的（超时只告警，
    // 不是 join 的上界 —— 见 RecycleTask 的严格 Cooperative 说明）。
    std::atomic<bool> body_done{false};
};

// 造一个"宿主真的能置位"的取消令牌。
//
// 为什么不直接 `task->cancel = task::CancelToken{}`：默认构造出来的令牌**没有标志**
// （flag 为空），于是 IsCanceled() 恒为 false、RequestCancel() 是空操作 —— 任务体
// 把它当取消标志就等于"永远取消不掉"。平时看不出来（任务照常跑完），只在关停 /
// 取消那条路径上炸成"析构卡死"或"取消按钮点了没反应"。
// ReportingPanel 走的是 `task->cancel = handle.cancel`（真令牌），所以生产侧一直是对的；
// 但这个结构体可以被别处拷贝使用，默认的坑就留在这儿。起任务请用这个工厂。
inline task::CancelToken MakeCancelToken() {
    return task::CancelToken(std::make_shared<std::atomic<bool>>(false));
}

// 回收的观测预算（见 ReportingPanel::RetireTask）：超过它说明任务体比预期收得慢，
// 调用方据此打告警。**它不限制 join** —— 报告页走严格 Cooperative，见 RecycleTask。
inline constexpr int kDefaultRecycleBudgetMs = 5000;

// 等一个任务体在预算内返回。返回 false 表示预算耗尽它还没回来。
//
// 这个预算只作**观测/告警线**用，不改变回收动作：报告页的任务体是严格 Cooperative 的
// （取消链一路通到引擎与 FFmpeg 中断回调），一定收得回来；预算回答的是"它是不是比
// 预期慢"，而不是"超时之后就不要它了"。
//
// 为什么不直接 join：join 没有超时，等不到"线程体是否已经返回"这个信号时，关界面
// 想知道它到底卡在哪就只能靠日志 —— 这里先把"有没有回来"变成可观测的布尔量。
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

// 回收一个任务：等线程体返回（观测预算）→ join 收句柄 → 置空 shared_ptr。
// 返回 false 表示**预算内**线程体还没回来（只作告警线，join 仍无上界，理由见下）。
//
// 报告页走的是"严格 Cooperative"路线（评审 P1-5 的方案 A）：
//   * RunSingle / RunBatch / QcRunner 的取消链已经打通 —— 任务体轮询的是同一颗令牌，
//     QcRunner 把它转成引擎的 cancel_source，FFmpeg 的中断回调也能看见它；
//   * 所以任务体一定收得回来，预算只是"预期多快收回来"的观测/告警线，不是 join 上界。
//
// 这里**不能**改成"超预算就 detach"：任务体捕获了 ReportingPanel 的 this，并通过
// QMetaObject::invokeMethod(this, ...) 投递 UI 更新（见 ReportingPanel.cpp 的 PostToUI），
// detach 之后这些投递会落到已经销毁的 QWidget 上。在"去掉悬空访问"与"给出硬性时间上限"
// 之间，报告页明确选前者：析构可能继续等待，但不会访问已销毁对象 —— 这正是"不能一边
// 声称有硬预算、一边无条件 join"的诚实版本。
//
// 调用方负责**在此之前**把任务取消掉并置好终态归还 —— 那三件事（取消来源、终态写入者、
// alive 标志）属于宿主，不属于这里。
inline bool RecycleTask(std::shared_ptr<AnalysisTask>& task,
                        int budget_ms = kDefaultRecycleBudgetMs) {
    if (!task)
        return true;
    const bool body_back = WaitTaskBody(task, budget_ms);
    // 严格 Cooperative: join 没有上界。超预算仍然 join —— 契约上任务体必须响应取消,
    // 走到这里只说明它比预期慢, 不等于可以把它连同它引用的宿主一起丢掉。
    if (task->thread.joinable())
        task->thread.join();
    task.reset();
    return body_back;
}

}  // namespace ui
}  // namespace videoeye
