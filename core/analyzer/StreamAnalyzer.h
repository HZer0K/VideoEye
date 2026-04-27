#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace analyzer {

// 包信息结构体
struct PacketInfo {
    int stream_index;
    int size;
    int64_t pts;
    int64_t dts;
    int duration;
    bool is_key_frame;
    std::chrono::steady_clock::time_point timestamp;
};

// 流统计信息
struct StreamStats {
    // 基本信息
    int total_packets = 0;
    long long total_bytes = 0;
    int video_packets = 0;
    int audio_packets = 0;
    
    // 帧率统计
    double current_fps = 0.0;
    double avg_fps = 0.0;
    int fps_window_size = 60; // 60秒窗口
    
    // 码率统计
    int current_bitrate_bps = 0;
    int avg_bitrate_bps = 0;
    int peak_bitrate_bps = 0;
    
    // GOP 统计
    int gop_size = 0;
    int current_gop_size = 0;
    int max_gop_size = 0;
    int key_frame_count = 0;

    // 视频帧类型统计
    int total_video_frames = 0;
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

// 流分析器类
class StreamAnalyzer {
public:
    StreamAnalyzer();
    ~StreamAnalyzer();
    
    // 分析数据包
    void AnalyzePacket(const AVPacket* packet, const AVFormatContext* format_ctx);
    
    // 获取统计信息
    StreamStats GetStats() const;
    
    // 重置统计
    void Reset();
    
    // 开始分析
    void Start();
    
    // 停止分析
    void Stop();
    
    // 获取最近的数据包历史 (用于图表显示)
    std::vector<PacketInfo> GetRecentPackets(int count = 100) const;
    
    // 获取帧率历史
    std::vector<double> GetFpsHistory() const { return fps_history_; }
    
    // 获取码率历史
    std::vector<int> GetBitrateHistory() const { return bitrate_history_; }

    void AnalyzeVideoFrame(AVPictureType type);
    
private:
    // 计算帧率
    void CalculateFps();
    
    // 计算码率
    void CalculateBitrate();
    
    // 更新 GOP 信息
    void UpdateGopInfo(const AVPacket* packet);
    
    // 成员变量
    mutable std::mutex mutex_;
    StreamStats stats_;
    std::vector<PacketInfo> packet_history_;
    std::vector<double> fps_history_;
    std::vector<int> bitrate_history_;
    
    bool is_analyzing_;
    int64_t last_pts_;
    int frame_count_;
    
    // 时间窗口
    std::chrono::steady_clock::time_point last_fps_calc_time_;
    std::chrono::steady_clock::time_point last_bitrate_calc_time_;
    
    // 历史记录最大值
    static constexpr int MAX_HISTORY_SIZE = 300; // 5分钟 @ 1fps
};

} // namespace analyzer
} // namespace videoeye
