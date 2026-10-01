#include "core/ffmpeg_io/FfmpegInterrupt.h"

namespace videoeye {
namespace ffmpeg_io {

int AvIoInterruptCallback(void* opaque) {
    const auto* state = reinterpret_cast<const AvInterruptState*>(opaque);
    if (state->cancel && state->cancel->load(std::memory_order_acquire)) return 1;
    if (state->deadline_us > 0 && av_gettime() > state->deadline_us) return 1;
    return 0;
}

void AttachInterrupt(AVFormatContext* fmt, AvInterruptState& state, int64_t timeout_us) {
    if (!fmt) return;
    state.deadline_us = (timeout_us > 0) ? av_gettime() + timeout_us : 0;
    fmt->interrupt_callback.callback = &AvIoInterruptCallback;
    fmt->interrupt_callback.opaque = &state;
}

}  // namespace ffmpeg_io
}  // namespace videoeye
