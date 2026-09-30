#pragma once

// 码率 / GOP 分析的结果类型。
//
// 下放到 domain 的原因与 StreamStats 一样: 它们是不含任何 FFmpeg / Qt 的纯值对象,
// 但消费者横跨 analysis(产生)、reporting(导出)和 ui(画曲线/表格)。留在
// BitrateGopAnalyzer.h 里的话, AnalysisResult.h 为了声明一个字段就得把整个
// 分析器实现拖进编译图。
//
// 注意 BitrateGopAnalysis::SceneKeyMatch 是内嵌结构 —— 它只在本结果里有意义,
// 不下放。

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/BitratePoint.h"
#include "core/domain/model/GopInfo.h"
#include "core/domain/model/MetricSeries.h"

namespace videoeye {
namespace model {

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

    // 是否因超过 max_samples 而做过有界降采样（曲线为近似）
    bool downsampled = false;
};

}  // namespace model
}  // namespace videoeye
