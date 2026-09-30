#pragma once

// StreamAnalyzer 的统计快照。
//
// 为什么住在 domain 而不是 analyzer 里：它是纯值对象（std 容器 + POD），
// 但消费方横跨两层 —— 报告导出器（core/reporting）和 UI 都要读它。
// 留在 StreamAnalyzer.h 里的话，任何只想读统计数字的人都会被
// libavcodec/avformat 拖进编译图（StreamAnalyzer 的签名里全是 AVPacket*）。
//
// 与 model::PacketInfo（core/domain/model/PacketInfo.h）不是一回事：
// 那个是播放器逐帧上报的单个包信息，这个是聚合后的统计。

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

namespace videoeye {
namespace model {

struct StreamStats {
    // 基本信息
    int total_packets = 0;
    long long total_bytes = 0;
    int video_packets = 0;
    int audio_packets = 0;

    // 帧率统计
    double current_fps = 0.0;
    double avg_fps = 0.0;
    int fps_window_size = 60;  // 60 秒窗口

    // 码率统计
    int current_bitrate_bps = 0;
    int avg_bitrate_bps = 0;
    int peak_bitrate_bps = 0;

    // GOP 统计（基于 packet flags 的轻量级近似）
    // 注：UI 侧另有更精确的 GopSummary 表（基于解码帧 pict_type），
    // 显示口径以 UI 侧为准。本字段供导出器在不解码时输出报告用。
    int current_gop_size = 0;  // 当前正在累加的 GOP 帧数
    int max_gop_size = 0;      // 历史最大 GOP 大小
    int key_frame_count = 0;   // 关键帧总数

    // 视频帧类型统计
    int total_video_frames = 0;
    int total_audio_frames = 0;
    int i_frame_count = 0;
    int p_frame_count = 0;
    int b_frame_count = 0;
    int other_frame_count = 0;

    // 包大小统计
    int avg_packet_size = 0;
    int max_packet_size = 0;
    int min_packet_size = INT32_MAX;

    // 时间统计
    double duration_seconds = 0.0;
    std::chrono::steady_clock::time_point start_time;

    // 按流类型统计
    std::map<int, int> packets_per_stream;
    std::map<int, long long> bytes_per_stream;

    // 获取可读字符串
    std::string ToString() const;
};

}  // namespace model
}  // namespace videoeye
