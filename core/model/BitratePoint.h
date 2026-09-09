#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 帧类型
//
// 取值与 FFmpeg 的 AVPictureType 对齐（Unknown=0/I=1/P=2/B=3/S=4/SI=5/SP=6/BI=7），
// 但本头文件刻意不 include FFmpeg —— 这样 BitrateGopAnalyzer 及其单测可以脱离
// FFmpeg 与 Qt 独立编译（与 SceneChangeAnalyzer 一致的做法）。
// 接入侧只需 `static_cast<FrameType>(pict_type)`。
enum class FrameType {
    Unknown = 0,  // 未解析（未开解析器，或解析器无法判定）
    I = 1,
    P = 2,
    B = 3,
    S = 4,        // MPEG-4 S(GMC)-VOP
    SI = 5,
    SP = 6,
    BI = 7,
};

const char* ToString(FrameType type);

// 是否为帧内编码帧（I/SI/S/BI 都属于帧内或可独立解码类型；BI 实际是 B 帧的特例，
// 这里仅 I/SI 视为真正可独立解码的帧内帧）
bool IsIntraFrame(FrameType type);

// 是否为会向后参考的帧（用于判断 GOP 是否 closed 的辅助信息，当前未参与判定）
bool IsReferenceFrame(FrameType type);

// 滑动窗口码率采样点
//
// 一个点 = 以 timestamp_seconds 为中心、长度 window_seconds 的窗口内
// 所有视频包字节数换算出的瞬时码率。窗口之间按 hop 步进（默认 window/4），
// 因此相邻点有重叠，曲线比"逐秒分桶"更平滑、峰值更接近真实 VBV 表现。
struct BitratePoint {
    double timestamp_seconds = 0.0;   // 窗口中心时刻（秒）
    double window_seconds = 1.0;      // 窗口长度（秒）
    int64_t bytes = 0;                // 窗口内字节数
    int frame_count = 0;              // 窗口内帧数
    double bitrate_kbps = 0.0;        // bytes * 8 / window / 1000
    bool overshoot = false;           // 是否超过目标峰值码率
    bool peak_region = false;         // 是否位于被标记为"峰值区间"的连续段内

    double WindowStart() const { return timestamp_seconds - window_seconds * 0.5; }
    double WindowEnd() const { return timestamp_seconds + window_seconds * 0.5; }
};

// 由窗口内字节数换算码率(kbps)；window 非法时返回 0
double BytesToKbps(int64_t bytes, double window_seconds);

}  // namespace model
}  // namespace videoeye
