#include "core/domain/model/StreamStats.h"

#include <sstream>

namespace videoeye {
namespace model {

std::string StreamStats::ToString() const {
    std::ostringstream oss;

    oss << "=== 流统计信息 ===" << std::endl;
    oss << "总包数: " << total_packets << std::endl;
    oss << "总字节数: " << (total_bytes / 1024) << " KB" << std::endl;
    oss << "视频包: " << video_packets << ", 音频包: " << audio_packets << std::endl;
    oss << "视频帧: " << total_video_frames << ", 音频帧: " << total_audio_frames << std::endl;
    oss << std::endl;

    oss << "帧率: " << current_fps << " fps (平均: " << avg_fps << " fps)" << std::endl;
    oss << "码率: " << (current_bitrate_bps / 1000) << " kbps (平均: "
        << (avg_bitrate_bps / 1000) << " kbps, 峰值: " << (peak_bitrate_bps / 1000) << " kbps)"
        << std::endl;
    oss << std::endl;

    oss << "GOP 大小: " << current_gop_size << " (最大: " << max_gop_size << ")" << std::endl;
    oss << "关键帧数: " << key_frame_count << std::endl;
    oss << "帧类型: I=" << i_frame_count << " P=" << p_frame_count << " B=" << b_frame_count
        << " 其他=" << other_frame_count << std::endl;
    oss << std::endl;

    oss << "包大小: 平均 " << avg_packet_size << " B, 最大 " << max_packet_size << " B, 最小 "
        << min_packet_size << " B" << std::endl;
    oss << "持续时间: " << duration_seconds << " 秒" << std::endl;

    return oss.str();
}

}  // namespace model
}  // namespace videoeye
