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
//     // 网络源例外：每次读包前用 ScanIdleTimeoutUs() 重新武装空闲截止时间
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
#include <libavutil/time.h> // av_gettime
}

namespace videoeye {
namespace ffmpeg_io {

struct AvInterruptState {
    std::atomic<bool>* cancel = nullptr; // 指向取消标志; 非空时取消即中断

    // 绝对截止时间（>0 表示有）。
    //
    // ❗ 必须是原子: 写它的人分处两个线程 —— AttachInterrupt()/探测阶段放宽是 IO
    // 线程在写（回调也在 IO 线程读它），而 MediaPlayer::RequestCancel() 是从 UI
    // 线程写 0（换媒体时让卡住的 read 立刻返回）。TSan 在 Linux CI 上抓到的就是
    // 这一对写写竞争（MediaPlayer.cpp:128 vs FfmpegInterrupt.cpp:15）。
    // 单次读写要"看到完整值"而不是撕裂的字面值，所以整块用 relaxed 的原子就够
    // （它不参与任何其它数据的同步，只要求自己不撕裂、不被优化掉）。
    std::atomic<int64_t> deadline_us{0};
};

// 返回 1 表示应中断当前 IO。opaque 为 AvInterruptState*。
int AvIoInterruptCallback(void* opaque);

// 把中断回调装到 fmt 上，并把截止时间设为 "现在 + timeout_us"。
// timeout_us <= 0 表示不设超时（只保留取消响应）。
// state 的生命周期由调用方保证，必须覆盖整个 IO 过程。
void AttachInterrupt(AVFormatContext* fmt, AvInterruptState& state, int64_t timeout_us);

// 打开 / 探测阶段的绝对超时（单位微秒）。扫描阶段不依赖它们，仅响应取消。
constexpr int64_t kOpenTimeoutUs = 15'000'000;  // 打开输入的最大等待
constexpr int64_t kProbeTimeoutUs = 30'000'000; // 探测流信息的最大等待

// 逐包扫描阶段的 IO 空闲超时（网络源默认值，单位微秒）。
//
// 为什么必须有：本地文件的 av_read_frame 一定会返回（读到 EOF 或真 IO 错误），
// 而网络源在服务端断流/半开连接时可能永远不返回。外层（QcRunner）的
// io_idle_timeout_ms 只能放弃等待并 detach，那颗引擎线程仍然卡在 IO 里，
// 继续占着 AVFormatContext、解码器与内存 —— 关窗/退出进程照样可能被它拖住。
// 只有把截止时间重新装进中断回调，才能真正把读操作踢出来。
constexpr int64_t kNetworkScanIdleTimeoutUs = 15'000'000;

// 扫描阶段该用多大的 IO 空闲超时（单位微秒，0 = 不设）。
//
// option_ms > 0 时以调用方配置为准（QC 的 QcRunBudgets::io_idle_timeout_ms 会透传到
// AnalysisOptions）；否则网络源退回默认值，本地输入不设超时 —— 长本地文件的正常
// 读取不能被任何固定超时误杀。
inline int64_t ScanIdleTimeoutUs(bool network_input, int option_ms) {
    if (option_ms > 0)
        return static_cast<int64_t>(option_ms) * 1000;
    return network_input ? kNetworkScanIdleTimeoutUs : 0;
}

} // namespace ffmpeg_io
} // namespace videoeye
