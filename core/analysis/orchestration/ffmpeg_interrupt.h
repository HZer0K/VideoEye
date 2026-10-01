#pragma once

// FFmpeg 阻塞 IO 的中断机制（被多个分析器共享）。
//
// 让 avformat_open_input / avformat_find_stream_info / av_read_frame 在
// 「打开超时」或「用户取消」时能及时从阻塞的网络 IO 中退出 —— 否则离线分析
// 遇到不可达的 RTSP/HTTP/卡死的裸流会长时间卡死，进而让关闭流程（WaitForAll）
// 在 join 受管线程时永久挂起。
//
// 用法（在分析器内部）:
//     analyzer::AvInterruptState interrupt;
//     interrupt.cancel = cancel_flag.get();          // 指向 TaskManager 的取消标志
//     interrupt.deadline_us = av_gettime() + analyzer::kOpenTimeoutUs;
//     fmt->interrupt_callback.callback = &analyzer::AvIoInterruptCallback;
//     fmt->interrupt_callback.opaque    = &interrupt;
// 之后在「进入逐包扫描」时把 deadline_us 清零、只保留取消响应，避免长本地文件
// 被打开阶段的早期超时误杀。

#include <atomic>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>  // av_gettime
}

namespace videoeye {
namespace analyzer {

struct AvInterruptState {
    std::atomic<bool>* cancel = nullptr;  // 指向取消标志; 非空时取消即中断
    int64_t deadline_us = 0;              // >0 表示有绝对截止时间
};

// 返回 1 表示应中断当前 IO。opaque 为 AvInterruptState*。
int AvIoInterruptCallback(void* opaque);

// 打开 / 探测阶段的绝对超时（单位微秒）。扫描阶段不依赖它们，仅响应取消。
constexpr int64_t kOpenTimeoutUs = 15'000'000;   // 打开输入的最大等待
constexpr int64_t kProbeTimeoutUs = 30'000'000;  // 探测流信息的最大等待

}  // namespace analyzer
}  // namespace videoeye
