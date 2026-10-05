#include "core/analysis/orchestration/AnalysisEngine.h"
// 各分析维度（时间轴 / 字幕 / 时码 / 辅助 / 音频 QC / 码率 GOP / 色彩 HDR）以及它们
// 各自要开的解码器、parser、swr 全在 AnalysisPipeline 里；本文件只负责"什么时候
// 创建、什么时候分发、什么时候收尾"，不直接 include 任何具体分析器。
#include "core/analysis/orchestration/AnalysisPipeline.h"
// 逐包扫描循环（读包 / 三种出口定性 / 扫描事实 / 进度）在 PacketScanLoop 里。
#include "core/analysis/orchestration/PacketScanLoop.h"
// 打开 / 探测 / 中断 / 取消 / 容器级事实 / 流摘要 都在 AnalysisInputSession 里；
// FFmpeg 中断回调本身住在 core/ffmpeg_io/FfmpegInterrupt.h（叶子模块），由会话去 include。
#include "core/analysis/orchestration/AnalysisInputSession.h"
// 扫描跑完之后的结果归并在 AnalysisResultAssembler；流媒体清单那条独立路径在
// StreamingManifestScan。两者都不碰 FFmpeg 上下文，只消费中间态。
#include "core/analysis/orchestration/AnalysisResultAssembler.h"
#include "core/analysis/orchestration/StreamingManifestScan.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "infrastructure/logging/Logger.h"
#include "infrastructure/logging/ScopedTimer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

namespace videoeye {
namespace {

// 进度限频的两个阈值已随逐包扫描一起搬到 PacketScanLoop.cpp。
//
// ---- FFmpeg 中断机制 ----
// AvInterruptState / AvIoInterruptCallback / kOpenTimeoutUs / kProbeTimeoutUs
// 已抽到 core/ffmpeg_io/FfmpegInterrupt.h 供所有分析器共享（见上方 include）。
// 打开/探测阶段对它们的具体用法（含 moov 顺序扫描、MP4 sample table、流摘要）
// 已随 AnalysisInputSession 一起搬走，见 core/analysis/orchestration/AnalysisInputSession.cpp。

// 流媒体清单的扩展名。清单是纯文本，扩展名是唯一的识别手段
// （魔数检测对 "#EXTM3U" / "<MPD" 无能为力）。
bool IsStreamingManifestExtension(const std::string& ext) {
    return ext == "m3u8" || ext == "m3u" || ext == "mpd";
}

// 各分析维度的**创建 / 逐包分发 / 收尾钩子**（含它们各自要开的解码器、parser、swr）
// 已搬到 core/analysis/orchestration/AnalysisPipeline.{h,cpp} ——
// 匿名命名空间里的那三个探测结构也一并跟过去了（它们现在是管道的成员类型）。

// 回报通道（AnalysisCallbacks）与终态写法（MarkFailed / Notify* / ResultSink /
// kNoFfmpegErrorCode）已搬到 core/analysis/orchestration/AnalysisTerminalState.{h,cpp} ——
// 引擎拆开之后每一片都要用，留在某一个 cpp 的匿名命名空间里就只能被抄走一份。

}  // namespace

bool AnalysisEngine::IsCancelledExit(int ret) const {
    // 看 CancelSource()（外部源优先，没有就用引擎自己那颗）而不是只看引擎自己的
    // cancel_requested_: FFmpeg 的中断回调盯的就是 CancelSource() 那一颗，判据必须
    // 跟它一致。以前这里只看引擎自有标志，于是"接了外部取消源的批处理任务被取消"
    // 会走到失败分支，界面弹出"文件可能截断或 IO 错误"——与打开/探测阶段
    // （AnalysisInputSession::IsCancelledExit 一直看的是 cancel_source_）不一致。
    return CancelSource()->load(std::memory_order_acquire) && ret == AVERROR_EXIT;
}

void AnalysisEngine::Cancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

void AnalysisEngine::Reset() {
    cancel_requested_.store(false, std::memory_order_release);
}

bool AnalysisEngine::IsCancelRequested() const {
    return cancel_requested_.load(std::memory_order_acquire);
}

bool AnalysisEngine::CancelRequested() const {
    if (active_cancel_source_ && active_cancel_source_->load(std::memory_order_acquire))
        return true;
    return cancel_requested_.load(std::memory_order_acquire);
}

// 返回可变指针而不是 const: 调用方（FFmpeg 的 AvInterruptState）要往里置位，
// 而"能不能改"这件事由谁能调 Cancel() 决定，跟"这次读的是哪颗标志"是两回事。
// 本方法是 const 的事实不变 —— 它只读自己，不因多返回一个可写引用就获得改自身的权限。
std::atomic<bool>* AnalysisEngine::CancelSource() const {
    if (active_cancel_source_)
        return active_cancel_source_.get();
    return const_cast<std::atomic<bool>*>(&cancel_requested_);
}

void AnalysisEngine::Run(const std::string& file_path, const AnalysisOptions& options,
                         const AnalysisCallbacks& callbacks, model::AnalysisResult* out_result,
                         std::shared_ptr<std::atomic<bool>> cancel_source) {
    VE_PERF("AnalysisEngine::Run");
    // 必须第一件事就挂上: 下面的"流媒体清单分流"会立刻走到另一条路径，
    // 那里读的是同一个 CancelSource()，晚一步挂就等于本次 Run 没接外部取消源。
    CancelSourceScope cancel_scope(*this, std::move(cancel_source));
    model::AnalysisResult result;
    result.file_path = file_path;
    // 声明在 result 之后：析构顺序反过来，回传时 result 仍然活着。
    const ResultSink result_sink(out_result, &result);

    // 提取小写扩展名（供"扩展名与实际容器不符"规则使用）
    {
        const size_t slash = file_path.find_last_of("/\\");
        const size_t dot = file_path.find_last_of('.');
        if (dot != std::string::npos &&
            (slash == std::string::npos || dot > slash)) {
            result.file_extension = file_path.substr(dot + 1);
            std::transform(result.file_extension.begin(), result.file_extension.end(),
                           result.file_extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
    }

    // ---- 流媒体清单分流（必须在 avformat_open_input 之前）----
    // FFmpeg 会把 .m3u8 / .mpd 当成 HLS / DASH 播放列表去发网络请求，
    // 离线分析时既不可控（卡在网络超时），也拿不到有意义的时长/码率序列。
    // 所以这里在打开任何 IO 之前就分流到自研的清单解析器。
    if (options.analyze_streaming_package && IsStreamingManifestExtension(result.file_extension) &&
        file_path.find("://") == std::string::npos) {
        StreamingManifestScan::Run(file_path, options, result, callbacks, CancelSource());
        return;
    }

    // ---- 打开 / 探测 / 容器级事实 / 流摘要（AnalysisInputSession）----
    //
    // 取消源交进去而不是自备一颗: 引擎接了外部令牌时，FFmpeg 的中断回调必须看同一颗，
    // 否则"用户点取消"对 avformat_open_input 是隐形的。
    AnalysisInputSession input(file_path, options, CancelSource());
    const AnalysisInputSession::Outcome outcome = input.Open(result);
    if (outcome == AnalysisInputSession::Outcome::Failed) {
        // 失败终态（含 error_message / scan_error_code）已由会话写进 result，这里只发回调。
        // 文案一律取 result.error_message：另拼一份就会出现"回调说的"和"结果里写的"不一致。
        NotifyFailed(callbacks, result.error_message);
        return;
    }
    if (outcome == AnalysisInputSession::Outcome::Cancelled) {
        NotifyCancelled(callbacks, result);
        return;
    }
    // 会话仍然持有上下文，**关闭的时点由本函数决定**（色彩 HDR 要在关闭前刷一次，
    // 字幕 / 时码的 Finish 要在关闭后跑）。
    AVFormatContext* fmt = input.context();
    // 来自容器头的总时长；流摘要与下面几处"没有流级时长时回退"的场合都要用
    const double file_duration = result.duration_seconds;

    // ---- 分析维度集合（建分析器 / 注册流 / 开解码器）----
    //
    // 六个维度的创建与逐包分发都收在 AnalysisPipeline 里：新增一个维度只需要改那一个
    // 文件，不用再同时改这里的创建段、下面的分发段和最后的收尾段。
    AnalysisPipeline pipeline(options);
    pipeline.Prepare(fmt, result, file_duration);

    // ---- 逐包扫描 ----
    //
    // 循环本身（读包 / 三种出口的定性 / 扫描事实统计 / 限频进度）在 PacketScanLoop 里。
    // 它**不做清理**: AVPacket 由它的析构释放，AVFormatContext 与解码通路仍归本函数 ——
    // 错误出口下"关上下文之前还要不要刷一次 HDR"只有这里知道。
    const double interval =
        (options.sample_interval_seconds > 0.0) ? options.sample_interval_seconds : 1.0;
    ScanBuckets buckets;
    ScanOutcome scan_outcome = ScanOutcome::Complete;
    {
        PacketScanLoop loop(fmt, options, result, callbacks);
        // 取消判定的两个谓词按引擎的策略注入："看哪一颗标志"（外部源优先于引擎自有）
        // 是引擎的事，循环只是借来问一句。
        scan_outcome = loop.Run(pipeline, buckets, interval, [&] { return CancelRequested(); },
                                [&](int ret) { return IsCancelledExit(ret); });
    }
    if (scan_outcome == ScanOutcome::Failed) {
        // 失败终态（含 error_message / scan_error_code）已由循环写进 result，这里只发回调。
        pipeline.ReleaseProbes();
        input.Close();
        NotifyFailed(callbacks, result.error_message);
        return;
    }
    // Complete 与 Cancelled 都往下走收尾：取消时各分析器照样 Finish，界面因此能拿到
    // 已扫到一半的字幕 / 时码 / 音频 QC，而不是一个空壳。终态仍由 scan_status 决定
    // （Assembler::Finish 的 completed 与进度文案都跟着它走）。
    //
    // 顺带消掉了以前的不一致：取消标记在循环开头被轮询到时，原来是 break 出来走收尾；
    // 而 av_read_frame 被中断回调打断时，原来是直接 NotifyCancelled 返回、一个分析器
    // 都不收尾。同样是按了取消，两条路给出的结果不一样。

    // 关闭上下文之前的最后一眼：部分封装的 HDR 元数据要读到包后才补进 codecpar。
    pipeline.BeforeClose(fmt);
    // flush 音频解码器与 swr 缓存里残留的样本（不碰 fmt，因此可以先于关闭执行）。
    pipeline.FlushAudio();
    // 这两个标记必须在 ReleaseProbes 之前读：释放之后 ready 就变 false 了。
    // 放在 FlushAudio 之后而不是之前 —— flush 自己也会喂帧，那里发现的中途改采样率
    // 同样该记一笔（以前是在 flush 之前抓的，flush 期间的变更会被漏掉）。
    const bool audio_decoder_ready = pipeline.audio_decoder_ready();
    const bool audio_rate_changed = pipeline.audio_rate_changed();
    pipeline.ReleaseProbes();
    // 上下文归输入会话所有，关闭也走它（会话析构时同样只会看到空指针）
    input.Close();

    // ---- 结果归并 ----
    //
    // 顺序不是随意的（详见 AnalysisResultAssembler.h）: 桶先算（它给出实测时长，
    // 字幕收尾要拿它兜底），各分析器 Finish 完之后才能发终态。
    AnalysisResultAssembler assembler(result, options);
    const double measured_duration = assembler.FinalizeBuckets(buckets, interval);
    assembler.FinalizeTimeline(pipeline.timeline());
    assembler.FinalizeGopFrames();
    assembler.FinalizeBitrateGop(pipeline.bitrate_gop(), pipeline.video_stream_index());
    assembler.FinalizeAudioQc(pipeline.audio_qc(), pipeline.audio_stream_index(),
                              audio_decoder_ready, audio_rate_changed);
    assembler.FinalizeColorHdr(pipeline.color_hdr(), pipeline.color_video_stream_index());
    assembler.FinalizeSubtitle(pipeline.subtitle(), measured_duration);
    assembler.FinalizeTimecode(pipeline.timecode());
    assembler.FinalizeAuxData(pipeline.aux_data());
    assembler.Finish(callbacks);
}

} // namespace videoeye
