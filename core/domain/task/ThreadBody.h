#pragma once

// 线程体的**统一收口**: 异常边界 + 终态判定 + 离开作用域必跑的清理。
//
// 为什么住在 core/domain/task 而不是 infrastructure/concurrency(TaskManager 旁边):
// 分层规则只允许 infrastructure 单向依赖 core/*, 于是 core/qc、core/qt、core/player
// 这三个最需要它的地方**看不见**放在 infrastructure 里的东西 —— 协议单元
// (core/domain/task/TaskProtocol.h)当初就是因为同一个原因下沉的, 这里是同一件事的
// 第二半。判断 owed 很简单: 谁能用, 它就该在哪一层。本文件是纯 C++17 (不碰 Qt /
// FFmpeg / 任何调度器), 所以放在叶子层没有代价。
//
// 它解决的是项目里重复了六次的同一种写法:
//
//     std::thread(worker([...]() {
//         try {
//             ...任务体...
//         } catch (const std::exception& e) { ...降级成失败... }
//         catch (...) { ... }
//         ...别忘了置 xx_done / xx_exited_ 标志...
//     }));
//
// 六份拷贝里有一份漏了 try(Bug: 报告页两条线程), 有几份注释里还在重复解释同一句
// "std::thread 的入口是 noexcept, 逃逸就是 terminate"。收 Template 的目的不是省几行,
// 是让"线程体必须怎么收尾"只有一个答案。

#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "core/domain/task/TaskProtocol.h"

namespace videoeye {
namespace task {

// 线程体的**异常边界**: 跑 body, 把它可能抛出的一切就地转成错误消息。
//
// 必要性: MSVC STL 里 std::thread 的入口是不抛保证的(成员函数作入口时尤其明显),
// 任何逃逸的异常直接 std::terminate() —— 进程闪退, UI 上连"这条任务失败了"都看不到,
// 也拿不到任何栈信息。有了它, "分析器/解析器抛异常"从"进程没了"降级成"这条任务 Failed"。
//
// 返回值语义(刻意做成 optional, 尽量小 surface):
//   空     —— body 正常返回;
//   非空   —— 捕获到的异常的文本(message)。
// 它**不返回 TaskState**: 要结合取消令牌 / 超时一起判定终态请用下面的 ResolveTerminal,
// 把"两件不同的事"揉成一个返回值会让调用方看不出自己在答哪个问题。
//
// noexcept 的边界: 只有构造错误消息那次分配理论上有抛的可能(std::bad_alloc);
// 那一刻进程本来也快没了, 与其在日志里造一个假的成功/失败, 不如让它按 terminate 走。
template <typename Body>
std::optional<std::string> GuardBody(Body&& body) noexcept {
    try {
        body();
        return std::nullopt;
    } catch (const std::exception& e) {
        return std::string(e.what());
    } catch (...) {
        return std::string("未知类型的异常");
    }
}

// 只有 GuardBody 的结果与令牌、不想套 RunTerminalBody 时用这个纯判定(幂等、可单测)。
inline TaskState ResolveTerminal(const std::optional<std::string>& failure,
                                 const CancelToken& token) noexcept {
    if (token.IsCanceled()) return TaskState::Canceled;
    if (failure) return TaskState::Failed;
    return TaskState::Succeeded;
}

// GuardBody + 终态判定一次做完。任务体产不出别的结论时用它最省事 —— 调方不必再写
// "if (failed) ... else ..." 那套本就千篇一律的分支。
template <typename Body>
TaskState RunTerminalBody(const CancelToken& token, Body&& body) noexcept {
    const std::optional<std::string> failure = GuardBody(std::forward<Body>(body));
    return ResolveTerminal(failure, token);
}

// 离开作用域必跑一次的清理器。C++17 没有 std::scope_exit, 而"线程体提前退出忘了置
// 完成标志"在这个项目里已经出过两次事故(报告页 body_done / 视觉缺陷 worker_exited_):
// 漏置的后果是回收路径傻等到预算耗尽, 严格 Cooperative 的任务还会一路卡到 join。
//
// 用法:
//     const auto settle = task::MakeScopeExit([task]() { task->body_done.store(true); });
template <typename Cleanup>
class ScopeExit {
public:
    explicit ScopeExit(Cleanup cleanup) : cleanup_(std::move(cleanup)) {}
    ~ScopeExit() { cleanup_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

private:
    Cleanup cleanup_;
};

template <typename Cleanup>
ScopeExit<Cleanup> MakeScopeExit(Cleanup cleanup) {
    return ScopeExit<Cleanup>(std::move(cleanup));
}

}  // namespace task
}  // namespace videoeye
