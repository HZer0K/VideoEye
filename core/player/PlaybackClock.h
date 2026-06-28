#pragma once

#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

namespace videoeye {
namespace player {

// 选择播放时钟流索引：视频优先，其次音频
inline int SelectPlaybackClockStreamIndex(int audio_stream_index, int video_stream_index) {
    if (video_stream_index >= 0) {
        return video_stream_index;
    }
    if (audio_stream_index >= 0) {
        return audio_stream_index;
    }
    return -1;
}

// 从包中提取时间戳（秒）
inline double PacketTimestampSeconds(const AVFormatContext* format_ctx, const AVPacket* packet) {
    if (!format_ctx || !packet || packet->stream_index < 0 ||
        packet->stream_index >= static_cast<int>(format_ctx->nb_streams)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    AVStream* stream = format_ctx->streams[packet->stream_index];
    if (!stream || stream->time_base.den == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const int64_t pts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
    if (pts == AV_NOPTS_VALUE) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return pts * av_q2d(stream->time_base);
}

// 播放节奏控制器：基于墙钟时间同步媒体播放速度
struct PlaybackClock {
    using SteadyClock = std::chrono::steady_clock;

    void Reset() {
        started = false;
        first_media_ts = 0.0;
        wall_start = SteadyClock::now();
    }

    void OnPaused(const SteadyClock::duration& paused_for) {
        if (started) {
            wall_start += paused_for;
        }
    }

    void Sync(double media_ts) {
        if (!std::isfinite(media_ts)) {
            return;
        }
        if (!started) {
            started = true;
            first_media_ts = media_ts;
            wall_start = SteadyClock::now();
        }
    }

    void PaceTo(double media_ts) {
        if (!std::isfinite(media_ts)) {
            return;
        }
        if (!started) {
            Sync(media_ts);
            return;
        }
        if (media_ts < first_media_ts) {
            return;
        }

        const auto target = wall_start + std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(media_ts - first_media_ts));
        const auto now = SteadyClock::now();
        if (target > now) {
            std::this_thread::sleep_for(target - now);
        }
    }

    bool started = false;
    double first_media_ts = 0.0;
    SteadyClock::time_point wall_start = SteadyClock::now();
};

} // namespace player
} // namespace videoeye
