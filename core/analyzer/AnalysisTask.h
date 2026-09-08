#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/MetricSeries.h"

namespace videoeye {
namespace analyzer {

// 全文件分析的可选项
struct AnalysisOptions {
    double sample_interval_seconds = 1.0;  // 码率/帧率序列的采样粒度
    int64_t max_packets = 0;               // 0 表示不限制（超长文件可设上限做抽样）
    bool detect_container_layout = true;   // MP4 家族是否检测 moov/mdat 顺序
};

// 单条流的静态摘要（demux 层，不解码）
struct StreamDigest {
    int index = -1;
    int media_type = -1;          // AVMediaType
    std::string codec_name;
    std::string profile_name;
    int width = 0;
    int height = 0;
    double avg_fps = 0.0;
    int sample_rate = 0;
    int channels = 0;
    int64_t bitrate_bps = 0;
    int64_t packet_count = 0;
    int64_t byte_count = 0;
    int64_t frame_count = 0;      // 视频按包估算
    int64_t key_frame_count = 0;
    double start_seconds = 0.0;
    double duration_seconds = 0.0;

    bool IsVideo() const { return media_type == 0; }  // AVMEDIA_TYPE_VIDEO
    bool IsAudio() const { return media_type == 1; }  // AVMEDIA_TYPE_AUDIO
};

// 全文件扫描结果（诊断的唯一数据源）
struct AnalysisResult {
    // 文件级
    std::string file_path;
    std::string container_format;
    double duration_seconds = 0.0;
    int64_t file_size_bytes = 0;
    int64_t overall_bitrate_bps = 0;
    bool seekable = true;
    bool moov_after_mdat = false;   // 仅 MP4 家族有效

    // 流级
    std::vector<StreamDigest> streams;

    // 时间序列指标
    model::MetricSeries total_bitrate_kbps;
    model::MetricSeries video_bitrate_kbps;
    model::MetricSeries video_fps;

    // GOP / 关键帧
    std::vector<double> gop_intervals_seconds;
    std::vector<int> gop_frame_sizes;
    double max_gop_interval_seconds = 0.0;
    int max_gop_frames = 0;

    // 时间戳健康度
    int64_t total_packets = 0;
    int64_t total_bytes = 0;
    int64_t video_packets = 0;
    int64_t key_frame_count = 0;
    int64_t packets_missing_dts = 0;
    int64_t pts_non_monotonic_count = 0;
    double max_timestamp_gap_seconds = 0.0;
    double max_gap_at_seconds = -1.0;     // 最大跳变发生的时间点（用于定位）
    double pts_non_monotonic_at_seconds = -1.0;

    // 包大小
    int64_t max_packet_bytes = 0;
    double avg_packet_bytes = 0.0;

    // 执行状态
    bool completed = true;        // false = 被取消
    std::string error_message;

    int VideoStreamCount() const;
    int AudioStreamCount() const;
    const StreamDigest* FirstVideoStream() const;
    const StreamDigest* FirstAudioStream() const;
};

} // namespace analyzer
} // namespace videoeye
