#pragma once

// 全文件分析的**逐包扫描循环**: av_read_frame 的驱动、三种出口的定性、扫描事实的
// 统计（包数 / 字节 / 逐秒桶）、进度上报。
//
// 为什么单独成类: 拆完 AnalysisPipeline 之后，Run() 里剩下的一段仍然同时做四件事 ——
// 读包、判 EOF/取消/错误、统计扫描事实、限频上报进度。这四件事只跟"循环"有关，
// 跟"分析什么"无关，混在一起就让"改进度策略"和"加分析维度"互相干扰。
//
// 出口的语义各不相同，调用方不要合并处理:
//   Complete  —— 扫到 EOF，结果可用（scan_status 可能已因 max_packets 被置 Sampled）
//   Cancelled —— 取消生效；终态已置 Cancelled，调用方只需发 NotifyCancelled
//   Failed    —— 真的 IO 错误；失败终态（error_message / scan_error_code）已写进
//                result，调用方只需发 NotifyFailed(result.error_message)
//
// 本类**不做清理**: AVPacket 由析构释放，AVFormatContext 与解码通路仍归外部
// （AnalysisInputSession / AnalysisPipeline）—— 错误出口下由调用方决定关与释放的
// 顺序，因为"关上下文前还要不要刷一次 HDR"只有调用方知道。

#include <chrono>
#include <cstdint>
#include <functional>

#include "core/analysis/AnalysisOptions.h"
#include "core/analysis/orchestration/AnalysisPipeline.h"
#include "core/analysis/orchestration/AnalysisResultAssembler.h"  // ScanBuckets
#include "core/analysis/orchestration/AnalysisTerminalState.h"
#include "core/domain/model/AnalysisResult.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace analyzer {

enum class ScanOutcome {
    Complete,
    Cancelled,
    Failed,
};

class PacketScanLoop {
public:
    // 取消判定的两个谓词由调用方注入，本类不持有取消标志:
    //   cancel_requested —— 每轮循环开头轮询一次（外部令牌与引擎自有标志取或）
    //   cancelled_exit   —— av_read_frame 返回负值时的定性（取消 vs 真 IO 错误）
    // 分开传而不是自备一颗标志：标志归引擎所有，"看哪一颗"是引擎的策略
    // （外部取消源优先），本类只是借来问一句。
    using CancelPredicate = std::function<bool()>;
    using CancelledExitPredicate = std::function<bool(int)>;

    PacketScanLoop(AVFormatContext* fmt, const AnalysisOptions& options,
                   model::AnalysisResult& result, const AnalysisCallbacks& callbacks);
    ~PacketScanLoop();

    PacketScanLoop(const PacketScanLoop&) = delete;
    PacketScanLoop& operator=(const PacketScanLoop&) = delete;

    // 跑完整循环。buckets 是出参（交给 AnalysisResultAssembler 换算码率序列），
    // interval_seconds 是分桶间隔（秒）。进度按包数与时间双重限频后直发回调。
    ScanOutcome Run(AnalysisPipeline& pipeline, ScanBuckets& buckets, double interval_seconds,
                    const CancelPredicate& cancel_requested,
                    const CancelledExitPredicate& cancelled_exit);

private:
    // 扫描事实：包数 / 字节 / 逐流累加 / 最大包 / 缺 DTS 计数
    void AccountPacket(AVPacket* pkt, bool is_video);
    // 逐秒桶：总字节记所有包，视频字节与帧数只记视频包
    void FillBuckets(ScanBuckets& buckets, double interval_seconds, const AVPacket* pkt,
                     bool is_video, double ts);
    bool ReportProgress(int64_t packet_index, int64_t last_pos, double ts);

    AVFormatContext* fmt_;
    const AnalysisOptions& options_;
    model::AnalysisResult& result_;
    const AnalysisCallbacks& callbacks_;

    AVPacket* pkt_;
    std::chrono::steady_clock::time_point last_progress_;
};

}  // namespace analyzer
}  // namespace videoeye
