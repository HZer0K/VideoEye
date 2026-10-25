#include "ui/reporting_panel/ReportingTaskCoordinator.h"

#include <optional>
#include <utility>

#include <QMetaObject>

#include "core/domain/task/ThreadBody.h"

#include "infrastructure/logging/Logger.h"

namespace videoeye {
namespace ui {

namespace {
constexpr const char* kTaskSlot = "reporting-panel";
} // namespace

ReportingTaskCoordinator::ReportingTaskCoordinator(Deps deps) : deps_(std::move(deps)) {
}

ReportingTaskCoordinator::~ReportingTaskCoordinator() {
    RetireAll();
}

const char* ReportingTaskCoordinator::SlotName(Slot slot) const {
    return slot == Slot::Single ? "单文件" : "批量";
}

std::shared_ptr<AnalysisTask>& ReportingTaskCoordinator::SlotTask(Slot slot) {
    return slot == Slot::Single ? single_ : batch_;
}

bool ReportingTaskCoordinator::IsRunning() const {
    return tasks_.IsRunning(kTaskSlot);
}

void ReportingTaskCoordinator::RequestCancel(Slot slot) {
    const std::shared_ptr<AnalysisTask>& task = SlotTask(slot);
    if (task)
        task->cancel.RequestCancel();
}

std::shared_ptr<AnalysisTask>
ReportingTaskCoordinator::Start(Slot slot, const std::function<void(const std::shared_ptr<AnalysisTask>&)>& body) {
    // 先回收同槽位的旧任务：对仍 joinable 的 std::thread 直接赋值会 std::terminate
    // （连续分析两次的老根因）。busy_ 在外面已拦住重复启动，这里是兜底。
    Retire(SlotTask(slot));

    // 任务身份与取消令牌一并向协议要：id 由 slot 分配（"第几次分析"不再自己另起
    // 一套），取消只有 handle.cancel 这一颗，任务体与 QcRunner 轮询的是同一颗。
    const task::TaskHandle handle = tasks_.BeginHandle(kTaskSlot, 0, task::TaskKind::Cooperative);
    if (!handle.valid())
        return nullptr; // 并发满：调用方提示，别静默不跑

    auto task = std::make_shared<AnalysisTask>();
    task->handle = handle;
    task->cancel = handle.cancel;
    task->body_done.store(false, std::memory_order_release);
    SlotTask(slot) = task;

    // body_done 在**包裹层**置位而不是塞进任务体的每条 return 路径：RunSingle /
    // RunBatch 里有提前返回（空目录之类），靠函数末尾那一行总会漏，回收就变成干等
    // 满预算。异常同样必须就地收口 —— 逃逸出去的异常直接 std::terminate，且
    // body_done 永不置位会让严格 Cooperative 的回收路径 join 到天荒地老。
    task->thread = std::thread([this, task, body]() {
        const std::optional<std::string> failure = task::GuardBody([&]() { body(task); });
        task->body_done.store(true, std::memory_order_release);
        if (failure)
            ReportThreadFailure(task, *failure);
    });
    return task;
}

void ReportingTaskCoordinator::Retire(std::shared_ptr<AnalysisTask>& task) {
    if (!task)
        return;
    // 先要一份句柄副本：下面的 RecycleTask 会把 task 置空，句柄得先拿出来。
    const task::TaskHandle handle = task->handle;

    // 运行时诊断（评审 P2-4 方案 A）：报告页走严格 Cooperative，**析构没有硬时间上限**。
    // 这条日志把"开始回收、正在等任务体响应取消"这件事留痕 —— 万一关停久等，日志能
    // 区分是"任务体在收尾"还是"取消没生效"。
    LOG_INFO(std::string("报告页开始回收任务 #") + std::to_string(handle.id) +
             "（严格 Cooperative：请求取消后等待线程体收尾，无硬时间上限）");

    // 请求取消并封死"还允许往面板上刷结果"这条路。顺序不能反 —— 先取消，任务体
    // 才可能在预算内自己收尾；取消必须做，否则取消按钮点了没反应（这条路径就是
    // 取消按钮 / 析构两条路共用的一份收尾逻辑）。
    task->cancel.RequestCancel();
    task->alive.store(false, std::memory_order_release);

    // 终态只在这里（宿主线程）归还：任务体不能碰 tasks_（它捕获了面板指针，关停时
    // tasks_ 比它先析构，从线程里 EndHandle 就是悬空访问）。于是 slot 上的终态在下
    // 一次启动 / 析构时补记 —— 正常流程下 join 已返回，这里一定是干净的
    // Running -> Canceled/Succeeded。
    if (handle.valid()) {
        tasks_.EndHandle(handle, handle.cancel.IsCanceled() ? task::TaskState::Canceled : task::TaskState::Succeeded);
    }

    // 回收：等线程体返回（观测预算）→ join 收句柄 → 置空。任务体捕获了面板指针并
    // 往面板上排队 UI 更新，detach 之后那些投递会落到已销毁的 QWidget 上；在"硬性
    // 时间上限"与"不悬空访问"之间，报告页明确选后者。
    if (!RecycleTask(task)) {
        LOG_WARN(std::string("报告页任务回收超出预期预算 ") + std::to_string(kDefaultRecycleBudgetMs) +
                 "ms，按严格 Cooperative 继续等待其响应取消（不 detach）");
    }
}

void ReportingTaskCoordinator::RetireAll() {
    Retire(single_);
    Retire(batch_);
}

void ReportingTaskCoordinator::Finish(const std::shared_ptr<AnalysisTask>& task, task::TaskState terminal) {
    if (!task)
        return;
    // 终态一次性写入：EndHandle 只在 id 仍与 slot 当前任务匹配时才生效，否则静默丢弃
    // （说明这个任务已被取代 / 句柄已失效）。写完之后 slot 不再停在 Running。
    tasks_.EndHandle(task->handle, terminal);

    // 归还句柄前必须先把线程收回（join）：任务体按值捕获了 task（shared_ptr），而
    // AnalysisTask 又持有 std::thread —— 二者构成引用环。若这里只置空不 join，环没人
    // 打破，AnalysisTask（连同尚可 join 的 std::thread）会永久泄漏。此时任务体已经
    // 返回、body_done 已置位，join 几乎瞬间返回。
    if (single_ == task) {
        RecycleTask(single_);
    } else if (batch_ == task) {
        RecycleTask(batch_);
    }
}

void ReportingTaskCoordinator::PostToUi(const std::shared_ptr<AnalysisTask>& task,
                                        const std::function<void()>& updater) const {
    // 任务已不再存活（面板正在销毁）时丢弃，避免触碰已销毁的 UI 对象
    if (!task || !task->alive.load(std::memory_order_acquire))
        return;
    if (!deps_.ui)
        return;
    QMetaObject::invokeMethod(deps_.ui, updater, Qt::QueuedConnection);
}

// 线程体抓到异常时的另一半收尾：由**任务线程**调用，UI 那一半经 PostToUi 排队回
// 主线程。异常发生在 RunSingle / RunBatch 里的话，正常路径那句清算 UI 的 PostToUi
// 根本到不了（它在函数末尾），于是 busy_ 会一直卡住、按钮全灰、转圈不停 —— 所以
// 这里必须把同一份收尾补齐。终态按统一语义判定（取消优先于异常）。
void ReportingTaskCoordinator::ReportThreadFailure(const std::shared_ptr<AnalysisTask>& task,
                                                   const std::string& message) {
    LOG_ERROR("报告页任务线程抛出异常: " + message);
    const task::TaskState terminal = task::ResolveTerminal(std::optional<std::string>(message), task->cancel);
    PostToUi(task, [this, task, terminal, msg = QString::fromStdString(message)]() {
        // 翻译上下文随之为 QObject（项目当前没有 .ts 文件，见 batch_result_model.h 的
        // 同类说明）：这是把口径搬出 QWidget 之后唯一的变化。
        const QString text = QObject::tr("分析异常: %1").arg(msg);
        if (deps_.show_failure)
            deps_.show_failure(text);
        if (deps_.append)
            deps_.append(text);
        if (deps_.set_busy)
            deps_.set_busy(false);
        // 失败就不再把待导出目录留着 —— 免得下一次成功时悄悄导出一份不相干的东西。
        if (deps_.abort_pending_export)
            deps_.abort_pending_export();
        Finish(task, terminal);
    });
}

} // namespace ui
} // namespace videoeye
