#pragma once

// 报告页后台任务的启停与终态归还（从 ReportingPanel 抽出）。
//
// 这一层管的是报告页**唯一带后台线程**的那套机制，也是最容易出错的一套：
//   * 起线程前必须先回收上一个任务（对仍 joinable 的 std::thread 直接赋值 = terminate）；
//   * 线程体必须就地收口异常，否则要么 std::terminate，要么 body_done 永不置位、
//     回收路径按严格 Cooperative 无限 join（关界面永久挂住）；
//   * 终态只能由**宿主线程**归还（任务体捕获了面板指针，tasks_ 比它先死）。
//
// 三条约束原先靠 ReportingPanel 里四个成员函数各自遵守（RetireTask / FinishTask /
// PostToUi / ReportThreadFailure），现在收成一个对象，UI 侧只留业务（RunSingle /
// RunBatch 本体与它们要刷哪些控件）。
//
// 它不是 QObject：不参与事件循环，只是把"槽位、句柄、线程、终态"绑在一起的一只
// 普通对象，由面板以 unique_ptr 持有 —— 于是它一定在面板成员之前析构，不存在
// "子对象比宿主晚死"的窗口。UI 侧的动作全部以 std::function 注入，可脱离 QWidget
// 驱动（见 test_reporting_task_coordinator.cpp）。

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <QObject> // QMetaObject::invokeMethod 的目标类型
#include <QString>

#include "core/domain/task/TaskProtocol.h"
#include "infrastructure/concurrency/TaskManager.h"

#include "ui/reporting_panel/analysis_task.h"

namespace videoeye {
namespace ui {

class ReportingTaskCoordinator {
public:
    // 报告页两个槽位：单文件分析 / 目录批量扫描。二者互斥（busy_ 已在外面拦住），
    // 但句柄与线程各归各的 —— 换任务时只回收自己那个槽位。
    enum class Slot { Single, Batch };

    struct Deps {
        // UI 更新的投递目标（面板自己，不拥有）。所有回调都经它排队回主线程。
        QObject* ui = nullptr;
        // 线程体抓到异常时的收尾动作。控制器不认识标签控件，所以整段注入。
        std::function<void(const QString& text)> show_failure; // 结论 / 汇总标签刷异常文本
        std::function<void(const QString& text)> append;       // 摘要区追加一行
        std::function<void(bool busy)> set_busy;
        std::function<void()> abort_pending_export; // 异常时丢弃挂起的输出目录
    };

    explicit ReportingTaskCoordinator(Deps deps);
    // 析构兜底：面板正常会在自己的析构里调 RetireAll()，这里再兜一次，
    // 免得将来有人加了新退出路径忘记收尾。
    ~ReportingTaskCoordinator();

    // 任一槽位是否仍在 Running（终态已归还则为 false）。
    bool IsRunning() const;

    // 启动一个后台任务：回收同槽位的旧任务 → 分配句柄 → 起线程（异常就地收口）。
    // 返回空表示并发已满，调用方负责提示，不要静默不跑。
    // body 在**线程里**执行，入参即本次任务（取消令牌与句柄都已经装好）。
    std::shared_ptr<AnalysisTask> Start(Slot slot,
                                        const std::function<void(const std::shared_ptr<AnalysisTask>&)>& body);

    // 只请求取消（取消按钮）：终态留给任务体自己归还，已完成的条目保留结果。
    void RequestCancel(Slot slot);

    // 任务**正常完成**时由 UI 线程归还终态并清掉该槽位句柄。
    void Finish(const std::shared_ptr<AnalysisTask>& task, task::TaskState terminal);

    // 取消 + 回收两个槽位（析构 / 换任务）。严格 Cooperative：join 无上界。
    void RetireAll();

    // 仅当任务仍存活时把更新投递回主线程；否则静默丢弃（面板正在销毁）。
    void PostToUi(const std::shared_ptr<AnalysisTask>& task, const std::function<void()>& updater) const;

private:
    std::shared_ptr<AnalysisTask>& SlotTask(Slot slot);
    void Retire(std::shared_ptr<AnalysisTask>& task);
    // 线程体抓到异常时的收尾（见 core/domain/task/ThreadBody.h 的 GuardBody）。
    void ReportThreadFailure(const std::shared_ptr<AnalysisTask>& task, const std::string& message);
    const char* SlotName(Slot slot) const;

    Deps deps_;
    // 任务身份（TaskId）、取消令牌、终态都在这里，与诊断页 / QcRunner 同一个
    // TaskManager 协议，只是 slot 不同。
    task::TaskManager tasks_{2};
    std::shared_ptr<AnalysisTask> single_;
    std::shared_ptr<AnalysisTask> batch_;
};

} // namespace ui
} // namespace videoeye
