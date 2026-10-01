#include "core/analysis/orchestration/ffmpeg_interrupt.h"

namespace videoeye {
namespace analyzer {

int AvIoInterruptCallback(void* opaque) {
    const auto* state = reinterpret_cast<const AvInterruptState*>(opaque);
    if (state->cancel && state->cancel->load(std::memory_order_acquire)) return 1;
    if (state->deadline_us > 0 && av_gettime() > state->deadline_us) return 1;
    return 0;
}

}  // namespace analyzer
}  // namespace videoeye
