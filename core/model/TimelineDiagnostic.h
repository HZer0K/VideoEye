#pragma once

#include <string>
#include <vector>

#include "core/model/DiagnosticIssue.h"
#include "core/model/MetricSeries.h"

namespace videoeye {
namespace model {

// 时间轴 / 同步问题类型
enum class TimelineIssueType {
    PtsNonMonotonic,             // PTS 非单调（回退）
    DtsNonMonotonic,             // DTS 非单调
    DtsAfterPts,                 // DTS 晚于 PTS（B 帧重排异常）
    AudioVideoStartOffset,       // 音视频首帧偏移过大
    AudioVideoDurationMismatch,  // 音视频总时长不一致
    FrameIntervalSpike,          // 帧间隔突刺（丢帧/卡顿）
    DuplicatedTimestamp,         // 重复时间戳（重复帧）
    MissingKeyframeIndex         // 缺少关键帧索引（无法 seek / 关键帧缺失）
};

const char* ToString(TimelineIssueType type);

// 单条时间轴诊断
struct TimelineDiagnostic {
    TimelineIssueType type = TimelineIssueType::PtsNonMonotonic;
    IssueSeverity severity = IssueSeverity::Warning;
    int stream_index = -1;
    double timestamp_ms = 0.0;     // 问题发生位置
    double metric_value_ms = 0.0;  // 实测值（ms 或 ms 差值）
    double threshold_ms = 0.0;     // 触发阈值
    int occurrence_count = 1;      // 同类问题出现次数（合并统计）
    std::string detail;
    std::string suggestion;

    std::string TypeText() const { return ToString(type); }
    std::string SeverityText() const { return ::videoeye::model::ToString(severity); }
};

// 时间轴分析汇总结果
struct TimelineAnalysisResult {
    // 流定位
    int video_stream_index = -1;
    int audio_stream_index = -1;

    // 概览指标
    double video_start_ms = 0.0;
    double audio_start_ms = 0.0;
    double av_start_offset_ms = 0.0;      // 音频首帧 - 视频首帧
    double max_av_offset_ms = 0.0;        // 播放过程最大音视频偏移（绝对值）
    double avg_frame_interval_ms = 0.0;
    double frame_interval_stddev_ms = 0.0;
    double max_frame_interval_ms = 0.0;
    double min_frame_interval_ms = 0.0;
    double video_duration_ms = 0.0;
    double audio_duration_ms = 0.0;
    int frame_count = 0;
    int key_frame_count = 0;
    bool vfr = false;                     // true = 可变帧率
    bool has_data = false;

    // 曲线（供 UI 图表）
    MetricSeries frame_interval_ms;       // 逐帧间隔
    MetricSeries av_offset_ms;            // 音视频偏移（逐次采样）
    MetricSeries packet_delta_ms;         // 包时间戳 delta（inter-arrival）

    // 帧间隔直方图（下标 = 毫秒，值 = 帧数），用于区分 CFR/VFR 与观察分布
    std::vector<int> frame_interval_histogram_ms;

    std::vector<TimelineDiagnostic> issues;

    int CountOf(TimelineIssueType type) const;
    bool HasIssue(TimelineIssueType type) const;
    std::string FrameRateVerdict() const { return vfr ? "VFR" : "CFR"; }
};

} // namespace model
} // namespace videoeye
