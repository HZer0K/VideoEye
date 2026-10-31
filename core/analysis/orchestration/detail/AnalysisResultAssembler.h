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
#include <vector>

#include "core/domain/model/AnalysisOptions.h"
// 具体分析器的头只出现在这里: 本类的工作就是把它们的中间态读出来写进结果，
// 它们是本类的**职责**而不是实现细节。
#include "core/analysis/diagnostics/AuxDataAnalyzer.h"
#include "core/analysis/diagnostics/SubtitleAnalyzer.h"
#include "core/analysis/diagnostics/TimecodeAnalyzer.h"
#include "core/analysis/diagnostics/TimelineAnalyzer.h"
#include "core/analysis/orchestration/AnalysisTerminalState.h"
#include "core/analysis/quality/AudioBitstreamAnalyzer.h"
#include "core/analysis/quality/AudioQcAnalyzer.h"
#include "core/analysis/quality/BitrateGopAnalyzer.h"
#include "core/analysis/quality/ColorHdrAnalyzer.h"
#include "core/analysis/quality/VisualDefectAnalyzer.h"
#include "core/domain/model/AnalysisResult.h"

namespace videoeye {

// 逐秒桶: 扫描循环按时间戳把包大小 / 视频包数攒到这里，收尾时换算成码率与包率序列。
// 放在本头文件而不是扫描循环那边的匿名命名空间里 —— 它同时被"生产者"（逐包扫描）
// 和"消费者"（本类）使用，藏在任何一边都会逼另一方抄一份定义。
struct ScanBucket {
    int64_t total_bytes = 0;
    int64_t video_bytes = 0;
    // 主视频轨的**包数**，不是帧数：demux 层一个包可能含多帧，也不保证一帧一包。
    // 消费方（video_fps 曲线与 fps 规则）必须按"包率估算"来解读。
    int64_t video_packets = 0;
};

// 逐秒桶集合。
//
// 桶数上限存在的理由：map 按时间桶持续增长，24 小时 / 1 秒粒度会攒 86400 个桶，
// 而消费方（三条逐秒曲线）最多只画 kMaxCurvePoints 个点 —— 保留更多没有意义，
// 却让后端内存随素材时长无界增长。达到上限后**不再新增桶**，后续包不计入逐秒
// 统计，truncated 标记"曲线只覆盖前一段"。
struct ScanBuckets {
    static constexpr size_t kMaxBuckets = 20000;
    std::map<int64_t, ScanBucket> by_second;
    // 见过的最后一个桶下标（含因上限被丢弃的）。时长兜底必须用它：
    // 截断后用 rbegin() 会把素材时长算短。
    int64_t last_bucket_index = -1;
    bool truncated = false;

    // 取/建桶。达到上限后不再新增、置 truncated 并返回 nullptr。
    // 抽成方法是为了能脱离 FFmpeg 上下文单独验证上限语义。
    ScanBucket* Acquire(int64_t index) {
        if (index > last_bucket_index)
            last_bucket_index = index;
        auto it = by_second.find(index);
        if (it != by_second.end())
            return &it->second;
        if (by_second.size() >= kMaxBuckets) {
            truncated = true;
            return nullptr;
        }
        return &by_second.emplace(index, ScanBucket{}).first->second;
    }
};

// 一条参与 PCM QC 的音轨的收尾输入。
//
// 为什么不直接传 AudioQcTrack：那个类型住在 AnalysisPipeline.h（它持有解码器、
// swr 等 FFmpeg 资源），而本类的职责边界是"只碰结果对象 + 分析器"。
// 用这个轻量视图隔开，Assembler 就不必知道解码通路的任何细节。
struct AudioQcTrackInput {
    int stream_index = -1;
    AudioQcAnalyzer* analyzer = nullptr;
    bool decoder_ready = false;
    bool rate_changed = false;
};

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
    // 逐轨收尾：tracks 的首元素应是主音轨（Prepare 的建法保证了这一点）。
    // result.audio_qc 取主轨那一份（向后兼容），全量写 audio_qc_streams。
    void FinalizeAudioQc(const std::vector<AudioQcTrackInput>& tracks, int primary_audio_stream_index,
                         int audio_tracks_total);
    // 音频位流参数（D1）：与 FinalizeAudioQc 无先后依赖，两者读的是不同数据源
    // （头 vs PCM）。放在音频 QC 之后只是为了日志顺序可读。
    void FinalizeAudioBitstream(AudioBitstreamAnalyzer& audio_bitstream, int audio_stream_index);
    // 参数变化时间线：直接搬进结果（Assembler 不做二次加工）
    void FinalizeParameterTimelines(const std::vector<model::ParameterTimeline>& timelines);
    // 帧级显示/解码顺序（POC）。空 = 没有可分析的 H.264 轨（维度会标不可用）。
    void FinalizeFrameOrder(const std::vector<model::FrameOrderTimeline>& timelines);
    // 必须在 color_probe 释放之后调用（部分 HDR 元数据只在解码帧上出现）
    void FinalizeColorHdr(ColorHdrAnalyzer& color_hdr, int color_video_stream_index);
    // 视觉画质 QC：必须在 FlushVisual 之后调用（EOF 残余帧已喂进分析器、缺陷段已闭合）。
    // decoded_frames 是本次实际解码的视频帧数（维度覆盖率的"分母"）。
    void FinalizeVisualQc(VisualDefectAnalyzer& visual_qc, int video_stream_index, int64_t decoded_frames);
    // 必须在 avformat_close_input 之后调用
    void FinalizeSubtitle(SubtitleAnalyzer& subtitle_analyzer, double measured_duration);
    void FinalizeTimecode(TimecodeAnalyzer& timecode_analyzer);
    void FinalizeAuxData(AuxDataAnalyzer& aux_analyzer);

    // 汇总各维度的"证据与覆盖"到 result.dimensions。
    //
    // 必须在所有 Finalize* 之后调用：它读的是已经落进 result 的各维度结论，
    // 提前调用会得到"未执行"的错误判断。
    //
    // 逐维度的判据在 .cpp 顶部的 Build*Status()（一个维度一个函数），本类的这个成员只
    // 负责顺序与落表：清表 -> 构造 -> MarkConclusive + SetDimension -> 回填历史布尔。
    // 加维度要改两处：写一个 Build*Status，再在 BuildAllDimensions 里按报告章节顺序插
    // 一行（有 test_dimension_aggregation.cpp 的用例同时守"每个维度都有判据"与顺序）。
    void FinalizeDimensionStatus(int audio_tracks_analyzed, int audio_tracks_total);

    // 收尾: 记录已扫包数 -> 进度 100% -> on_finished。
    // completed 由 scan_status 决定: 取消路径也会走到这里（它 break 出循环后仍要跑完
    // 各分析器的收尾），所以进度文案不能写死"分析完成"。
    void Finish(const AnalysisCallbacks& callbacks);

private:
    // 用 dimensions 表回填 7 个历史 *_analyzed 布尔，让"跑没跑"只有一份答案。
    //
    // 生产阶段（逐包扫描 / 各分析器收尾）必须继续写这些布尔 —— 扫描循环要边跑边判断；
    // 但到 FinalizeDimensionStatus 这里，维度的 executed 才是权威口径：一旦某维度被判定为
    // "选项关闭 / 输入不适用 / 没拿到数据"，对应布尔也必须跟着为 false，不能让它继续为 true
    // 把"未执行"骗成"已执行"。消费方不要直接读这些布尔，用 AnalysisResult::DimensionExecuted()。
    void SyncLegacyDimensionFlags();

    model::AnalysisResult& result_;
    const AnalysisOptions& options_;
    // 桶算出来的实测时长（FinalizeDimensionStatus 用它做覆盖分母）
    double measured_duration_ = 0.0;
};

} // namespace videoeye
