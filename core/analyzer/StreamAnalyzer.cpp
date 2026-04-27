#include "StreamAnalyzer.h"
#include "utils/Logger.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <climits>

namespace videoeye {
namespace analyzer {

StreamAnalyzer::StreamAnalyzer()
    : is_analyzing_(false)
    , last_pts_(AV_NOPTS_VALUE)
    , frame_count_(0)
    , last_fps_calc_time_(std::chrono::steady_clock::now())
    , last_bitrate_calc_time_(std::chrono::steady_clock::now()) {
}

StreamAnalyzer::~StreamAnalyzer() {
    Stop();
}

void StreamAnalyzer::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_analyzing_ = true;
    stats_.start_time = std::chrono::steady_clock::now();
    LOG_INFO("流分析器已启动");
}

void StreamAnalyzer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_analyzing_ = false;
    LOG_INFO("流分析器已停止");
}

void StreamAnalyzer::AnalyzePacket(const AVPacket* packet, const AVFormatContext* format_ctx) {
    if (!packet || !is_analyzing_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 更新基本统计
    stats_.total_packets++;
    stats_.total_bytes += packet->size;
    stats_.packets_per_stream[packet->stream_index]++;
    stats_.bytes_per_stream[packet->stream_index] += packet->size;
    
    // 区分视频和音频包
    if (format_ctx) {
        if (packet->stream_index < format_ctx->nb_streams) {
            AVCodecParameters* codec_params = format_ctx->streams[packet->stream_index]->codecpar;
            if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
                stats_.video_packets++;
            } else if (codec_params->codec_type == AVMEDIA_TYPE_AUDIO) {
                stats_.audio_packets++;
            }
        }
    }
    
    // 更新包大小统计
    stats_.max_packet_size = std::max(stats_.max_packet_size, packet->size);
    stats_.min_packet_size = std::min(stats_.min_packet_size, packet->size);
    stats_.avg_packet_size = stats_.total_bytes / stats_.total_packets;
    
    // 更新 GOP 信息
    UpdateGopInfo(packet);
    
    // 记录包历史
    PacketInfo pkt_info;
    pkt_info.stream_index = packet->stream_index;
    pkt_info.size = packet->size;
    pkt_info.pts = packet->pts;
    pkt_info.dts = packet->dts;
    pkt_info.duration = packet->duration;
    pkt_info.is_key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    pkt_info.timestamp = std::chrono::steady_clock::now();
    
    packet_history_.push_back(pkt_info);
    if (packet_history_.size() > MAX_HISTORY_SIZE) {
        packet_history_.erase(packet_history_.begin());
    }
    
    // 计算帧率 (每秒一次)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_fps_calc_time_).count();
    
    if (elapsed >= 1) {
        CalculateFps();
        last_fps_calc_time_ = now;
    }
    
    // 计算码率 (每秒一次)
    elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_bitrate_calc_time_).count();
    
    if (elapsed >= 1) {
        CalculateBitrate();
        last_bitrate_calc_time_ = now;
    }
    
    // 更新持续时间
    stats_.duration_seconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - stats_.start_time).count() / 1000.0;
}

StreamStats StreamAnalyzer::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void StreamAnalyzer::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    stats_ = StreamStats();
    packet_history_.clear();
    fps_history_.clear();
    bitrate_history_.clear();
    last_pts_ = AV_NOPTS_VALUE;
    frame_count_ = 0;
    
    LOG_INFO("流分析器已重置");
}

void StreamAnalyzer::AnalyzeVideoFrame(AVPictureType type) {
    if (!is_analyzing_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_video_frames++;

    switch (type) {
    case AV_PICTURE_TYPE_I:
        stats_.i_frame_count++;
        break;
    case AV_PICTURE_TYPE_P:
        stats_.p_frame_count++;
        break;
    case AV_PICTURE_TYPE_B:
        stats_.b_frame_count++;
        break;
    default:
        stats_.other_frame_count++;
        break;
    }
}

std::vector<PacketInfo> StreamAnalyzer::GetRecentPackets(int count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (packet_history_.size() <= count) {
        return packet_history_;
    }
    
    return std::vector<PacketInfo>(
        packet_history_.end() - count,
        packet_history_.end()
    );
}

void StreamAnalyzer::CalculateFps() {
    // 计算当前 GOP 的帧率
    if (stats_.key_frame_count > 0 && frame_count_ > 0) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_fps_calc_time_).count() / 1000.0;
        
        if (elapsed > 0) {
            stats_.current_fps = frame_count_ / elapsed;
            
            // 更新历史
            fps_history_.push_back(stats_.current_fps);
            if (fps_history_.size() > MAX_HISTORY_SIZE) {
                fps_history_.erase(fps_history_.begin());
            }
            
            // 计算平均帧率
            if (!fps_history_.empty()) {
                double sum = std::accumulate(fps_history_.begin(), fps_history_.end(), 0.0);
                stats_.avg_fps = sum / fps_history_.size();
            }
            
            frame_count_ = 0;
        }
    }
}

void StreamAnalyzer::CalculateBitrate() {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_bitrate_calc_time_).count() / 1000.0;
    
    if (elapsed > 0 && stats_.total_bytes > 0) {
        // 计算当前码率 (bps)
        stats_.current_bitrate_bps = (stats_.total_bytes * 8) / elapsed;
        
        // 更新峰值码率
        stats_.peak_bitrate_bps = std::max(stats_.peak_bitrate_bps, stats_.current_bitrate_bps);
        
        // 更新历史
        bitrate_history_.push_back(stats_.current_bitrate_bps);
        if (bitrate_history_.size() > MAX_HISTORY_SIZE) {
            bitrate_history_.erase(bitrate_history_.begin());
        }
        
        // 计算平均码率
        if (!bitrate_history_.empty()) {
            long long sum = std::accumulate(bitrate_history_.begin(), bitrate_history_.end(), 0LL);
            stats_.avg_bitrate_bps = sum / bitrate_history_.size();
        }
    }
}

void StreamAnalyzer::UpdateGopInfo(const AVPacket* packet) {
    // 检查是否是关键帧
    if (packet->flags & AV_PKT_FLAG_KEY) {
        stats_.key_frame_count++;
        
        // 记录当前 GOP 大小
        if (stats_.current_gop_size > 0) {
            stats_.gop_size = stats_.current_gop_size;
            stats_.max_gop_size = std::max(stats_.max_gop_size, stats_.current_gop_size);
        }
        
        // 重置当前 GOP 计数器
        stats_.current_gop_size = 0;
    }
    
    stats_.current_gop_size++;
    frame_count_++;
}

std::string StreamStats::ToString() const {
    std::ostringstream oss;
    
    oss << "=== 流统计信息 ===" << std::endl;
    oss << "总包数: " << total_packets << std::endl;
    oss << "总字节数: " << (total_bytes / 1024) << " KB" << std::endl;
    oss << "视频包: " << video_packets << ", 音频包: " << audio_packets << std::endl;
    oss << std::endl;
    
    oss << "帧率: " << current_fps << " fps (平均: " << avg_fps << " fps)" << std::endl;
    oss << "码率: " << (current_bitrate_bps / 1000) << " kbps (平均: " 
        << (avg_bitrate_bps / 1000) << " kbps, 峰值: " << (peak_bitrate_bps / 1000) << " kbps)" << std::endl;
    oss << std::endl;
    
    oss << "GOP 大小: " << gop_size << " (最大: " << max_gop_size << ")" << std::endl;
    oss << "关键帧数: " << key_frame_count << std::endl;
    oss << "帧类型: I=" << i_frame_count << " P=" << p_frame_count << " B=" << b_frame_count
        << " 其他=" << other_frame_count << std::endl;
    oss << std::endl;
    
    oss << "包大小: 平均 " << avg_packet_size << " B, 最大 " 
        << max_packet_size << " B, 最小 " << min_packet_size << " B" << std::endl;
    oss << "持续时间: " << duration_seconds << " 秒" << std::endl;
    
    return oss.str();
}

} // namespace analyzer
} // namespace videoeye
