#pragma once

// FFmpeg 阻塞 IO 的中断与超时机制（跨层共享的基础设施）。
//
// 本文件所在目录是一个**叶子模块**（VideoEyeFfmpegIo）：只依赖 FFmpeg 公共头，
// 不 include 任何 core/ 层的头，谁都能安全地依赖它。分析、播放、导出三条链路都要
// 靠它让 avformat_open_input / avformat_find_stream_info / av_read_frame 在
// 「用户取消」或「打开超时」时及时从阻塞的网络 IO 中退出来。
//
// 为什么值得单独成模块: "取消能不能及时生效"对每个碰 FFmpeg 的模块都是同一件事。
// 它以前住在 core/analysis/orchestration（导出层够不着），于是导出链路只能自己
// 在 av_read_frame 返回之后查标志 —— 输入是网络地址/管道时，那个返回永远不会来。
//
// 用法:
//     ffmpeg_io::AvInterruptState interrupt;
//     interrupt.cancel = cancel_flag.get();                  // 可为空(只靠超时)
//     ffmpeg_io::AttachInterrupt(fmt, interrupt, ffmpeg_io::kOpenTimeoutUs);
//     avformat_open_input(&fmt, ...);
//     ...
//     interrupt.deadline_us = av_gettime() + ffmpeg_io::kProbeTimeoutUs;   // 探测阶段放宽
//     ...
//     interrupt.deadline_us = 0;   // 进入逐包扫描: 只响应取消
//
// ⚠️ 两个必须遵守的前提:
//   1) 回调要在**打开之前**就装好，所以 fmt 必须由调用方自己
//      avformat_alloc_context() 出来再交给 avformat_open_input —— 传 nullptr 让
//      FFmpeg 自己分配，就没有地方装回调了。
//   2) state 的生命周期要覆盖整个 IO 过程（open + 探测 + 读取），
//      因为 opaque 是指向它的裸指针。

#include <atomic>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>  // av_gettime
}

namespace videoeye {
namespace ffmpeg_io {

struct AvInterruptState {
    std::atomic<bool>* cancel = nullptr;  // 指向取消标志; 非空时取消即中断
    int64_t deadline_us = 0;              // >0 表示有绝对截止时间
};

// 返回 1 表示应中断当前 IO。opaque 为 AvInterruptState*。
int AvIoInterruptCallback(void* opaque);

// 把中断回调装到 fmt 上，并把截止时间设为 "现在 + timeout_us"。
// timeout_us <= 0 表示不设超时（只保留取消响应）。
// state 的生命周期由调用方保证，必须覆盖整个 IO 过程。
void AttachInterrupt(AVFormatContext* fmt, AvInterruptState& state, int64_t timeout_us);

// 打开 / 探测阶段的绝对超时（单位微秒）。扫描阶段不依赖它们，仅响应取消。
constexpr int64_t kOpenTimeoutUs = 15'000'000;   // 打开输入的最大等待
constexpr int64_t kProbeTimeoutUs = 30'000'000;  // 探测流信息的最大等待

}  // namespace ffmpeg_io
}  // namespace videoeye
