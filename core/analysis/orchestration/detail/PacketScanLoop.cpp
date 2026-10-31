#include "core/analysis/orchestration/detail/PacketScanLoop.h"

#include <algorithm>
#include <cmath>

#include "core/analysis/detail/AnalysisStreamSelection.h"
#include "infrastructure/logging/ScopedTimer.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

namespace videoeye {
namespace {

constexpr int kProgressMinIntervalMs = 100;
constexpr int kProgressMinPackets = 2000;

} // namespace

PacketScanLoop::PacketScanLoop(AVFormatContext* fmt, const AnalysisOptions& options, model::AnalysisResult& result,
                               const AnalysisCallbacks& callbacks, ffmpeg_io::AvInterruptState* interrupt,
                               int64_t idle_timeout_us)
    : fmt_(fmt), options_(options), result_(result), callbacks_(callbacks), pkt_(av_packet_alloc()),
      interrupt_(interrupt), idle_timeout_us_(idle_timeout_us), last_progress_(std::chrono::steady_clock::now()) {
}

PacketScanLoop::~PacketScanLoop() {
    av_packet_free(&pkt_);
}

ScanOutcome PacketScanLoop::Run(AnalysisPipeline& pipeline, ScanBuckets& buckets, double interval_seconds,
                                const CancelPredicate& cancel_requested, const CancelledExitPredicate& cancelled_exit) {
    if (pkt_ == nullptr) {
        result_.scan_error_code = AVERROR(ENOMEM);
        MarkFailed(result_, "无法分配数据包结构");
        return ScanOutcome::Failed;
    }

    int64_t packet_index = 0;
    int64_t last_pos = 0;

    NotifyProgress(callbacks_, 0.0, "扫描数据包");

    VE_PERF("逐包扫描(全文件 demux + 音频解码 + GOP)");
    while (true) {
        if (cancel_requested()) {
            result_.scan_status = model::AnalysisStatus::Cancelled;
            return ScanOutcome::Cancelled;
        }
        // 网络源：每次读包前重新武装空闲截止时间（上一次成功读取之后重新计时）。
        // 本地输入 idle_timeout_us_ == 0，这里什么都不做。
        if (interrupt_ != nullptr && idle_timeout_us_ > 0)
            interrupt_->deadline_us.store(av_gettime() + idle_timeout_us_, std::memory_order_relaxed);
        const int ret = av_read_frame(fmt_, pkt_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                result_.scan_status = model::AnalysisStatus::Complete;
                return ScanOutcome::Complete;
            }
            // 读取数据包阶段出现错误（文件截断 / IO 错误 / 网络中断）。
            // 这与"完整扫到 EOF"不同：必须作为失败处理，不能把半成品当完整 QC 报告。
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            // 取消标记的轮询在循环开头，但 av_read_frame 可能在两次轮询之间
            // 就被中断回调打断（网络/磁盘 IO 里根本轮不到那次检查），
            // 这种"取消 + AVERROR_EXIT"必须报成 Cancelled 而不是半成品失败。
            if (cancelled_exit(ret)) {
                result_.scan_status = model::AnalysisStatus::Cancelled;
                return ScanOutcome::Cancelled;
            }
            // 中断回调只可能因为"取消"或"截止时间到期"返回 1：取消已在上面处理，
            // 所以这里的 AVERROR_EXIT 就是本循环武装的空闲超时到期。
            // 必须报成失败而不是半个结果 —— 网络断流后的部分扫描不能当完整 QC 用。
            if (ret == AVERROR_EXIT && idle_timeout_us_ > 0) {
                result_.scan_error_code = AVERROR(ETIMEDOUT);
                MarkFailed(result_, "读取数据包超过 " + std::to_string(idle_timeout_us_ / 1000) +
                                        " ms 没有新数据（输入 IO 空闲超时）");
                return ScanOutcome::Failed;
            }
            result_.scan_error_code = ret; // 真 AVERROR 优先，MarkFailed 不会覆盖它
            MarkFailed(result_, "读取数据包失败（文件可能截断或 IO 错误）: " + std::string(errbuf));
            return ScanOutcome::Failed;
        }

        if (pkt_->stream_index < 0 || pkt_->stream_index >= static_cast<int>(fmt_->nb_streams)) {
            av_packet_unref(pkt_);
            continue;
        }

        AVStream* st = fmt_->streams[pkt_->stream_index];
        const double tb = av_q2d(st->time_base);
        const bool is_video = IsAnalysisVideoStream(st);
        const bool has_pts = (pkt_->pts != AV_NOPTS_VALUE);
        const double ts = has_pts ? static_cast<double>(pkt_->pts) * tb : -1.0;

        // 维度分发：时间轴 / 字幕 / 时码 / 辅助 / 音频 / 关键帧与 GOP / 色彩与 HDR
        pipeline.OnPacket(fmt_, pkt_, result_, packet_index);

        AccountPacket(pkt_, is_video);
        FillBuckets(buckets, interval_seconds, pkt_, is_video, ts);

        if (pkt_->pos > 0)
            last_pos = pkt_->pos;
        av_packet_unref(pkt_);

        // 进度上报（限频）
        ++packet_index;
        if (options_.max_packets > 0 && packet_index >= options_.max_packets) {
            // 命中包数上限：只完成了抽样扫描，不是完整结果。
            if (result_.scan_status == model::AnalysisStatus::Complete) {
                result_.scan_status = model::AnalysisStatus::Sampled;
            }
            return ScanOutcome::Complete;
        }
        if (packet_index % kProgressMinPackets == 0) {
            if (ReportProgress(packet_index, last_pos, ts)) {
                last_progress_ = std::chrono::steady_clock::now();
            }
        }
    }
}

void PacketScanLoop::AccountPacket(AVPacket* pkt, bool is_video) {
    model::StreamDigest& digest = result_.streams[pkt->stream_index];
    result_.total_packets += 1;
    result_.total_bytes += pkt->size;
    digest.packet_count += 1;
    digest.byte_count += pkt->size;
    if (is_video) {
        // 注意：这里是**视频包数**，不是精确帧数（demux 层没有逐帧事件）。
        // 保留该字段给"每轨包数"消费方，真正的帧级统计在 BitrateGopAnalyzer。
        digest.frame_count += 1;
        result_.video_packets += 1;
    }
    if (pkt->size > result_.max_packet_bytes)
        result_.max_packet_bytes = pkt->size;
    if (pkt->dts == AV_NOPTS_VALUE)
        result_.packets_missing_dts += 1;
}

void PacketScanLoop::FillBuckets(ScanBuckets& buckets, double interval_seconds, const AVPacket* pkt, bool is_video,
                                 double ts) {
    if (ts < 0.0)
        return;
    const int64_t bucket_index = static_cast<int64_t>(std::floor(ts / interval_seconds));
    // 桶数上限：超出后不再新增，仅标记截断（last_bucket_index 仍在 Acquire 里更新，
    // 时长兜底因此不受影响）。已经存在的桶继续累加，前段曲线仍是完整统计。
    ScanBucket* slot = buckets.Acquire(bucket_index);
    if (slot == nullptr)
        return;
    slot->total_bytes += pkt->size;
    if (!is_video)
        return;

    // 只累计**主视频轨**的字节与包数。以前所有视频轨都往同一个桶里加：
    // 多节目/多角度文件会让 video_fps 翻倍，而这条曲线又被"逐秒帧率标准差"规则消费。
    // 注意这里统计的是**包数**，不是帧数（一个包可能含多帧，也不保证一帧一包）。
    int primary_video = result_.primary_video_stream_index;
    if (primary_video < 0) {
        // 主轨未标（旧结果/直接构造的测试）：退化为第一条非封面视频轨。
        for (const auto& digest : result_.streams) {
            if (digest.IsVideo() && !digest.attached_picture) {
                primary_video = digest.index;
                break;
            }
        }
    }
    if (primary_video >= 0 && pkt->stream_index != primary_video)
        return;
    slot->video_bytes += pkt->size;
    slot->video_packets += 1;
}

// 返回是否真的上报了（上报了才更新 last_progress_）
bool PacketScanLoop::ReportProgress(int64_t packet_index, int64_t last_pos, double ts) {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_).count() < kProgressMinIntervalMs) {
        return false;
    }
    double percent = 0.0;
    if (result_.file_size_bytes > 0 && last_pos > 0) {
        percent = 100.0 * static_cast<double>(last_pos) / static_cast<double>(result_.file_size_bytes);
    } else if (result_.duration_seconds > 0.0 && ts > 0.0) {
        percent = 100.0 * ts / result_.duration_seconds;
    }
    percent = std::clamp(percent, 0.0, 99.0);
    NotifyProgress(callbacks_, percent, "扫描数据包 " + std::to_string(packet_index) + " 个");
    return true;
}

} // namespace videoeye
