#include "core/player/RawPcmFormats.h"

namespace videoeye {
namespace player {

const std::vector<RawPcmFormatOption>& RawPcmFormatOptions() {
    // 顺序即弹窗里的顺序（小端/有符号的常见格式排在前面）。
    // label 里必须带着自己的 demuxer 名: 有单测按这一点配对校验。
    static const std::vector<RawPcmFormatOption> kOptions = {{"s16le (16-bit little-endian)", "s16le"},
                                                             {"s16be (16-bit big-endian)", "s16be"},
                                                             {"u8 (8-bit unsigned)", "u8"},
                                                             {"s24le (24-bit little-endian)", "s24le"},
                                                             {"s24be (24-bit big-endian)", "s24be"},
                                                             {"s32le (32-bit little-endian)", "s32le"},
                                                             {"f32le (32-bit float little-endian)", "f32le"},
                                                             {"f32be (32-bit float big-endian)", "f32be"}};
    return kOptions;
}

} // namespace player
} // namespace videoeye
