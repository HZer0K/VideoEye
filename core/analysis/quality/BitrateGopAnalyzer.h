#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/BitratePoint.h"
#include "core/domain/model/GopInfo.h"
#include "core/domain/model/MetricSeries.h"
#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/BitrateGopResult.h"
#include "core/domain/model/SceneChangeResult.h"

namespace videoeye {

// 码率/GOP 的结果类型（BitrateAnomalyType / BitrateAnomaly /
// BitrateGopAnalysis）与 SceneChangeResult 都已下放到 domain ——
// AnalysisResult 和 UI 只想读结果，不该为此拖着分析器实现。
// 别名留着，免得既有调用方改几十行。
using model::BitrateAnomaly;
using model::BitrateAnomalyType;
using model::BitrateGopAnalysis;
using model::SceneChangeResult;

// 码率与 GOP 深度分析器
//
// 职责:
//   1) 以事件为单位喂数据（时间戳 + 大小 + 帧类型 + 是否关键帧），维护 GOP 边界
//   2) Finish() 时一次性计算多窗口滑动码率、I/P/B 比例、GOP 分布与异常
//   3) 可与 SceneChangeAnalyzer 的结果关联，产出"场景切换后缺少关键帧"类编码优化建议
//
// 刻意不依赖 FFmpeg / Qt: 时间戳由调用方换算为秒, 帧类型用 model::FrameType。
// 这样单测可以直接喂合成数据跑验收（见 tests/unit/test_bitrate_gop_analyzer.cpp）。
class BitrateGopAnalyzer {
public:
    void Reset(const BitrateGopOptions& options = BitrateGopOptions{});
    void SetOptions(const BitrateGopOptions& options);
    const BitrateGopOptions& options() const { return options_; }

    // ---- 数据输入（显式事件，每个语义单元只进一次统计）----
    //
    // 输入分三种互斥事件，调用方（AnalysisPipeline）负责把 demux/解码/parser 的
    // 事实映射到其中之一：
    //   * OnFrame      —— 一个**语义帧**（解码器输出或 parser 输出），一包产出
    //                     多帧时每帧各调一次；size 是归属到该帧的字节数
    //                     （一包一帧时传整包大小，一包多帧时由调用方均分）。
    //   * OnPacketBytes —— 只有字节、没有帧：解码器已收下本包但帧还没产出
    //                     （帧延迟），或 parser 未吐出完整帧。字节先入账，
    //                     帧等真正产出时再计，避免"按包猜一帧"与后续真实帧重复。
    //   * OnPacket     —— 无帧类型探测时的包级回退：一包记一帧，类型未知，
    //                     is_key 来自 AV_PKT_FLAG_KEY。
    // 曾经这里只有 OnFrame/OnPacket 两个入口，调用方在解码循环内外各调一次
    // OnFrame（fed 标志二次调用），帧数/字节/GOP 全部重复计数 —— 事件化之后
    // "重复"在结构上不再可能。

    // 帧级输入（codec parser 或解码器拿到 pict_type 后使用）
    void OnFrame(int stream_index, double timestamp_seconds, int64_t size,
                 model::FrameType type, bool is_idr);

    // 纯字节事件：不计帧、不计 GOP，只入账字节并参与码率曲线
    void OnPacketBytes(int stream_index, double timestamp_seconds, int64_t size);

    // 包级输入（离线全文件扫描，帧类型未知时传 Unknown；is_key 来自 AV_PKT_FLAG_KEY）
    void OnPacket(int stream_index, double timestamp_seconds, int64_t size, bool is_key);

    // 关联场景切换结果（来自 SceneChangeAnalyzer::Feed 的输出）
    void AssociateSceneChanges(const std::vector<SceneChangeResult>& changes);

    // 对"已经 Finish 过"的结果补充/刷新场景切换关联。
    // 场景切换通常来自播放过程中的逐帧检测，晚于全文件扫描，因此 UI 需要在拿到
    // 切换点后单独调用一次；不会重算码率与 GOP 统计。
    static void ApplySceneChanges(BitrateGopAnalysis& result,
                                 const std::vector<SceneChangeResult>& changes,
                                 const BitrateGopOptions& options);
    // 按当前结果重新生成建议列表（阈值或场景关联变化后调用）
    static void RebuildSuggestions(BitrateGopAnalysis& result, const BitrateGopOptions& options);

    // ---- 结果 ----
    const BitrateGopAnalysis& Finish();
    const BitrateGopAnalysis& result() const { return result_; }
    bool finished() const { return finished_; }

    // 便捷查询
    const std::vector<model::MetricSeries>& window_curves() const { return result_.window_curves; }
    // 取指定窗口长度的曲线（找不到返回 nullptr）
    const model::MetricSeries* WindowCurve(double window_seconds) const;

private:
    struct Sample {
        double timestamp_seconds = 0.0;
        int64_t size = 0;
        model::FrameType type = model::FrameType::Unknown;
        bool is_idr = false;
        // false = 纯字节事件（OnPacketBytes）：只参与码率曲线，不计帧数/GOP/
        // 帧大小异常。true = 语义帧（OnFrame / OnPacket）。
        bool is_frame = true;
    };

    // counts_frame=false 时只入账字节（total_bytes + 曲线样本），不碰帧数与 GOP。
    void AddSample(int stream_index, double timestamp_seconds, int64_t size,
                   model::FrameType type, bool is_idr, bool counts_frame);
    void StartGop(double timestamp_seconds, int stream_index, bool closed);
    void CloseCurrentGop(double end_seconds, bool complete);

    // 计算所有窗口长度的滑动窗口码率曲线
    void BuildBitrateCurves();
    // 由默认窗口曲线统计峰值/均值/分位数并标记 overshoot
    void SummarizeBitrate();
    void DetectGopAnomalies();
    void DetectPeakOvershoot();
    void DetectOversizedFrames();
    void DetectSparseKeyframes();

    // 场景切换关联与建议生成也支持对已有结果调用（见 ApplySceneChanges / RebuildSuggestions）
    static void MatchSceneChanges(BitrateGopAnalysis& result,
                                  const std::vector<SceneChangeResult>& changes,
                                  const BitrateGopOptions& options);

    BitrateGopOptions options_;
    BitrateGopAnalysis result_;
    std::vector<Sample> samples_;
    std::vector<SceneChangeResult> scene_changes_;

    model::GopInfo current_gop_;
    bool gop_open_ = false;
    int video_stream_index_ = -1;
    double last_timestamp_ = -1.0;
    bool finished_ = false;
};

}  // namespace videoeye
