#include "core/ffmpeg_io/FfmpegInterrupt.h"

namespace videoeye {
namespace ffmpeg_io {

int AvIoInterruptCallback(void* opaque) {
    const auto* state = reinterpret_cast<const AvInterruptState*>(opaque);
    if (state->cancel && state->cancel->load(std::memory_order_acquire))
        return 1;
    // 先取一次再比较: 直接写 state->deadline_us > 0 && av_gettime() > state->deadline_us
    // 会读两遍原子，中途被 UI 线程改成 0 时两次读到的不是同一个值。
    const int64_t deadline = state->deadline_us.load(std::memory_order_relaxed);
    if (deadline > 0 && av_gettime() > deadline)
        return 1;
    return 0;
}

void AttachInterrupt(AVFormatContext* fmt, AvInterruptState& state, int64_t timeout_us) {
    if (!fmt)
        return;
    state.deadline_us.store((timeout_us > 0) ? av_gettime() + timeout_us : 0, std::memory_order_relaxed);
    fmt->interrupt_callback.callback = &AvIoInterruptCallback;
    fmt->interrupt_callback.opaque = &state;
}

} // namespace ffmpeg_io
} // namespace videoeye
