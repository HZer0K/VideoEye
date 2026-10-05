#pragma once

// 全文件分析的回报通道与终态写法。
//
// 为什么从 AnalysisEngine.cpp 里搬出来: 引擎要拆成「输入会话 / 扫描循环 / 分析管线 /
// 结果汇编器」几片之后，每一片都要发进度、写失败终态、回传结果对象，而这些事的正确性
// 恰恰依赖于"三件事必须一起写"（见 MarkFailed 的注释）。把它们留在某一个 cpp 的匿名
// 命名空间里，等于逼着其余几片各自抄一份 —— 抄的那份迟早会漏掉一半。
//
// 本文件是**唯一**允许写失败终态、发终态回调的地方。

#include <functional>
#include <string>

#include "core/domain/model/AnalysisResult.h"

namespace videoeye {

// 执行过程中的回报通道。全部从**工作线程**调用，实现方自己负责跨线程投递。
struct AnalysisCallbacks {
    // percent: 0..100
    std::function<void(double percent, const std::string& stage)> on_progress;
    // 打开 / 探测 / 读取失败等"没能产出完整结果"的情况
    std::function<void(const std::string& message)> on_failed;
    // completed = 到达终态而非被取消；result 在 Failed 前不会走到 on_finished
    std::function<void(bool completed, const model::AnalysisResult& result)> on_finished;
};

// AnalysisResult::scan_error_code 的约定是"0 = 无；否则为 FFmpeg 错误码或 -1"。
// 清单解析 / 分片校验这类自研路径没有 FFmpeg 错误码，失败时记这个值 ——
// 保持"失败 => scan_error_code != 0"，同时不冒充某个具体的 AVERROR。
constexpr int kNoFfmpegErrorCode = -1;

// 回调是可选的（批处理可能不关心进度），逐个判空再调用。
void NotifyProgress(const AnalysisCallbacks& callbacks, double percent, const std::string& stage);
void NotifyFailed(const AnalysisCallbacks& callbacks, const std::string& message);
void NotifyFinished(const AnalysisCallbacks& callbacks, bool completed,
                    const model::AnalysisResult& result);

// 取消收尾：与 NotifyFailed 互斥的两条终态之一（另一条是 NotifyFinished(completed=true)）。
// 以前取消信号是从「失败」分支里漏出去的，界面就会弹"文件损坏"这类文案 ——
// 可用户明明是自己按的取消。现在统一走这里。
void NotifyCancelled(const AnalysisCallbacks& callbacks, model::AnalysisResult& result);

// 失败终态的统一写法：状态、文案、错误码三件事必须一起落进结果对象。
//
// AnalysisResult::scan_status 的默认值是 Complete，只调 NotifyFailed 而不同步改它，
// 就会留下"回调说失败、结果对象却说完成"的不一致 —— 结果一旦被上层缓存 / 复用
// （批处理回传、对比视图、报告导出），就会拿一份空的流媒体包当成跑完的结果展示。
//
// scan_error_code 只在还没有值时才补 kNoFfmpegErrorCode：逐包扫描那条路已经记了
// 真实的 AVERROR，不能被覆盖掉。
void MarkFailed(model::AnalysisResult& result, const std::string& message);

// 终态回传守卫：Run() 的失败/取消分支只发回调、不走 on_finished，调用方想拿到
// 结果对象里的终态（scan_status / error_message）只能靠出参。用析构兜住所有
// return 路径，免得以后每加一条失败分支就漏一处赋值。
class ResultSink {
public:
    ResultSink(model::AnalysisResult* out, const model::AnalysisResult* src) noexcept
        : out_(out), src_(src) {}

    ~ResultSink() {
        if (out_ != nullptr) *out_ = *src_;
    }

    ResultSink(const ResultSink&) = delete;
    ResultSink& operator=(const ResultSink&) = delete;

private:
    model::AnalysisResult* out_;
    const model::AnalysisResult* src_;
};

}  // namespace videoeye
