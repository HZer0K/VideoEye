#pragma once

// 裸视频基本流（Annex-B ES 一类）的公共判定与换算。
//
// 这类输入的共同点：FFmpeg 的 demuxer 能读包，但容器层**没有**总时长、码率与
// 索引 —— format->duration == AV_NOPTS_VALUE、av_seek_frame 必然失败。播放侧
// 需要三样东西才能把进度条与拖动做对，且口径必须只有一份：
//   1. 判定"这是裸流"（只有裸流才走时长估算与按字节定位的兜底）；
//   2. 一帧占多少时间（packet->duration 缺失时由帧率换算）；
//   3. 目标时间按比例换算成文件字节偏移。
//
// 放在头文件里是为了让只链接 FFmpeg 的单测（不链接播放器/Qt）能直接验证口径。

#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace videoeye {
namespace player {
namespace bare_stream {

// FFmpeg 里"裸视频基本流"的 demuxer 名（与文件后缀无关，探测/显式指定都可能命中）。
inline bool IsBareVideoEsFormatName(const char* name) {
    if (!name)
        return false;
    static const char* const kNames[] = {
        "h264", "hevc", "vvc", "obu", "av1", "mpegvideo", "m4v", "vc1", "dirac", "cavsvideo",
    };
    for (const char* candidate : kNames) {
        if (std::strcmp(name, candidate) == 0)
            return true;
    }
    return false;
}

// 判据同时看 demuxer 名与有无容器时长：个别封装（如带索引的 m4v）虽然 demuxer
// 名在名单里，却给了 duration，走正常的 av_seek_frame 即可，不该进兜底路径。
inline bool IsBareVideoEs(const AVFormatContext* format_ctx) {
    if (!format_ctx || !format_ctx->iformat)
        return false;
    if (!IsBareVideoEsFormatName(format_ctx->iformat->name))
        return false;
    return format_ctx->duration == AV_NOPTS_VALUE || format_ctx->duration <= 0;
}

// 一帧的秒数：优先 avg_frame_rate（探测给出的平均值），回退 r_frame_rate。
// 两者都不可用返回 0，表示无法推出帧间隔 —— 调用方应放弃，而不是拿猜测值凑节奏。
inline double FrameDurationSeconds(const AVStream* stream) {
    if (!stream)
        return 0.0;
    AVRational fps = stream->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0)
        fps = stream->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0)
        return 0.0;
    return static_cast<double>(fps.den) / static_cast<double>(fps.num);
}

// 一帧在流时基下占多少 tick。
// 换算：一帧秒数 / av_q2d(time_base)。
// 例：h264 demuxer 给 time_base=1/1200000、fps=25 → 48000 tick = 40ms，与
// packet->duration 那条路径的实测值完全一致。
inline std::int64_t FrameDurationTicks(const AVStream* stream) {
    if (!stream || stream->time_base.den == 0 || stream->time_base.num == 0)
        return 0;
    const double seconds_per_frame = FrameDurationSeconds(stream);
    if (seconds_per_frame <= 0.0)
        return 0;
    const double ticks = seconds_per_frame / av_q2d(stream->time_base);
    if (!(ticks >= 1.0))
        return 0;
    return static_cast<std::int64_t>(llround(ticks));
}

inline std::int64_t FrameDurationTicks(const AVFormatContext* format_ctx, int stream_index) {
    if (!format_ctx || stream_index < 0 || stream_index >= static_cast<int>(format_ctx->nb_streams))
        return 0;
    return FrameDurationTicks(format_ctx->streams[stream_index]);
}

// 目标时间按比例换算成文件字节偏移。裸流没有索引，只能做近似定位；误差来自
// 变码率，但对"拖到大概位置再解码"来说足够。
inline std::int64_t ComputeByteSeekOffset(std::int64_t file_size, std::int64_t target_ms, std::int64_t duration_ms) {
    if (file_size <= 0 || duration_ms <= 0 || target_ms <= 0)
        return 0;
    if (target_ms >= duration_ms)
        return file_size - 1;
    const long double ratio = static_cast<long double>(target_ms) / static_cast<long double>(duration_ms);
    const auto offset = static_cast<std::int64_t>(static_cast<long double>(file_size - 1) * ratio);
    return offset < 0 ? 0 : offset;
}

// 毫秒 → 流时基 tick（定位后重定合成时间轴的起点用）。
inline std::int64_t MsToStreamTicks(std::int64_t ms, AVRational time_base) {
    return av_rescale_q(ms, AVRational{1, 1000}, time_base);
}

// 有界扫描的结果 → 总时长（毫秒）。
// 扫到 EOF 就是精确值；只扫了一段就按"已读字节覆盖的媒体时长"线性外推。
// 变码率素材上这是估算 —— 调用方必须把结果当"可拖动的近似总量"，不是精确时长。
inline std::int64_t EstimateDurationMsFromScan(double media_seconds, std::int64_t scanned_bytes, std::int64_t file_size,
                                               bool reached_eof) {
    if (!(media_seconds > 0.0))
        return 0;
    if (reached_eof)
        return static_cast<std::int64_t>(std::llround(media_seconds * 1000.0));
    if (scanned_bytes <= 0 || file_size <= 0)
        return 0;
    const long double scale = static_cast<long double>(file_size) / static_cast<long double>(scanned_bytes);
    const long double ms = static_cast<long double>(media_seconds) * 1000.0L * scale;
    if (ms <= 0.0L)
        return 0;
    return static_cast<std::int64_t>(ms);
}

} // namespace bare_stream
} // namespace player
} // namespace videoeye
