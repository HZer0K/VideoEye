#include "core/analysis/orchestration/AnalysisEngine.h"
// 各分析维度（时间轴 / 字幕 / 时码 / 辅助 / 音频 QC / 码率 GOP / 色彩 HDR）以及它们
// 各自要开的解码器、parser、swr 全在 AnalysisPipeline 里；本文件只负责"什么时候
// 创建、什么时候分发、什么时候收尾"，不直接 include 任何具体分析器。
#include "core/analysis/orchestration/AnalysisPipeline.h"
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
namespace analyzer {
namespace {

constexpr int kProgressMinIntervalMs = 100;
constexpr int kProgressMinPackets = 2000;
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
    return IsCancelRequested() && ret == AVERROR_EXIT;
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
    const double interval = (options.sample_interval_seconds > 0.0) ? options.sample_interval_seconds : 1.0;
    ScanBuckets buckets;

    AVPacket* pkt = av_packet_alloc();
    int64_t packet_index = 0;
    int64_t last_pos = 0;

    auto last_progress = std::chrono::steady_clock::now();
    NotifyProgress(callbacks, 0.0, "扫描数据包");

    // 逐包扫描的统一收尾：取消和真·IO 错误都要走同一份清理，
    // 免得以后改一处漏一处（漏 avformat_close_input 就是句柄泄漏）。
    //
    // 关上下文必须走 input.Close()：会话自己也持着这枚指针，直接 avformat_close_input(&fmt)
    // 只会清掉这里的局部变量，会话析构时会对已释放的上下文再关一次。
    auto teardown_scan = [&]() {
        av_packet_free(&pkt);
        input.Close();
        fmt = nullptr;
        // 解码器 / parser / swr 归管道所有（各 probe 的 Release 自带幂等，
        // 正常路径已经释放过一次，这里再调一次是空操作）。
        pipeline.ReleaseProbes();
    };

    {
    VE_PERF("逐包扫描(全文件 demux + 音频解码 + GOP)");
    while (true) {
        if (CancelRequested()) {
            result.scan_status = model::AnalysisStatus::Cancelled;
            break;
        }
        const int ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                result.scan_status = model::AnalysisStatus::Complete;
            } else {
                // 读取数据包阶段出现错误（文件截断 / IO 错误 / 网络中断）。
                // 这与"完整扫到 EOF"不同：必须作为失败处理，不能把半成品当完整 QC 报告。
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
                // 取消标记的轮询在循环开头，但 av_read_frame 可能在两次轮询之间
                // 就被中断回调打断（网络/磁盘 IO 里根本轮不到那次检查），
                // 这种"取消 + AVERROR_EXIT"必须报成 Cancelled 而不是半成品失败。
                if (IsCancelledExit(ret)) {
                    teardown_scan();
                    NotifyCancelled(callbacks, result);
                    return;
                }
                result.scan_error_code = ret;  // 真 AVERROR 优先，MarkFailed 不会覆盖它
                MarkFailed(result, "读取数据包失败（文件可能截断或 IO 错误）: " +
                                       std::string(errbuf));
                teardown_scan();
                NotifyFailed(callbacks, result.error_message);
                return;
            }
            break;  // EOF
        }

        if (pkt->stream_index < 0 ||
            pkt->stream_index >= static_cast<int>(fmt->nb_streams)) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* st = fmt->streams[pkt->stream_index];
        model::StreamDigest& digest = result.streams[pkt->stream_index];
        const double tb = av_q2d(st->time_base);
        const bool is_video = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        const bool has_pts = (pkt->pts != AV_NOPTS_VALUE);
        const double ts = has_pts ? static_cast<double>(pkt->pts) * tb : -1.0;

        // 维度分发：时间轴 / 字幕 / 时码 / 辅助 / 音频 / 关键帧与 GOP / 色彩与 HDR。
        // 只喂 pkt —— 时间戳、逐秒桶与下面这些计数是"扫描事实"而不是"维度"，
        // 所以留在本函数里（A4 拆 PacketScanLoop 时它们跟着循环一起走）。
        pipeline.OnPacket(fmt, pkt, result, packet_index);

        result.total_packets += 1;
        result.total_bytes += pkt->size;
        digest.packet_count += 1;
        digest.byte_count += pkt->size;
        if (is_video) digest.frame_count += 1;
        if (pkt->size > result.max_packet_bytes) result.max_packet_bytes = pkt->size;
        if (pkt->dts == AV_NOPTS_VALUE) result.packets_missing_dts += 1;
        if (is_video) result.video_packets += 1;

        // 逐秒桶：码率 + 帧率（视频字节/帧数只记视频包，总字节记所有包）
        if (ts >= 0.0) {
            const int64_t bucket_index = static_cast<int64_t>(std::floor(ts / interval));
            ScanBucket& bucket = buckets[bucket_index];
            bucket.total_bytes += pkt->size;
            if (is_video) {
                bucket.video_bytes += pkt->size;
                bucket.video_frames += 1;
            }
        }

        last_pos = (pkt->pos > 0) ? pkt->pos : last_pos;
        av_packet_unref(pkt);

        // 进度上报（限频）
        ++packet_index;
        if (options.max_packets > 0 && packet_index >= options.max_packets) {
            // 命中包数上限：只完成了抽样扫描，不是完整结果。
            if (result.scan_status == model::AnalysisStatus::Complete) {
                result.scan_status = model::AnalysisStatus::Sampled;
            }
            break;
        }
        if (packet_index % kProgressMinPackets == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress).count() >=
                kProgressMinIntervalMs) {
                last_progress = now;
                double percent = 0.0;
                if (result.file_size_bytes > 0 && last_pos > 0) {
                    percent = 100.0 * static_cast<double>(last_pos) /
                              static_cast<double>(result.file_size_bytes);
                } else if (result.duration_seconds > 0.0 && ts > 0.0) {
                    percent = 100.0 * ts / result.duration_seconds;
                }
                percent = std::clamp(percent, 0.0, 99.0);
                NotifyProgress(callbacks, percent,
                                      "扫描数据包 " + std::to_string(packet_index) + " 个");
            }
        }
    }
    }
    av_packet_free(&pkt);

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

} // namespace analyzer
} // namespace videoeye
