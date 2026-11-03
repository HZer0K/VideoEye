#pragma once

// FFmpeg 输入选项的构造（跨层共享）。
//
// 本目录是叶子模块（VideoEyeFfmpegIo）: 只依赖 FFmpeg 公共头、不 include 任何 core/ 层
// 的头, 谁都能安全依赖。放这里的理由和 FfmpegInterrupt 一样 —— 同一件事有两个调用方
// （媒体信息探测 / 播放器的裸 PCM 打开）, 各写一份必然漂移。2026-11 就是这样漂的:
// 两边都写着 av_dict_set("channels"), 而 FFmpeg 8 的 pcm demuxer 早就不认这个选项,
// 传了不报错也不生效, 打开后一律按单声道解、时长被算成 1/N。

#include <string>

namespace videoeye {
namespace ffmpeg_io {

/// 声道数 → FFmpeg 的 ch_layout 选项字符串（1 声道 "1c"、立体声 "2c"、5.1 "6c"）。
///
/// "Nc" 是 av_channel_layout_from_string 认的"只给声道数、不指定具体摆位"写法;
/// 6c / 8c 会被 FFmpeg 补成 5.1 / 7.1。
///
/// @return 空串表示"不要设这个选项"（channels <= 0）。调用方必须据此跳过 av_dict_set:
///         写进去的 "0c" 会让 avformat_open_input 直接返回 AVERROR(EINVAL)。
///
/// ⚠️ 不要改回 "channels": FFmpeg 8 的 pcm demuxer 已移除该 AVOption。
std::string ChannelLayoutOption(int channels);

} // namespace ffmpeg_io
} // namespace videoeye
