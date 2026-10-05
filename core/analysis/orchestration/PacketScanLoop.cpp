#include "core/analysis/orchestration/PacketScanLoop.h"

#include <algorithm>
#include <cmath>

#include "infrastructure/logging/ScopedTimer.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

namespace videoeye {
namespace {

constexpr int kProgressMinIntervalMs = 100;
constexpr int kProgressMinPackets = 2000;

}  // namespace

PacketScanLoop::PacketScanLoop(AVFormatContext* fmt, const AnalysisOptions& options,
                               model::AnalysisResult& result, const AnalysisCallbacks& callbacks)
    : fmt_(fmt),
      options_(options),
      result_(result),
      callbacks_(callbacks),
      pkt_(av_packet_alloc()),
      last_progress_(std::chrono::steady_clock::now()) {}

PacketScanLoop::~PacketScanLoop() {
    av_packet_free(&pkt_);
}

ScanOutcome PacketScanLoop::Run(AnalysisPipeline& pipeline, ScanBuckets& buckets,
                                double interval_seconds, const CancelPredicate& cancel_requested,
                                const CancelledExitPredicate& cancelled_exit) {
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
            result_.scan_error_code = ret;  // 真 AVERROR 优先，MarkFailed 不会覆盖它
            MarkFailed(result_, "读取数据包失败（文件可能截断或 IO 错误）: " +
                                    std::string(errbuf));
            return ScanOutcome::Failed;
        }

        if (pkt_->stream_index < 0 ||
            pkt_->stream_index >= static_cast<int>(fmt_->nb_streams)) {
            av_packet_unref(pkt_);
            continue;
        }

        AVStream* st = fmt_->streams[pkt_->stream_index];
        const double tb = av_q2d(st->time_base);
        const bool is_video = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        const bool has_pts = (pkt_->pts != AV_NOPTS_VALUE);
        const double ts = has_pts ? static_cast<double>(pkt_->pts) * tb : -1.0;

        // 维度分发：时间轴 / 字幕 / 时码 / 辅助 / 音频 / 关键帧与 GOP / 色彩与 HDR
        pipeline.OnPacket(fmt_, pkt_, result_, packet_index);

        AccountPacket(pkt_, is_video);
        FillBuckets(buckets, interval_seconds, pkt_, is_video, ts);

        if (pkt_->pos > 0) last_pos = pkt_->pos;
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
        digest.frame_count += 1;
        result_.video_packets += 1;
    }
    if (pkt->size > result_.max_packet_bytes) result_.max_packet_bytes = pkt->size;
    if (pkt->dts == AV_NOPTS_VALUE) result_.packets_missing_dts += 1;
}

void PacketScanLoop::FillBuckets(ScanBuckets& buckets, double interval_seconds,
                                 const AVPacket* pkt, bool is_video, double ts) {
    if (ts < 0.0) return;
    const int64_t bucket_index = static_cast<int64_t>(std::floor(ts / interval_seconds));
    ScanBucket& bucket = buckets[bucket_index];
    bucket.total_bytes += pkt->size;
    if (is_video) {
        bucket.video_bytes += pkt->size;
        bucket.video_frames += 1;
    }
}

// 返回是否真的上报了（上报了才更新 last_progress_）
bool PacketScanLoop::ReportProgress(int64_t packet_index, int64_t last_pos, double ts) {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_).count() <
        kProgressMinIntervalMs) {
        return false;
    }
    double percent = 0.0;
    if (result_.file_size_bytes > 0 && last_pos > 0) {
        percent = 100.0 * static_cast<double>(last_pos) /
                  static_cast<double>(result_.file_size_bytes);
    } else if (result_.duration_seconds > 0.0 && ts > 0.0) {
        percent = 100.0 * ts / result_.duration_seconds;
    }
    percent = std::clamp(percent, 0.0, 99.0);
    NotifyProgress(callbacks_, percent, "扫描数据包 " + std::to_string(packet_index) + " 个");
    return true;
}

}  // namespace videoeye
