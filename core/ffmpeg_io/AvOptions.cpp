#include "core/ffmpeg_io/AvOptions.h"

namespace videoeye {
namespace ffmpeg_io {

std::string ChannelLayoutOption(int channels) {
    if (channels <= 0)
        return std::string();
    return std::to_string(channels) + "c";
}

} // namespace ffmpeg_io
} // namespace videoeye
