#pragma once

// 后台任务的明确所有权载体：线程、取消标记、生命周期守卫三者绑定在一起。
//
// 这是 P0 修复（报告页线程生命周期）的核心数据结构。原先对仍 joinable 的
// std::thread 直接赋值会触发 std::terminate（"连续分析两次"的根因）；析构时
// 又把捕获 this 的线程 detach，取消未生效时存在访问已销毁面板的风险。
//
// 现在把"线程 + 取消 + 存活"收进一个由 shared_ptr 管理的对象，并提供 RecycleTask()
// 用于"再次启动前先回收已结束的线程"。该头文件不依赖 Qt，可被单测直接引用。

#include <atomic>
#include <memory>
#include <thread>

namespace videoeye {
namespace ui {

struct AnalysisTask {
    std::thread thread;
    std::atomic<bool> cancelled{false};  // 取消标记（由分析回调读取，独立于面板生命周期）
    std::atomic<bool> alive{true};        // 任务存续期间为 true；析构/回收时置 false 阻止回调触碰 this
};

// 回收已结束的线程（join 一个已退出的线程会立即返回），回收后再赋新线程，
// 避免对仍 joinable 的 std::thread 直接赋值导致 std::terminate（连续分析两次的根因）。
// 入参可为空（默认构造未启动的 task），RecycleTask 对其安全无副作用。
inline void RecycleTask(std::shared_ptr<AnalysisTask>& task) {
    if (task && task->thread.joinable()) task->thread.join();  // 已退出的线程 join 立即返回
    task.reset();
}

}  // namespace ui
}  // namespace videoeye
