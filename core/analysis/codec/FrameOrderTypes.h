#pragma once

// H.264 / HEVC 帧级 POC 追踪器共用的入口参数。
//
// 为什么单独成文件：`FrameOrderTracker`（按编码分派的外观）要同时 include
// 两个追踪器的头，而两个追踪器的头又要引用这些类型 —— 放在任一方都会成环。
// 这里只放"两种编码语义完全一致"的东西，不放任何编码相关字段。

#include <cstddef>

namespace videoeye {

// 码流里 NAL 单元的封装方式。
//
// 与 ParameterSetTracker::Framing 同义：定长长度前缀 vs Annex B 起始码。
// 两种编码的判定逻辑一致（连"长度前缀恰好落在 0x100..0x1FF 会被误认成起始码"
// 这个坑都一样），所以共用一份枚举，不各写一套。
enum class NalFraming {
    Auto = 0,      // 逐包判定（容器没给前缀宽度时才用）
    AnnexB,        // 00 00 01 / 00 00 00 01
    LengthPrefixed // 1/2/4 字节长度前缀
};

struct FrameOrderOptions {
    // 最多把多少帧记进 FrameOrderTimeline::frames。
    // 统计量（I/P/B、重排、GOP）不受此上限影响，只有逐帧明细受限。
    size_t max_frames = 4096;
};

} // namespace videoeye
