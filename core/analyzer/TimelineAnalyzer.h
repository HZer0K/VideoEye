#pragma once

#include <map>
#include <vector>

#include "core/model/FrameTimingInfo.h"
#include "core/model/TimelineDiagnostic.h"

namespace videoeye {
namespace analyzer {

// 时间轴与同步诊断分析器
//
// 用法（两种数据来源可单独或同时使用）：
//   1) demux 层: OnPacket(PacketTiming)  —— 全文件扫描 / 播放时逐包
//   2) decode 层: OnFrame(FrameTimingInfo) —— 播放时逐帧（帧间隔、repeat_pict、VFR/CFR）
//   3) 同步采样: OnSyncSample(audio_ms, video_ms) —— 播放时音视频偏移
// 数据喂完后调用 Finish() 计算汇总指标与派生问题，结果从 result() 取。
class TimelineAnalyzer {
public:
    struct Options {
        double av_start_offset_warn_ms = 100.0;   // 首帧偏移告警
        double av_start_offset_error_ms = 500.0;  // 首帧偏移错误
        double av_duration_mismatch_warn_ms = 500.0;
        double frame_interval_spike_ratio = 3.0;      // 相对平均间隔倍数
        double frame_interval_spike_min_ms = 80.0;    // 绝对下限，避免小间隔噪声
        double duplicate_tolerance_ms = 0.5;          // 重复时间戳容差
        double dts_after_pts_tolerance_ms = 0.5;
        double vfr_stddev_ms = 1.0;                   // 帧间隔标准差阈值
        double vfr_relative_spread = 0.02;            // (p95-p5)/中位数 阈值
        int max_issues_per_type = 40;                 // 同类问题最多记录条数
        int histogram_max_ms = 200;                   // 直方图覆盖的最大间隔
    };

    TimelineAnalyzer() = default;

    void Reset();
    void SetOptions(const Options& options);
    const Options& options() const { return options_; }

    void OnPacket(const model::PacketTiming& packet);
    void OnFrame(const model::FrameTimingInfo& frame);
    void OnSyncSample(double audio_ms, double video_ms);

    // 数据喂完后调用：计算 A/V 偏移、时长差、帧间隔统计、VFR/CFR 判定与派生问题
    void Finish();

    // 不修改内部状态地获取"当前快照"（直播/播放过程中周期性刷新 UI 用）
    model::TimelineAnalysisResult Snapshot() const;

    const model::TimelineAnalysisResult& result() const { return result_; }
    model::TimelineAnalysisResult& result() { return result_; }

private:
    struct StreamState {
        int media_type = -1;
        double last_pts_ms = model::kNoTimestamp;
        double last_dts_ms = model::kNoTimestamp;
        double expected_next_pts_ms = model::kNoTimestamp;
        double first_ts_ms = model::kNoTimestamp;
        double last_ts_ms = model::kNoTimestamp;
        double last_duration_ms = model::kNoTimestamp;
        int64_t packet_count = 0;
        int64_t keyframe_count = 0;
    };

    void AddIssue(model::TimelineIssueType type, model::IssueSeverity severity, int stream_index,
                  double timestamp_ms, double metric_value_ms, double threshold_ms,
                  const std::string& detail, const std::string& suggestion);
    void FinalizeFrameStats();
    void FinalizeStreamSummary();

    Options options_;
    model::TimelineAnalysisResult result_;
    std::map<int, StreamState> streams_;
    std::map<int, std::map<int, size_t>> issue_index_;   // type -> (stream -> index in issues)
    std::map<int, int> issue_total_count_;               // type -> 累计条数（含被限流丢弃的）
    int packet_index_ = 0;
    int frame_index_ = 0;
    bool finished_ = false;
};

} // namespace analyzer
} // namespace videoeye
