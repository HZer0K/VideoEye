#pragma once

// 短窗口试解码（切片 D 层 3）。
//
// `AV_PKT_FLAG_KEY` 只是容器声明。索引损坏时它会错标，从半截 GOP 开始解码
// 也不会有任何容器层报错 —— 产物「能打开但花屏」。这一层用真实解码器裁决：
// 从候选时间点 seek 过去，取同流若干包送进解码器，必须真的解出帧且没有损坏
// 标记，才允许把恢复点证据升级为 TrialDecodeOk。
//
// 边界（必须随结论一起报出去，不许当成「已验证可播放」）：
//   * 第一版只覆盖 H.264，且只认 4 字节长度前缀（avcC lengthSizeMinusOne == 3）；
//   * 只看有限包数，不能证明整段 GOP 可解，更不能证明整段产物可解；
//   * seek 仍由 demuxer 完成，索引彻底失效时这里会直接失败 —— 这正是要如实报告的。

#include <atomic>
#include <cstdint>
#include <string>

#include "core/repair/RepairBudget.h"

namespace videoeye {
namespace repair {

struct RepairTrialDecodeOptions {
    // 送多少条同流视频包进解码器。1 帧只能证明「这一帧能解」；
    // 给 8 让她能跨过 B 帧重排，但仍远不足以证明整段 GOP。
    int max_video_packets = 8;
    // 读包总数上限（含其它流），防止在坏文件上无限读。
    int max_packets_read = 256;
    // 允许「包时间早于目标时间」的容差：seek 是 backward 的，首包常常略早。
    double tolerance_seconds = 1.0;
};

struct RepairTrialDecodeResult {
    bool attempted = false; // false = 没试成（编码不支持 / 文件打不开），不是通过
    bool ok = false;
    int video_packets = 0;
    int frames_decoded = 0;
    int corrupt_frames = 0;
    double seek_seconds = 0.0;
    double first_packet_seconds = 0.0;
    std::string error;
    std::string evidence;
};

RepairTrialDecodeResult TrialDecodeVideoAt(const std::string& path, double at_seconds,
                                           const RepairTrialDecodeOptions& options, const std::atomic<bool>* cancel,
                                           const RepairRunBudget* budget);

} // namespace repair
} // namespace videoeye
