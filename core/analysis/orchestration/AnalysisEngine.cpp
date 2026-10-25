#include "core/analysis/orchestration/AnalysisEngine.h"
// 各分析维度（时间轴 / 字幕 / 时码 / 辅助 / 音频 QC / 码率 GOP / 色彩 HDR）以及它们
// 各自要开的解码器、parser、swr 全在 AnalysisPipeline 里；本文件只负责"什么时候
// 创建、什么时候分发、什么时候收尾"，不直接 include 任何具体分析器。
#include "core/analysis/orchestration/detail/AnalysisPipeline.h"
// 逐包扫描循环（读包 / 三种出口定性 / 扫描事实 / 进度）在 PacketScanLoop 里。
#include "core/analysis/orchestration/detail/PacketScanLoop.h"
// 打开 / 探测 / 中断 / 取消 / 容器级事实 / 流摘要 都在 AnalysisInputSession 里；
// FFmpeg 中断回调本身住在 core/ffmpeg_io/FfmpegInterrupt.h（叶子模块），由会话去 include。
#include "core/analysis/orchestration/detail/AnalysisInputSession.h"
// 扫描跑完之后的结果归并在 AnalysisResultAssembler；流媒体清单那条独立路径在
// StreamingManifestScan。两者都不碰 FFmpeg 上下文，只消费中间态。
#include "core/analysis/orchestration/StreamingManifestScan.h"
#include "core/analysis/orchestration/detail/AnalysisResultAssembler.h"
// MPEG-TS 传输层体检：QC 规则要的失步/CC/PCR/加扰只能按字节扫 TS 包头拿到，
// 与容器结构页共用同一个解析器（单一事实源），只是扫描量按 QC 场景收窄。
#include "core/analysis/container/TsStructureAnalyzer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
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
// 已随 AnalysisInputSession 一起搬走，见 core/analysis/orchestration/detail/AnalysisInputSession.cpp。

// 流媒体清单的扩展名。清单是纯文本，扩展名是唯一的识别手段
// （魔数检测对 "#EXTM3U" / "<MPD" 无能为力）。
bool IsStreamingManifestExtension(const std::string& ext) {
    return ext == "m3u8" || ext == "m3u" || ext == "mpd";
}

// MPEG-TS 传输层体检，结果写进 result.ts_transport 供 container.ts.* 规则消费。
//
// 为什么必须单独一趟按字节扫：失步 / 连续计数错误 / PCR 间隔 / 加扰都是**TS 包头**
// 里的事实，而 avformat 的 demux 只把 PES 解成 AVPacket —— 逐包扫描看不到包头。
// 解析器与容器结构页共用 TsStructureAnalyzer（单一事实源），只是扫描量按 QC 收窄。
//
// 不做的事：
//   * 网络源不扫（URL 读两遍既不必要也不可靠，读第二遍还可能拿到不同的直播窗口）；
//   * 不是 mpegts 就不扫（其它容器没有 TS 包头可言）；
//   * 失败只记 error_message，不影响主扫描 —— 它是补充证据，不是前置条件。
void FillTsTransport(const std::string& file_path, const AVFormatContext* fmt, const AnalysisOptions& options,
                     const std::atomic<bool>* cancel, model::AnalysisResult& result) {
    if (!options.analyze_ts_transport)
        return;
    if (file_path.find("://") != std::string::npos)
        return;
    const char* iformat_name = (fmt != nullptr && fmt->iformat != nullptr) ? fmt->iformat->name : nullptr;
    if (iformat_name == nullptr || std::string(iformat_name).find("mpegts") == std::string::npos) {
        return;
    }

    TsStructureOptions ts_options;
    // 0 = 扫完整个文件；与容器结构页的默认值（50 万包）分开，批量 QC 用更小的预算。
    ts_options.max_scan_packets = (options.ts_transport_max_packets > 0) ? options.ts_transport_max_packets : 0;

    model::ContainerStructureResult container;
    TsStructureAnalyzer analyzer;
    if (!analyzer.Analyze(file_path, container, cancel, ts_options)) {
        result.ts_transport = model::TransportStreamInfo();
        // 走到这里说明输入确实是 mpegts（上面的 iformat 判断已放行），只是没扫出结论。
        // applicable 必须留真：否则报告层会把它和"这是 MP4，不该扫"混为一谈，
        // 于是"TS 没拿到传输层结论"这件最该说清楚的事反而消失了。
        result.ts_transport.applicable = true;
        result.ts_transport.error_message = container.error_message;
        LOG_INFO("TS 传输层体检未完成: " + container.error_message);
        return;
    }
    result.ts_transport = container.transport;
    result.ts_transport.applicable = true;
    LOG_INFO("TS 传输层体检: " + std::to_string(result.ts_transport.scanned_packets) + "/" +
             std::to_string(result.ts_transport.estimated_packets) +
             " 包, 失步=" + std::to_string(result.ts_transport.sync_loss) +
             ", CC 错=" + std::to_string(result.ts_transport.continuity_errors) +
             ", PCR 最大间隔=" + std::to_string(static_cast<int>(result.ts_transport.pcr_max_gap_ms)) + "ms");
    // 载荷级另记一行：规则未命中时这是唯一能回答"到底扫了没有"的线索。
    LOG_INFO(std::string("TS 载荷级: ") +
             (result.ts_transport.pes_payload_scanned
                  ? ("ES " + std::to_string(result.ts_transport.pes_streams_checked) + " 条, IDR " +
                     std::to_string(result.ts_transport.idr_count) + " 个, 最长无 IDR " +
                     std::to_string(static_cast<int>(result.ts_transport.max_no_idr_interval_ms)) +
                     "ms, 最大 PES 断档 " + std::to_string(static_cast<int>(result.ts_transport.max_pid_gap_ms)) + "ms")
                  : std::string("未执行")));
}

// 各分析维度的**创建 / 逐包分发 / 收尾钩子**（含它们各自要开的解码器、parser、swr）
// 已搬到 core/analysis/orchestration/detail/AnalysisPipeline.{h,cpp} ——
// 匿名命名空间里的那三个探测结构也一并跟过去了（它们现在是管道的成员类型）。

// 回报通道（AnalysisCallbacks）与终态写法（MarkFailed / Notify* / ResultSink /
// kNoFfmpegErrorCode）已搬到 core/analysis/orchestration/AnalysisTerminalState.{h,cpp} ——
// 引擎拆开之后每一片都要用，留在某一个 cpp 的匿名命名空间里就只能被抄走一份。

} // namespace

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
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            result.file_extension = file_path.substr(dot + 1);
            std::transform(result.file_extension.begin(), result.file_extension.end(), result.file_extension.begin(),
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

    // ---- MPEG-TS 传输层体检 ----
    // 放在 Prepare 之前：它只读文件头、自开 ifstream，不碰 fmt 的读取状态；
    // 越早跑完越不会和后面的逐包扫描抢 IO。取消令牌与扫描共用同一颗。
    FillTsTransport(file_path, fmt, options, CancelSource(), result);

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
    const double interval = (options.sample_interval_seconds > 0.0) ? options.sample_interval_seconds : 1.0;
    ScanBuckets buckets;
    ScanOutcome scan_outcome = ScanOutcome::Complete;
    {
        PacketScanLoop loop(fmt, options, result, callbacks);
        // 取消判定的两个谓词按引擎的策略注入："看哪一颗标志"（外部源优先于引擎自有）
        // 是引擎的事，循环只是借来问一句。
        scan_outcome = loop.Run(
            pipeline, buckets, interval, [&] { return CancelRequested(); },
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
    pipeline.BeforeClose(fmt, result);
    // flush 音频解码器与 swr 缓存里残留的样本（不碰 fmt，因此可以先于关闭执行）。
    pipeline.FlushAudio();
    // flush 帧类型探测解码器：EOF 时仍压在解码器里的帧（重排序延迟）在此收干净，
    // 同样不碰 fmt。必须在 ReleaseProbes 之前。
    pipeline.FlushFrameProbe();
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
    // 逐轨音频 QC：把 Pipeline 里每条音轨的"分析器 + 通路状态"打包成只读视图交给 Assembler。
    // 主轨在 audio_tracks()[0]（Pipeline 保证顺序），Assembler 靠 primary_audio_stream_index 认它。
    std::vector<AudioQcTrackInput> audio_inputs;
    audio_inputs.reserve(pipeline.audio_tracks().size());
    for (AudioQcTrack& track : pipeline.audio_tracks()) {
        AudioQcTrackInput input;
        input.stream_index = track.stream_index;
        // analyzer 是 Pipeline 的值成员，生命周期长于 assembler；这里只借用，不转移所有权。
        input.analyzer = &track.analyzer;
        input.decoder_ready = audio_decoder_ready;
        input.rate_changed = audio_rate_changed;
        audio_inputs.push_back(input);
    }
    assembler.FinalizeAudioQc(audio_inputs, pipeline.audio_stream_index(), pipeline.audio_tracks_total());
    assembler.FinalizeAudioBitstream(pipeline.audio_bitstream(), pipeline.audio_stream_index());
    // 参数变化时间线：直接搬（Assembler 不加工），必须在 BeforeClose 之后 —— 那时 tracker 已 Finish
    assembler.FinalizeParameterTimelines(pipeline.parameter_timelines());
    assembler.FinalizeFrameOrder(pipeline.frame_order_timelines());
    assembler.FinalizeColorHdr(pipeline.color_hdr(), pipeline.color_video_stream_index());
    assembler.FinalizeSubtitle(pipeline.subtitle(), measured_duration);
    assembler.FinalizeTimecode(pipeline.timecode());
    assembler.FinalizeAuxData(pipeline.aux_data());
    // 证据与覆盖状态必须最后算：它要读上面所有 Finalize 写好的 result 字段
    // （audio_qc / audio_bitstream / color_hdr / ts_transport ...），顺序前置会全部读到默认值。
    assembler.FinalizeDimensionStatus(static_cast<int>(audio_inputs.size()), pipeline.audio_tracks_total());
    assembler.Finish(callbacks);
}

} // namespace videoeye
