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
#include <memory>
#include <string>

#include "core/analysis/AnalysisOptions.h"
// 回报通道（AnalysisCallbacks）与终态写法（MarkFailed / Notify* / ResultSink）住在
// AnalysisTerminalState.h：引擎拆成输入会话 / 扫描循环 / 分析管线 / 结果汇编器之后，
// 每一片都要发进度、写失败终态，这些工具必须全项目只有一份。
// 这里仍然 include 它，是为了让"只想跑一次分析"的调用方（QcRunner、单测）不必知道
// 引擎内部的拆分 —— 它们照旧只 include 本文件就能拿到 AnalysisCallbacks。
#include "core/analysis/orchestration/AnalysisTerminalState.h"
#include "core/domain/model/AnalysisResult.h"

namespace videoeye {
namespace analyzer {

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
    // cancel_source: 可选的**外部**取消源，通常是后台任务协议里那颗取消令牌
    // (task::CancelToken::flag())。传入后本次 Run 的所有取消轮询点（逐包扫描、流媒体
    // 清单解析、分片校验）都改看它，引擎自己的 Cancel() 就退化成"没人用"的兜底。
    // 这么留口子是为了让"用户点了取消"这件事在项目里只有**一份**标志：
    // 宿主置位令牌（TaskManager::Cancel），引擎只是它的观察者，不再自备一套。
    // 传 nullptr（默认）时行为与以前完全一致。
    //
    // out_result: 可选的终态回传出参（默认 nullptr = 不回传，保持旧行为）。
    // 失败分支只发 on_failed、**不**走 on_finished，而 scan_status / error_message
    // 又写在结果对象里 —— 上层（批处理、单测）想拿到"失败时到底是个什么状态"
    // 就必须有这条通道，否则只能像 QcRunner 那样在外面凭失败信号自己补一个状态。
    // 取消分支同样会回传（scan_status=Cancelled）。
    void Run(const std::string& file_path, const AnalysisOptions& options,
             const AnalysisCallbacks& callbacks,
             model::AnalysisResult* out_result = nullptr,
             std::shared_ptr<std::atomic<bool>> cancel_source = nullptr);

private:
    // 本次 Run 是否要取消。外部取消源优先于引擎自己的 Cancel()，
    // 两者都看是为了让"没接外部源的旧调用点"继续按原语义工作。
    bool CancelRequested() const;

    // 本次 Run 实际使用的那颗取消标志（供 FFmpeg 中断回调与流媒体解析器复用同一颗），
    // 外部源为空时返回引擎自己的那颗。
    std::atomic<bool>* CancelSource() const;

    struct CancelSourceScope;
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
    // 仅在 Run() 执行期间非空（由 CancelSourceScope 管理），所以这里不需要额外加锁：
    // 读它只发生在正在跑的那一次 Run 的同一个线程里。
    std::shared_ptr<std::atomic<bool>> active_cancel_source_;
};

// 把外部取消源挂到本次 Run 上，出作用域即摘掉。
// 必须做成 RAII: Run() 的失败分支（avformat_open_input 失败、清单解析失败……）都会
// 提前 return，而取消源是要一直跟着的 —— 漏摘一次，下一次 Run 就会误判"被取消了"。
class AnalysisEngine::CancelSourceScope {
public:
    explicit CancelSourceScope(AnalysisEngine& owner, std::shared_ptr<std::atomic<bool>> source)
        : owner_(owner) {
        owner_.active_cancel_source_ = std::move(source);
    }
    CancelSourceScope(const CancelSourceScope&) = delete;
    CancelSourceScope& operator=(const CancelSourceScope&) = delete;
    ~CancelSourceScope() { owner_.active_cancel_source_.reset(); }

private:
    AnalysisEngine& owner_;
};

} // namespace analyzer
} // namespace videoeye
