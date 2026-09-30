#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <map>

#include "core/domain/model/StreamStats.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace analyzer {

// StreamStats 已下放到 core/domain/model/StreamStats.h（纯值对象，报告层也要读）。
// 这里留别名，免得既有调用方为了换个位置改几十行。
using model::StreamStats;


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
    void AnalyzeAudioFrame();
    
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
    long long bitrate_window_bytes_;
    
    // 时间窗口
    std::chrono::steady_clock::time_point last_fps_calc_time_;
    std::chrono::steady_clock::time_point last_bitrate_calc_time_;
    
    // 历史记录最大值
    static constexpr int MAX_HISTORY_SIZE = 300; // 5分钟 @ 1fps
};

} // namespace analyzer
} // namespace videoeye
