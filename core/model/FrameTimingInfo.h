#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace videoeye {
namespace model {

// 无效时间戳哨兵（NaN）。所有 ms 字段统一用 IsValidTimestamp() 判断有效性，
// 避免用 0 / -1 这类可能与真实值冲突的魔法数。
constexpr double kNoTimestamp = std::numeric_limits<double>::quiet_NaN();

inline bool IsValidTimestamp(double timestamp_ms) {
    return !std::isnan(timestamp_ms);
}

// demux 层：单个 packet 的时间信息（已按 stream time_base 统一换算为毫秒）
struct PacketTiming {
    int index = 0;
    int stream_index = -1;
    int media_type = -1;        // AVMediaType: 0=video, 1=audio
    double pts_ms = kNoTimestamp;
    double dts_ms = kNoTimestamp;
    double duration_ms = kNoTimestamp;
    int64_t pos = -1;
    int size = 0;
    int flags = 0;
    bool key_frame = false;
};

// decode 层：单个 frame 的时间信息
struct FrameTimingInfo {
    int index = 0;
    int stream_index = -1;
    double pts_ms = kNoTimestamp;
    double best_effort_ms = kNoTimestamp;   // AVFrame::best_effort_timestamp
    double display_ms = kNoTimestamp;       // 实际显示时间（播放器采用的时间戳）
    double duration_ms = kNoTimestamp;      // 帧持续时间（含 repeat_pict 时可用于修正）
    int pict_type = 0;                      // AVPictureType
    int repeat_pict = 0;                    // 重复场/帧指示（3:2 pulldown 等）
    bool key_frame = false;
    int size = 0;

    bool IsValid() const { return IsValidTimestamp(display_ms); }
};

} // namespace model
} // namespace videoeye
