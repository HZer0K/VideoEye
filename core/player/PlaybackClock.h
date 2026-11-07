#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
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

// 裸流（Annex-B ES 一类）没有容器时间轴：av_read_frame 出的包 pts/dts 恒为
// AV_NOPTS_VALUE，但 h264/hevc demuxer 仍会给出可用的 packet->duration
// （实测 1/1200000 时基下 48000 → 40ms/帧）。缺时间戳时 PacketTimestampSeconds
// 返回 NaN、PaceTo 直接 return，解码线程全速吐帧，位置恒 0、A/V 采样失效。
//
// 这里按流把帧间隔累加成单调递增的 pts/dts，让播放节奏、帧时间戳、位置上报
// 与分析回调重新共享一个时间基准。只补时间戳，不改包数据、不重排帧。
//
// 帧间隔的来源有两条，按优先级取：
//   1. packet->duration（demuxer 直接给的，最贴近实际码流，能反映变帧率）；
//   2. AVStream 的 avg_frame_rate / r_frame_rate 换算（部分裸流不给 duration）。
// 两条都拿不到就**放弃**合成 —— 拿猜测的帧率凑节奏比不补更糟。
//
// ⚠️ 只对「从未出现过真实时间戳」的流生效：一旦某条流给过真实 pts/dts 就永久
// 停手，绝不干扰容器里的 B 帧重排 / edit list / 非恒定帧率时间轴。
class RawStreamTimestampFiller {
public:
    // 返回 true 表示本次确实补了时间戳（调用方可据此记录诊断）。
    //
    // format_ctx 可选：某些裸流 demuxer 不给 packet->duration，但仍能通过
    // AVStream::avg_frame_rate（或 r_frame_rate）算出恒定帧间隔。没有它就只能
    // 放弃合成 —— 宁可不动时间戳，也不要凭空编一个节奏出来。
    bool Fill(AVPacket* packet, const AVFormatContext* format_ctx = nullptr) {
        if (!packet || packet->stream_index < 0)
            return false;

        StreamState& state = states_[packet->stream_index];
        if (packet->pts != AV_NOPTS_VALUE || packet->dts != AV_NOPTS_VALUE) {
            state.saw_real_timestamp = true;
            return false;
        }
        if (state.saw_real_timestamp)
            return false;

        const std::int64_t step =
            (packet->duration > 0) ? packet->duration : FrameDurationTicks(format_ctx, packet->stream_index);
        if (step <= 0)
            return false;

        packet->pts = state.next_ts;
        packet->dts = state.next_ts;
        state.next_ts += step;
        return true;
    }

    // 换片 / 重新打开时调用；DecodeThread 每次播放都会新建对象，正常无需手动调。
    void Reset() {
        states_.clear();
    }

    bool HasRealTimestamps(int stream_index) const {
        const auto it = states_.find(stream_index);
        return it != states_.end() && it->second.saw_real_timestamp;
    }

private:
    // 一帧在流时基下占多少 tick。
    //
    // 优先 avg_frame_rate（容器/探测给出的平均值，裸 ES 也能拿到），回退 r_frame_rate
    // （码流里最"紧凑"的帧率，恒帧率素材上两者相等）。两者都不可用时返回 0，表示
    // **无法**推出步长 —— 调用方据此放弃合成，而不是拿一个猜测值凑节奏。
    //
    // 换算：一帧秒数 = fps.den / fps.num；tick 数 = 一帧秒数 / av_q2d(time_base)。
    // 例：h264 demuxer 给 time_base=1/1200000、fps=25 → 48000 tick = 40ms，与
    // packet->duration 那条路径的实测值完全一致。
    static std::int64_t FrameDurationTicks(const AVFormatContext* format_ctx, int stream_index) {
        if (!format_ctx || stream_index < 0 || stream_index >= static_cast<int>(format_ctx->nb_streams))
            return 0;
        const AVStream* stream = format_ctx->streams[stream_index];
        if (!stream || stream->time_base.den == 0 || stream->time_base.num == 0)
            return 0;

        AVRational fps = stream->avg_frame_rate;
        if (fps.num <= 0 || fps.den <= 0)
            fps = stream->r_frame_rate;
        if (fps.num <= 0 || fps.den <= 0)
            return 0;

        const double seconds_per_frame = static_cast<double>(fps.den) / static_cast<double>(fps.num);
        const double ticks = seconds_per_frame / av_q2d(stream->time_base);
        if (!std::isfinite(ticks) || ticks < 1.0)
            return 0;
        return static_cast<std::int64_t>(std::llround(ticks));
    }

    struct StreamState {
        std::int64_t next_ts = 0;
        bool saw_real_timestamp = false;
    };
    std::map<int, StreamState> states_;
};

} // namespace player
} // namespace videoeye
