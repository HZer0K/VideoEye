#pragma once

// 后台任务的**协议层**: 任务身份、取消令牌、终态种类、关闭策略。
//
// 为什么协议住在 domain、实现住在 infrastructure:
// 项目里曾经同时存在五套后台任务体系(TaskManager / QtWorkerOwner / QtAnalysisController /
// QcRunner / ReportingPanel), 四件事分化出四种答案 —— "取消令牌长什么样""终态怎么判断"
// "过期结果怎么丢""关闭时能放弃任务吗"。想收敛它们, 协议就必须能被**所有那几处**看见,
// 而它们分布在 core/qc、core/qt、core/exporter、ui 好几层里。
//
// 协议原先直接写在 infrastructure/concurrency/TaskManager.h 里, 可分层规则只允许
// infrastructure 依赖 core/*(方向朝下), core/qc 与 core/qt 一律不许反向 include
// infrastructure —— 也就是说 QcRunner、QtAnalysisController 这两处恰恰最该统一的地方
// 根本看不到这个协议, 统一无从谈起。所以这里只放**纯 C++ 的协议类型**(不碰 Qt、
// 不碰 FFmpeg、不碰任何执行器), 由 infrastructure/concurrency/TaskManager 承载唯一的
// 调度实现。谁想参与这套协议, include 本头文件就够了。
//
// 与 domain 里其它 *Result.h 一样, 本文件是纯声明: 没有状态机、没有锁、没有调度。

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace videoeye {
namespace task {

using TaskId = std::uint64_t;

// 任务终态。Idle/Running 是过程态, 其余三种是一旦写入就不再变化的终态。
enum class TaskState {
    Idle,      // 该 slot 上当前没有任务
    Running,   // 运行中
    Succeeded, // 终态: 成功
    Failed,    // 终态: 出错(含异常逃逸)
    Canceled,  // 终态: 被取消 / 被同 slot 的新任务取代
};

inline bool IsTerminalState(TaskState state) {
    return state == TaskState::Succeeded || state == TaskState::Failed || state == TaskState::Canceled;
}

// 关闭阶段能不能"放弃"这个任务。这个区分是关闭流程能否给出**整体**耗时上限的前提:
// Cooperative 的任务体必须响应取消, 所以等到预算耗尽也一定收得回来;
// BlockingIo 可能卡在第三方阻塞调用里, 过预算就只能放弃(见 TaskManager::WaitForAll)。
enum class TaskKind {
    // 协作式: 任务体会轮询取消令牌, 不会长时间停在第三方阻塞调用里。
    // 关闭时**一定会** join 它 —— join 是唯一能证明它已经不再访问外部对象的手段,
    // 所以契约被破坏时宁可多等, 也不冒悬空访问的风险。
    Cooperative,

    // 可能卡在第三方阻塞调用上的任务: FFmpeg 的网络 IO、异常设备读取等。这类调用即使
    // 装了 AVIOInterruptCB 也不保证一定响应 —— 比如回调只在下一个网络包到来时才被检查。
    // 关闭时在预算内等不到就被放弃(detach), 不再拖住整个关闭流程。
    //
    // 代价是一条更严格的生命周期约定, 调用方得遵守, 否则 detach 之后就是悬空访问:
    //   * 任务体只能按值 / shared_ptr 捕获依赖, 不得持有任何可能在它结束前销毁的裸引用;
    //   * 不得依赖"任务一定会被 join"这件事做任何清理;
    //   * 结果发布必须自己判过期(TaskManager::IsCurrent)。
    BlockingIo,
};

// 取消令牌: 由任务体在循环里轮询。
//
// 令牌内部持有一份 shared_ptr 的取消标志, 因此即使调度器先于任务线程析构,
// 任务体调用 IsCanceled() 仍然是安全的。
class CancelToken {
public:
    CancelToken() = default;
    explicit CancelToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {
    }

    bool IsCanceled() const {
        return flag_ && flag_->load(std::memory_order_acquire);
    }
    // 宿主请求取消（幂等）。
    //
    // 协议里写的是"宿主置位、任务体轮询"，但令牌原先只有"读"没有"写" ——
    // 每一个有自带线程的上层（报告页、诊断页）都只好自己再备一颗 std::atomic<bool>，
    // 于是"取消"这个项目里就有了第二套、第三套标志。这个口子就是为把那些改回来开的。
    //
    // 故意做成 const: 置位不需要改令牌本身，只是改它指着的那一颗。
    void RequestCancel() const {
        if (flag_) flag_->store(true, std::memory_order_release);
    }
    explicit operator bool() const {
        return static_cast<bool>(flag_);
    }

    // 暴露底层取消标志(shared_ptr 保活)。供调用方把指针交给 FFmpeg 的 AVIOInterruptCB.opaque,
    // 使 avformat_open_input 等阻塞 IO 在取消之后能及时中断(否则关闭流程会挂死)。
    // 无令牌时返回空。
    std::shared_ptr<std::atomic<bool>> flag() const {
        return flag_;
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// 所有后台任务统一的传播载体: 宿主把它交给任务体, 任务体拿它判断"我是不是还在被要求
// 继续"以及"我的结论还能不能报"。
//
// 约定(执行方式可以不同, 这三条不许分叉):
//   * id      —— 本次任务的唯一身份, 由 Begin 在锁内分配; 上报终态 / 判过期都认它。
//   * cancel  —— 取消令牌的唯一来源, 宿主置位、任务体轮询; 禁止再另起一套标志。
//   * kind    —— 关闭预算耗尽时这个任务能不能被放弃, 调度器据此决定 join 还是 detach。
//
// 自带线程(QThread / 自建 std::thread)的任务也走这份协议: 它照样经 Begin 拿到 id 与
// 令牌, 在自己的线程里轮询, 收尾时把终态还回同一个 slot。
struct TaskHandle {
    TaskId id = 0;                       // 0 = 未登记 / 并发已满被拒
    CancelToken cancel;                  // 唯一的取消来源
    TaskKind kind = TaskKind::Cooperative;
    std::string slot;                    // 归还终态 / 判过期要用的槽位

    bool valid() const noexcept { return id != 0; }
};

}  // namespace task
}  // namespace videoeye
