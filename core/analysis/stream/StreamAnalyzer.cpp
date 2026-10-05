#include "core/analysis/stream/StreamAnalyzer.h"
#include "core/domain/model/StreamStats.h"
#include "infrastructure/logging/Logger.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <climits>

namespace videoeye {

StreamAnalyzer::StreamAnalyzer()
    : is_analyzing_(false)
    , last_pts_(AV_NOPTS_VALUE)
    , frame_count_(0)
    , bitrate_window_bytes_(0)
    , last_fps_calc_time_(std::chrono::steady_clock::now())
    , last_bitrate_calc_time_(std::chrono::steady_clock::now()) {
}

StreamAnalyzer::~StreamAnalyzer() {
    Stop();
}

void StreamAnalyzer::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now(); 
    is_analyzing_ = true;
    stats_.start_time = now;
    frame_count_ = 0;
    bitrate_window_bytes_ = 0;
    last_fps_calc_time_ = now;
    last_bitrate_calc_time_ = now;
    LOG_INFO("流分析器已启动");
}

void StreamAnalyzer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_analyzing_ = false;
    LOG_INFO("流分析器已停止");
}

void StreamAnalyzer::AnalyzePacket(const AVPacket* packet, const AVFormatContext* format_ctx) {
    if (!packet) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_analyzing_) {
        return;
    }
    
    // 区分视频和音频包
    AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
    if (format_ctx) {
        const int idx = packet->stream_index;
        if (idx >= 0 && idx < static_cast<int>(format_ctx->nb_streams) && format_ctx->streams) {
            AVStream* stream = format_ctx->streams[idx];
            if (stream && stream->codecpar) {
                media_type = stream->codecpar->codec_type;
            }
        }
    }

    // 更新基本统计
    stats_.total_packets++;
    stats_.total_bytes += packet->size;
    bitrate_window_bytes_ += packet->size;
    stats_.packets_per_stream[packet->stream_index]++;
    stats_.bytes_per_stream[packet->stream_index] += packet->size;

    // 区分视频和音频包
    if (media_type == AVMEDIA_TYPE_VIDEO) {
        stats_.video_packets++;
    } else if (media_type == AVMEDIA_TYPE_AUDIO) {
        stats_.audio_packets++;
    }
    
    // 更新包大小统计
    stats_.max_packet_size = std::max(stats_.max_packet_size, packet->size);
    stats_.min_packet_size = std::min(stats_.min_packet_size, packet->size);
    stats_.avg_packet_size = stats_.total_bytes / stats_.total_packets;
    
    // 更新 GOP 信息: 只统计视频流的关键帧/帧数.
    // 注意: 必须用 media_type 明确判定为视频, 不能退化为 "!format_ctx" 兜底——
    // 否则在拿不到 format_ctx 时, 音频包/数据包也会按 GOP 帧累加, 污染 key_frame_count 与 GOP 统计.
    if (media_type == AVMEDIA_TYPE_VIDEO) {
        UpdateGopInfo(packet);
    }
    
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
    bitrate_window_bytes_ = 0;
    last_fps_calc_time_ = std::chrono::steady_clock::now();
    last_bitrate_calc_time_ = last_fps_calc_time_;
    
    LOG_INFO("流分析器已重置");
}

void StreamAnalyzer::AnalyzeVideoFrame(AVPictureType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_analyzing_) {
        return;
    }
    stats_.total_video_frames++;
    frame_count_++;

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

void StreamAnalyzer::AnalyzeAudioFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_analyzing_) {
        return;
    }
    stats_.total_audio_frames++;
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

std::vector<double> StreamAnalyzer::GetFpsHistory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fps_history_;
}

std::vector<int> StreamAnalyzer::GetBitrateHistory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bitrate_history_;
}

void StreamAnalyzer::CalculateFps() {
    if (frame_count_ > 0) {
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
    
    if (elapsed > 0 && bitrate_window_bytes_ > 0) {
        // 计算当前码率 (bps)
        stats_.current_bitrate_bps = static_cast<int>(bitrate_window_bytes_ * 8) / elapsed;
        
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
        
        // 重置当前 GOP 计数器
        bitrate_window_bytes_ = 0;
    }
}

void StreamAnalyzer::UpdateGopInfo(const AVPacket* packet) {
    // 基于 packet flags 的轻量级 GOP 统计. 仅供 ReportExporter 在不解码时输出报告用;
    // UI 侧用解码帧 pict_type 计算更精确的 GOP 表.
    if (packet->flags & AV_PKT_FLAG_KEY) {
        stats_.key_frame_count++;

        // 收尾上一个 GOP, 记录历史最大值
        if (stats_.current_gop_size > 0) {
            stats_.max_gop_size = std::max(stats_.max_gop_size, stats_.current_gop_size);
        }

        // 重置当前 GOP 计数器
        stats_.current_gop_size = 0;
    }

    stats_.current_gop_size++;
}

} // namespace videoeye
