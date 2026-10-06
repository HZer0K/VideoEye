#pragma once

#include "core/domain/model/FrameData.h"

namespace videoeye {
namespace player {

// 深拷贝一帧：把 src 的每个平面拷进 dst 自己的 owned 缓冲区，dst 不再引用 src 的内存。
//
// 为什么放在 core/player 而不是 core/domain/model/FrameData 里：
// 平面行数要按色度下采样算（planes 1/2 要右移 log2_chroma_h），这一步只能问
// FFmpeg 的 av_pix_fmt_desc_get —— 而 core/domain 是禁 FFmpeg 的层
// （scripts/check_layering.py 的 NO_FFMPEG，且没有例外通道）。FrameData 因此只当
// 纯数据袋：宽高/format/linesize/data 指针，几何解释交给持有 FFmpeg 的层。
//
// src.format 必须是 AVPixelFormat。取不到描述符时**不猜** 1/2 平面的行数：
// 按整高去拷色度平面会越过源缓冲区（linesize*height > 实际的 linesize*(height>>shift)），
// 所以那种情况下只拷平面 0 并返回 false。
// 返回 true 表示所有非空平面都拷了。
bool CopyFrameData(const model::FrameData& src, model::FrameData& dst);

} // namespace player
} // namespace videoeye
