#pragma once

// 全文件分析的**结果归并**: 逐包扫描跑完之后，把所有分析器的中间态收进
// model::AnalysisResult，并给出终态（进度 100% + on_finished）。
//
// 为什么单独成类: 以前这段是 Run() 的最后一百多行，于是"往结果里加一个维度"要同时
// 改创建、逐包分发、**收尾**三处，而收尾这处的写法（顺序、要不要兜底、要不要打日志、
// 状态字段怎么置）全靠读那 100 行才能搞清楚。收进来之后，"结果里最终有什么"这件事
// 只有本文件一个答案。
//
// 顺序不是随意的，搬动时不要重排:
//   * 桶 -> 序列（measured_duration 由桶算出，后面字幕收尾要用它兜底）；
//   * 解码器 / swr 释放之后才能收尾色彩与码率（它们要用到最后几帧的 side data）；
//   * 各分析器 Finish() 之后才能发终态信号（信号一发出去，UI 就会去读结果）。
//
// 本类只碰"结果对象 + 分析器"，不碰 FFmpeg 上下文、不碰线程、不决定取消 ——
// 那些仍归 AnalysisEngine。

#include <cstdint>
#include <map>

#include "core/analysis/AnalysisOptions.h"
// 具体分析器的头只出现在这里: 本类的工作就是把它们的中间态读出来写进结果，
// 它们是本类的**职责**而不是实现细节。
#include "core/analysis/diagnostics/AuxDataAnalyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"
#include "core/analysis/diagnostics/TimecodeAnalyzer.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/analysis/orchestration/AnalysisTerminalState.h"
#include "core/analysis/quality/AudioQcAnalyzer.h"
#include "core/analysis/quality/BitrateGopAnalyzer.h"
#include "core/analysis/quality/ColorHdrAnalyzer.h"
#include "core/domain/model/AnalysisResult.h"

namespace videoeye {

// 逐秒桶: 扫描循环按时间戳把包大小 / 帧数攒到这里，收尾时换算成码率与帧率序列。
// 放在本头文件而不是扫描循环那边的匿名命名空间里 —— 它同时被"生产者"（逐包扫描）
// 和"消费者"（本类）使用，藏在任何一边都会逼另一方抄一份定义。
struct ScanBucket {
    int64_t total_bytes = 0;
    int64_t video_bytes = 0;
    int64_t video_frames = 0;
};

using ScanBuckets = std::map<int64_t, ScanBucket>;

class AnalysisResultAssembler {
public:
    AnalysisResultAssembler(model::AnalysisResult& result, const AnalysisOptions& options);

    // 桶 -> 平均包大小 / 总码率兜底 / 逐流码率兜底 / 三条时间序列。
    // 返回**实测时长**: 容器没给时长时用桶估算，供字幕收尾兜底（否则"超出媒体时长"
    // 这类判定会因为没有参照而全漏）。
    double FinalizeBuckets(const ScanBuckets& buckets, double interval_seconds);

    // 时间轴与同步（demux 层 packet 时间 / decode 层 frame 时间）
    void FinalizeTimeline(TimelineAnalyzer& timeline_analyzer);

    // 逐包扫描攒下的 GOP 帧数统计: 没有逐帧信息时用帧大小序列兜底 max_gop_frames
    void FinalizeGopFrames();

    void FinalizeBitrateGop(BitrateGopAnalyzer& bitrate_gop, int video_stream_index);
    void FinalizeAudioQc(AudioQcAnalyzer& audio_qc, int audio_stream_index,
                         bool audio_decoder_ready, bool audio_rate_changed);
    // 必须在 color_probe 释放之后调用（部分 HDR 元数据只在解码帧上出现）
    void FinalizeColorHdr(ColorHdrAnalyzer& color_hdr, int color_video_stream_index);
    // 必须在 avformat_close_input 之后调用
    void FinalizeSubtitle(SubtitleAnalyzer& subtitle_analyzer, double measured_duration);
    void FinalizeTimecode(TimecodeAnalyzer& timecode_analyzer);
    void FinalizeAuxData(AuxDataAnalyzer& aux_analyzer);

    // 收尾: 记录已扫包数 -> 进度 100% -> on_finished。
    // completed 由 scan_status 决定: 取消路径也会走到这里（它 break 出循环后仍要跑完
    // 各分析器的收尾），所以进度文案不能写死"分析完成"。
    void Finish(const AnalysisCallbacks& callbacks);

private:
    model::AnalysisResult& result_;
    const AnalysisOptions& options_;
};

}  // namespace videoeye
