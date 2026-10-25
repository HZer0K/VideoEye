#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/BitrateGopResult.h"
#include "core/domain/model/BitratePoint.h"
#include "core/domain/model/GopInfo.h"
#include "core/domain/model/MetricSeries.h"
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
    // 一个语义帧的事件描述（OnFramesClaimPending 批量认领字节用）。
    // 只带"帧自己知道的事" —— 字节由待认领池分配，调用方不该、也没法猜。
    struct FrameEvent {
        double timestamp_seconds = 0.0;
        model::FrameType type = model::FrameType::Unknown;
        bool is_idr = false;
    };

    void Reset(const BitrateGopOptions& options = BitrateGopOptions{});
    void SetOptions(const BitrateGopOptions& options);
    const BitrateGopOptions& options() const {
        return options_;
    }

    // ---- 数据输入（显式事件，每个语义单元只进一次统计）----
    //
    // 字节有两套账，调用方（AnalysisPipeline）必须分清自己在喂哪一种事实：
    //   * 包级账（total_bytes / 码率曲线）—— 字节按**到达时间**入账，与帧无关。
    //   * 帧级账（frame_bytes / GOP 大小 / 大帧判定）—— 字节按**归属帧**入账。
    //
    // 四个入口：
    //   * OnFrame      —— 一个语义帧 + 该帧的字节，字节**尚未**入账（一包一帧、
    //                     parser 输出、合成样本）。两套账各记一次，数额相同。
    //   * OnPacketBytes —— 只有字节没有帧。awaiting_frame=true 时字节进"待认领
    //                     池"，等后续 OnFramesClaimPending 按帧数分摊认领；
    //                     false 表示这些字节永远不对应任何帧（裸流分片残留）。
    //   * OnFramesClaimPending —— 解码器产出的一批帧来认领待认领池里的字节。
    //                     这是"解码延迟"场景唯一正确的关联方式：包先入账、
    //                     帧晚几包才出来，帧的字节只能从池子里摊，不能拿"本次
    //                     送入包的字节"顶替（那样 GOP 字节之和会远小于总字节，
    //                     I 帧大小也跟着错）。
    //   * OnPacket     —— 无帧类型探测时的包级回退：一包记一帧，类型未知，
    //                     is_key 来自 AV_PKT_FLAG_KEY。
    // 曾经这里只有 OnFrame/OnPacket 两个入口，调用方在解码循环内外各调一次
    // OnFrame（fed 标志二次调用），帧数/字节/GOP 全部重复计数 —— 事件化之后
    // "重复"在结构上不再可能。

    // 帧级输入（codec parser 或解码器拿到 pict_type 后使用）
    void OnFrame(int stream_index, double timestamp_seconds, int64_t size, model::FrameType type, bool is_idr);

    // 纯字节事件：不计帧、不计 GOP，只入账字节并参与码率曲线。
    // awaiting_frame=true 时字节额外进待认领池（见 OnFramesClaimPending）。
    void OnPacketBytes(int stream_index, double timestamp_seconds, int64_t size, bool awaiting_frame = false);

    // 解码器产出的一批帧：把待认领池里的字节按帧数均分认领（余数给最后一帧）。
    // 池为空、或多个帧分摊同一池时，帧字节是估算 —— 会置 frame_bytes_estimated，
    // 不能可靠关联就必须明说，不能假装精确。
    void OnFramesClaimPending(int stream_index, const std::vector<FrameEvent>& frames);

    // 包级输入（离线全文件扫描，帧类型未知时传 Unknown；is_key 来自 AV_PKT_FLAG_KEY）
    void OnPacket(int stream_index, double timestamp_seconds, int64_t size, bool is_key);

    // 关联场景切换结果（来自 SceneChangeAnalyzer::Feed 的输出）
    void AssociateSceneChanges(const std::vector<SceneChangeResult>& changes);

    // 对"已经 Finish 过"的结果补充/刷新场景切换关联。
    // 场景切换通常来自播放过程中的逐帧检测，晚于全文件扫描，因此 UI 需要在拿到
    // 切换点后单独调用一次；不会重算码率与 GOP 统计。
    static void ApplySceneChanges(BitrateGopAnalysis& result, const std::vector<SceneChangeResult>& changes,
                                  const BitrateGopOptions& options);
    // 按当前结果重新生成建议列表（阈值或场景关联变化后调用）
    static void RebuildSuggestions(BitrateGopAnalysis& result, const BitrateGopOptions& options);

    // ---- 结果 ----
    const BitrateGopAnalysis& Finish();
    const BitrateGopAnalysis& result() const {
        return result_;
    }
    bool finished() const {
        return finished_;
    }

    // 便捷查询
    const std::vector<model::MetricSeries>& window_curves() const {
        return result_.window_curves;
    }
    // 取指定窗口长度的曲线（找不到返回 nullptr）
    const model::MetricSeries* WindowCurve(double window_seconds) const;

private:
    // 曲线采样点。降采样时相邻两点会被**合并**成桶（字节相加），所以每个字段
    // 都可能是"多个原始事件的合计"。
    struct Sample {
        double timestamp_seconds = 0.0;
        // 进入码率曲线 / total_bytes 的字节数（包级事实）
        int64_t size = 0;
        // 归属到帧的字节数（帧级事实；纯字节事件为 0）
        int64_t frame_bytes = 0;
        // 本桶代表几个语义帧（合并后可能 > 1；纯字节事件为 0）
        int frame_weight = 0;
        model::FrameType type = model::FrameType::Unknown;
        bool is_idr = false;
        // false = 纯字节事件（OnPacketBytes）：只参与码率曲线，不计帧数/GOP/
        // 帧大小异常。true = 语义帧（OnFrame / OnPacket / 认领池的帧）。
        bool is_frame = true;
        // 帧字节是估算（从待认领池摊出来的），不能当精确值用
        bool bytes_estimated = false;
    };

    // stream_bytes 进包级账（total_bytes + 曲线），frame_bytes 进帧级账
    // （frame_bytes + GOP 大小 + 大帧判定）。认领池场景下 stream_bytes 传 0 ——
    // 那些字节早就按包到达时间入过账了，再记一次就是重复计数。
    void AddSample(int stream_index, double timestamp_seconds, int64_t stream_bytes, int64_t frame_bytes,
                   model::FrameType type, bool is_idr, bool counts_frame, bool bytes_estimated);
    void StartGop(double timestamp_seconds, int stream_index, bool closed);
    void CloseCurrentGop(double end_seconds, bool complete);

    // 超过 max_samples 时的有界降采样：相邻两点合并成桶，**字节相加**。
    // 直接丢一半样本（老做法）会让前缀和与帧间隔都缩水 —— 20 帧输入在
    // max_samples=4 下窗口字节数从 2000 掉到 200、估计时长从 2 秒涨到 3.6 秒。
    void DownsampleSamples();
    // 帧间隔单独维护（只看语义帧），不受降采样影响
    double EstimatedFrameInterval() const;

    // 计算所有窗口长度的滑动窗口码率曲线
    void BuildBitrateCurves();
    // 由默认窗口曲线统计峰值/均值/分位数并标记 overshoot
    void SummarizeBitrate();
    void DetectGopAnomalies();
    void DetectPeakOvershoot();
    void DetectOversizedFrames();
    void DetectSparseKeyframes();

    // 场景切换关联与建议生成也支持对已有结果调用（见 ApplySceneChanges / RebuildSuggestions）
    static void MatchSceneChanges(BitrateGopAnalysis& result, const std::vector<SceneChangeResult>& changes,
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

    // 待认领字节池：送进解码器（或 parser）但还没有帧来认领的字节。
    // OnPacketBytes(awaiting_frame=true) 进池，OnFramesClaimPending 出池，
    // Finish() 时池里剩下的就是"没能关联到任何帧"的字节。
    int64_t pending_bytes_ = 0;

    // 时长与帧间隔**单独维护**：samples_ 会被降采样合并，样本数不再等于帧数，
    // 拿"samples_.size()"反推帧间隔会把时长算错（见 DownsampleSamples 注释）。
    double first_timestamp_ = -1.0;      // 首个事件（含纯字节）的时间
    double last_event_timestamp_ = -1.0; // 末个事件的时间
    double frame_first_timestamp_ = -1.0;
    double frame_last_timestamp_ = -1.0;
    int64_t frame_event_count_ = 0;
};

} // namespace videoeye
