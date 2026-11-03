#pragma once

#include <vector>

namespace videoeye {
namespace player {

/// 一种可选的无压缩 PCM 采样格式。
struct RawPcmFormatOption {
    const char* label;   ///< 弹窗里显示的名字，形如 "s16le (16-bit little-endian)"
    const char* demuxer; ///< FFmpeg 的 AVInputFormat::name，形如 "s16le"
};

/// 支持的裸 PCM 格式表 —— "下拉框显示什么 / 用哪个 demuxer 打开"的唯一事实源。
///
/// ⚠️ demuxer 必须是 FFmpeg 的**输入**格式名。pcm_s16le 是*编码器*名，拿它去
/// av_find_input_format 只会拿到 nullptr，打开会在"找不到裸流 demuxer"处直接失败
/// （2026-11 之前一直是这个状态，即裸 PCM 从来没能打开过）。
/// tests/unit/test_player_open_async.cpp 的 RawPcmFormatCatalog 用例会遍历本表，
/// 逐个交给 av_find_input_format 验证 —— 改表忘了同步名字会当场红。
const std::vector<RawPcmFormatOption>& RawPcmFormatOptions();

} // namespace player
} // namespace videoeye
