#pragma once

#include <cstdint>
#include <string>

namespace videoeye {
namespace model {

// 一条 GOP（Group Of Pictures）记录
//
// 口径: GOP 以关键帧（IDR/CRA/recovery point）开始，到下一个关键帧之前结束。
// 因此 gops.size() 通常等于关键帧数（最后一个 GOP 可能未被收尾，complete=false）。
struct GopInfo {
    int index = 0;              // 0-based 序号
    int stream_index = -1;      // 所属流索引
    double start_seconds = 0.0; // 起始（首个关键帧）时间戳
    double end_seconds = 0.0;   // 结束时间戳（下一个关键帧；未收尾时为最后帧 + 估测帧间隔）

    int frame_count = 0;        // 帧数
    int64_t byte_count = 0;     // 总字节数

    // 帧类型分布（frame_types_known=false 时只有 i_count 可信，其余归入 unknown_count）
    int i_count = 0;
    int p_count = 0;
    int b_count = 0;
    int unknown_count = 0;

    bool closed_gop = true;     // 是否 closed GOP（以 IDR 开头 → 可独立解码）
    bool complete = false;      // 是否已被下一个关键帧正常收尾
    bool frame_types_known = false;  // I/P/B 是否来自解析器或解码器

    int64_t max_frame_bytes = 0;    // GOP 内最大帧字节数
    double max_frame_seconds = 0.0; // 该最大帧的时间戳
    int64_t max_i_frame_bytes = 0;  // GOP 内最大 I 帧字节数

    double DurationSeconds() const;
    double AverageBitrateKbps() const;
    double AverageFrameBytes() const;
    double AverageFps() const;

    // "50 / 0P / 99B" 形式的帧类型摘要
    std::string FrameTypeSummary() const;
    std::string ToString() const;
};

}  // namespace model
}  // namespace videoeye
