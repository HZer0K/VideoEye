#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/analyzer/SceneChangeAnalyzer.h"
#include "core/model/BitratePoint.h"
#include "core/model/GopInfo.h"
#include "core/model/MetricSeries.h"

namespace videoeye {
namespace analyzer {

// 码率与 GOP 深度分析的配置项
//
// 所有阈值都集中在这里，便于 UI 暴露成可编辑项；分析器本身不内置魔法数字。
struct BitrateGopOptions {
    // 滑动窗口长度（秒）。第一项为默认窗口，UI 下拉框直接遍历此列表。
    // 0.5 / 1 / 2 / 5 秒是码率分析的常用档位。
    std::vector<double> windows_seconds = {1.0, 0.5, 2.0, 5.0};

    // 目标峰值码率（kbps）。<=0 时自动取 平均码率 * auto_peak_ratio。
    double target_peak_kbps = 0.0;
    double auto_peak_ratio = 2.0;

    // GOP 时长上限（秒）/ 帧数上限。任一超过即记为超长 GOP。
    double max_gop_seconds = 10.0;
    int max_gop_frames = 300;

    // 关键帧间隔（秒）标准差/均值上限，超过记为间隔不均匀
    double gop_irregular_ratio = 0.6;

    // 帧大小 > 全片平均帧大小 * large_frame_ratio → 异常大帧
    double large_frame_ratio = 8.0;

    // I 帧大小 > 平均 I 帧大小 * i_frame_oversize_ratio → I 帧过大
    double i_frame_oversize_ratio = 3.0;

    // 场景切换点前后多少秒内没有关键帧 → 判定"场景切换后缺少关键帧"
    double scene_key_tolerance_seconds = 0.5;
    // 低于该强度的切换点不参与关联（噪声过滤）
    double scene_score_threshold = 0.45;

    // 单类型异常的最大保留条数（防止长视频刷屏）
    int max_anomalies_per_type = 200;
};

// 异常类型
enum class BitrateAnomalyType {
    LongGop,                     // GOP 时长或帧数超过阈值
    IrregularKeyInterval,        // 关键帧间隔不均匀
    PeakOvershoot,               // 瞬时码率超过目标峰值
    OversizedFrame,              // 异常大帧
    OversizedIFrame,             // I 帧过大
    SceneChangeWithoutKeyframe,  // 场景切换附近没有关键帧
    SparseKeyframes,             // 关键帧密度过低（seek 困难）
};

const char* ToString(BitrateAnomalyType type);

// 一条异常记录（UI 表格 + 图表标记的数据源）
struct BitrateAnomaly {
    BitrateAnomalyType type = BitrateAnomalyType::LongGop;
    double start_seconds = 0.0;  // 异常起始时刻
    double end_seconds = 0.0;    // 异常结束时刻（点状异常与 start 相同）
    double value = 0.0;          // 实测值（见 unit）
    double threshold = 0.0;      // 触发阈值（与 value 同单位）
    std::string unit;            // value/threshold 的单位：s / 帧 / kbps / B / 个
    int gop_index = -1;          // 关联的 GOP 序号（-1 = 无）
    std::string detail;          // 人类可读描述
    std::string suggestion;      // 优化建议
};

// 码率与 GOP 分析结果
struct BitrateGopAnalysis {
    // ---- 码率 ----
    double duration_seconds = 0.0;
    int64_t total_bytes = 0;
    int64_t total_frames = 0;
    double avg_bitrate_kbps = 0.0;
    double peak_bitrate_kbps = 0.0;
    double min_bitrate_kbps = 0.0;
    double median_bitrate_kbps = 0.0;
    double p95_bitrate_kbps = 0.0;
    double target_peak_kbps = 0.0;
    double peak_to_mean_ratio = 0.0;   // 峰值/均值，衡量 VBR 波动
    double window_seconds = 1.0;       // 下列曲线使用的窗口长度

    // 默认窗口的采样点（含 overshoot 标记，UI 折线图 + 异常峰值标记用）
    std::vector<model::BitratePoint> bitrate_points;
    // 各窗口长度的曲线（与 options.windows_seconds 一一对应），供切换窗口时直接取用
    std::vector<model::MetricSeries> window_curves;

    // ---- 帧类型 ----
    int64_t i_frame_count = 0;
    int64_t p_frame_count = 0;
    int64_t b_frame_count = 0;
    int64_t unknown_frame_count = 0;
    int64_t i_frame_bytes_ = 0;      // I 帧字节数累计（用于计算平均 I 帧大小）
    bool frame_types_known = false;  // false = 只有 I 帧（来自关键帧标记）可信

    double IFrameRatio() const;
    double PFrameRatio() const;
    double BFrameRatio() const;
    double AverageFrameBytes() const;
    double AverageIFrameBytes() const;

    // ---- GOP ----
    std::vector<model::GopInfo> gops;
    double gop_duration_mean = 0.0;
    double gop_duration_stddev = 0.0;
    double gop_duration_max = 0.0;
    int gop_frames_min = 0;
    int gop_frames_max = 0;
    double gop_frames_mean = 0.0;
    double key_interval_mean = 0.0;
    double key_interval_stddev = 0.0;
    double key_interval_irregularity = 0.0;  // stddev / mean
    int long_gop_count = 0;
    int closed_gop_count = 0;
    int open_gop_count = 0;

    // ---- 异常与建议 ----
    std::vector<BitrateAnomaly> anomalies;
    std::vector<std::string> suggestions;

    int CountAnomalies(BitrateAnomalyType type) const;
    std::vector<const BitrateAnomaly*> AnomaliesOf(BitrateAnomalyType type) const;

    // I 帧时间戳（用于折线图叠加 I 帧标记）
    std::vector<double> i_frame_seconds;
    // 关键帧（IDR/CRA）时间戳
    std::vector<double> key_frame_seconds;
    // 参与关联的场景切换点及该点附近是否有关键帧
    struct SceneKeyMatch {
        double timestamp_seconds = 0.0;
        double score = 0.0;
        bool has_nearby_keyframe = false;
        double nearest_keyframe_distance = -1.0;
    };
    std::vector<SceneKeyMatch> scene_matches;

    std::string ToString() const;
};

// 码率与 GOP 深度分析器
//
// 职责:
//   1) 以包/帧为单位喂数据（时间戳 + 大小 + 帧类型 + 是否关键帧），维护 GOP 边界
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

    // ---- 数据输入 ----

    // 包级输入（离线全文件扫描，帧类型未知时传 Unknown；is_key 来自 AV_PKT_FLAG_KEY）
    void OnPacket(int stream_index, double timestamp_seconds, int64_t size, bool is_key);

    // 帧级输入（codec parser 或解码器拿到 pict_type 后使用）
    void OnFrame(int stream_index, double timestamp_seconds, int64_t size,
                 model::FrameType type, bool is_idr);

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
    };

    void AddSample(int stream_index, double timestamp_seconds, int64_t size,
                   model::FrameType type, bool is_idr);
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

}  // namespace analyzer
}  // namespace videoeye
