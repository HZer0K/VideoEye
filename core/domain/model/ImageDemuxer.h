#pragma once

#include <string_view>

namespace videoeye::model {

inline bool IsImageDemuxer(std::string_view name) {
    return name == "gif" || name == "apng" || name == "png_pipe" || name == "jpeg_pipe" || name == "bmp_pipe" ||
           name == "webp_pipe" || name == "tiff_pipe";
}

} // namespace videoeye::model
