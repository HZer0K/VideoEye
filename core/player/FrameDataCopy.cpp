#include "core/player/FrameDataCopy.h"

#include <cstring>

extern "C" {
#include <libavutil/common.h>   // AV_CEIL_RSHIFT
#include <libavutil/pixdesc.h>
}

namespace videoeye {
namespace player {

bool CopyFrameData(const model::FrameData& src, model::FrameData& dst) {
    dst.Clear();

    if (&src == &dst) {
        return true;
    }

    dst.width = src.width;
    dst.height = src.height;
    dst.format = src.format;
    dst.pts = src.pts;
    dst.timestamp = src.timestamp;

    const AVPixFmtDescriptor* desc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src.format));

    bool complete = true;
    for (int i = 0; i < 8; ++i) {
        dst.linesize[i] = src.linesize[i];
        if (!src.data[i] || src.linesize[i] <= 0) {
            dst.data[i] = nullptr;
            continue;
        }

        int plane_height = src.height;
        if (desc && (i == 1 || i == 2)) {
            plane_height = AV_CEIL_RSHIFT(src.height, desc->log2_chroma_h);
        } else if (!desc && (i == 1 || i == 2)) {
            // 不知道色度下采样倍数就别拷：按整高读会越过源缓冲区。
            complete = false;
            continue;
        }

        const int size = src.linesize[i] * plane_height;
        if (size <= 0) {
            continue;
        }

        dst.owned[i].resize(size);
        std::memcpy(dst.owned[i].data(), src.data[i], size);
        dst.data[i] = dst.owned[i].data();
    }

    return complete;
}

} // namespace player
} // namespace videoeye
