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

// 与时间轴问题类型对应的稳定 rule_id。
// 时间轴分析器产出的问题统一并入 DiagnosticIssue 体系（category=Timing），
// 这样诊断报告/问题表/计分只认 DiagnosticIssue 一种载体，避免与时间轴模块各搞一套。
inline const char* RuleIdOf(TimelineIssueType type) {
    switch (type) {
        case TimelineIssueType::PtsNonMonotonic:            return "timeline.pts_non_monotonic";
        case TimelineIssueType::DtsNonMonotonic:            return "timeline.dts_non_monotonic";
        case TimelineIssueType::DtsAfterPts:                return "timeline.dts_after_pts";
        case TimelineIssueType::AudioVideoStartOffset:      return "timeline.av_start_offset";
        case TimelineIssueType::AudioVideoDurationMismatch: return "timeline.av_duration_mismatch";
        case TimelineIssueType::FrameIntervalSpike:         return "timeline.frame_interval_spike";
        case TimelineIssueType::DuplicatedTimestamp:        return "timeline.duplicated_timestamp";
        case TimelineIssueType::MissingKeyframeIndex:       return "timeline.missing_keyframe_index";
    }
    return "timeline.unknown";
}

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

    // 问题列表（统一为 DiagnosticIssue，category=Timing）
    std::vector<DiagnosticIssue> issues;

    int CountOf(TimelineIssueType type) const;
    bool HasIssue(TimelineIssueType type) const;
    std::string FrameRateVerdict() const { return vfr ? "VFR" : "CFR"; }
};

} // namespace model
} // namespace videoeye
