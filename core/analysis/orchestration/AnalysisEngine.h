#pragma once

// 全文件分析的执行引擎（**不依赖 QObject**）。
//
// 为什么拆成两个类: 以前"跑分析"和"用 Qt 信号把结果抛回界面"长在同一个类里
// （老名字叫 AnalysisCoordinator，已删除），于是命令行 / 批处理 / 单元测试想跑一次
// 分析，也得先起一个 QObject、接一套 Qt 信号。而 QcRunner 与批量 QC 根本不需要事件循环。
//
// 拆完之后:
//   AnalysisEngine        —— 只负责执行 + 响应取消，通过普通回调回报，可单测
//   core/qt/QtAnalysisController —— 负责线程、generation 与 Qt 信号（UI 用它）

#include <atomic>
#include <functional>
#include <string>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace analyzer {

// 执行过程中的回报通道。全部从**工作线程**调用，实现方自己负责跨线程投递。
struct AnalysisCallbacks {
    // percent: 0..100
    std::function<void(double percent, const std::string& stage)> on_progress;
    // 打开 / 探测 / 读取失败等"没能产出完整结果"的情况
    std::function<void(const std::string& message)> on_failed;
    // completed = 到达终态而非被取消；result 在 Failed 前不会走到 on_finished
    std::function<void(bool completed, const model::AnalysisResult& result)> on_finished;
};

class AnalysisEngine {
public:
    AnalysisEngine() = default;

    // 请求取消当前分析（异步；持有者通常在另一个线程调用）
    void Cancel();

    // 清掉上一次的取消标记，准备跑新的一次分析（atomic 成员让这个类不可赋值，
    // 所以不能像值类型那样整体重建）。
    void Reset();

    bool IsCancelRequested() const;

    // 执行一次全文件分析。同步阻塞，调用方自行决定是否放到后台线程。
    //
    // out_result: 可选的终态回传出参（默认 nullptr = 不回传，保持旧行为）。
    // 失败分支只发 on_failed、**不**走 on_finished，而 scan_status / error_message
    // 又写在结果对象里 —— 上层（批处理、单测）想拿到"失败时到底是个什么状态"
    // 就必须有这条通道，否则只能像 QcRunner 那样在外面凭失败信号自己补一个状态。
    // 取消分支同样会回传（scan_status=Cancelled）。
    void Run(const std::string& file_path, const AnalysisOptions& options,
             const AnalysisCallbacks& callbacks,
             model::AnalysisResult* out_result = nullptr);

private:
    // FFmpeg 的中断回调一旦被触发，open / 探测 / 逐包读三个阶段都会用 AVERROR_EXIT 收场。
    // 判「这一路是用户取消」必须两个条件同时成立：取消标记已被置位、且 FFmpeg 侧确实
    // 以 EXIT 结束。只看 ret 会把「取消前恰好撞上一次真的 IO 错误」误判成取消，
    // 只看标记又会把「刚点取消、FFmpeg 还没来得及响应」的失败误判成完成。
    bool IsCancelledExit(int ret) const;

    // 流媒体清单（.m3u8 / .mpd）分支：只跑自研清单解析，不做 FFmpeg demux。
    // 从 Run 里单独拆出来是因为这条路径完全不碰 avformat ——
    // FFmpeg 会把清单当播放列表去发网络请求，离线 QC 既不可控也无法单测。
    void RunStreamingManifest(const std::string& file_path,
                              const AnalysisOptions& options,
                              model::AnalysisResult& result,
                              const AnalysisCallbacks& callbacks);

    std::atomic<bool> cancel_requested_{false};
};

} // namespace analyzer
} // namespace videoeye
