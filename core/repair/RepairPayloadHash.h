#pragma once

// 跨容器音频载荷哈希的「访问单元规范化」。
//
// MP4 / Matroska 里的 AAC 包是裸访问单元（AudioSpecificConfig 在 extradata），
// MPEG-TS 里同一个访问单元外面还套着 7 字节（无 CRC）或 9 字节（有 CRC）的
// ADTS 头。逐包对原始字节做 FNV-1a 时，一次完全合法的无损 MP4→TS 重封装
// 会因每个包多出这个头而被判「音频载荷哈希不一致」，进而整体验证失败、
// 引擎拒绝发布。这里只对齐「参与哈希的字节序列」，不改动写进产物的任何字节。
//
// ⚠️ 只用于取证哈希，不能拿它当解码输入：ADTS 头里还有采样率 / 声道等参数，
// 解码必须继续使用 demuxer 给出的原始包。
//
// 为什么放在 header：执行器（算输入侧哈希）和验证器（算产物侧哈希）必须用
// **同一个**规范化函数，任何一侧漏用都会把合法重封装判红。

#include <cstddef>
#include <cstdint>

#include "core/repair/RepairUtils.h"

// ⚠️ 本文件是 VideoEyeRepair 的公共头，**不能 include FFmpeg 头**：
// 链接可见性门禁会要求把 FFmpeg 从 PRIVATE 提到 PUBLIC，那会把编解码头泄漏给
// 所有消费者。codec 判定改由调用方（各自已 include FFmpeg 的 .cpp）传 bool 进来。

namespace videoeye {
namespace repair {

// ADTS 固定头的同步字：0xFFF（12 bit）+ layer=00（2 bit）。
// ID（MPEG-2/4）与 protection_absent 位随意，故用 0xF6 掩码后比较 0xF0。
inline bool LooksLikeAdtsHeader(const std::uint8_t* data, std::size_t size) {
    return data != nullptr && size >= 7 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0;
}

// 返回单个 ADTS 帧的总长度（含头）；0 = 头部不完整或长度字段不可用。
inline std::size_t AdtsFrameLength(const std::uint8_t* data, std::size_t size) {
    if (!LooksLikeAdtsHeader(data, size))
        return 0;
    return (static_cast<std::size_t>(data[3] & 0x03) << 11) | (static_cast<std::size_t>(data[4]) << 3) |
           (static_cast<std::size_t>(data[5]) >> 5);
}

// 把一个音频包按「裸访问单元序列」喂进 FNV-1a 状态。
//
// 一个 PES 包里可能装了多个 ADTS 帧，所以不能只剥一次头就返回单一区间：
// 逐帧剥离后依次累计，才能与 MP4 侧「每包一个 AU」的字节序列严格对齐
// （FNV 是流式累计，包边界不参与哈希，因此两侧结果一致）。
// 非 AAC / 不像 ADTS / 尾部残帧一律按原始字节累计，绝不静默吞掉内容。
// is_aac：调用方用 `codec_id == AV_CODEC_ID_AAC` 传入，本头保持零 FFmpeg 依赖。
inline uint64_t UpdateAudioPayloadHash(uint64_t state, const std::uint8_t* data, std::size_t size, bool is_aac) {
    if (!is_aac || data == nullptr || size == 0 || !LooksLikeAdtsHeader(data, size))
        return Fnv1a64Update(state, data, size);

    std::size_t pos = 0;
    while (LooksLikeAdtsHeader(data + pos, size - pos)) {
        const std::size_t header = (data[pos + 1] & 0x01) != 0 ? 7u : 9u; // protection_absent=1 → 无 CRC
        const std::size_t frame = AdtsFrameLength(data + pos, size - pos);
        if (frame < header + 1 || frame > size - pos)
            break; // 尾部不是完整 ADTS 帧：原样累计，不猜
        state = Fnv1a64Update(state, data + pos + header, frame - header);
        pos += frame;
    }
    if (pos < size)
        state = Fnv1a64Update(state, data + pos, size - pos);
    return state;
}

// 结论层要写清楚「这次哈希到底比的是什么」，避免把规范化后的比对说成
// 「原始包字节一致」。非 AAC 轨没有规范化能力，跨封装时只能标未验证。
inline const char* AudioHashScope(bool is_aac) {
    return is_aac ? "normalized_aac_access_unit" : "raw_packet_bytes";
}

} // namespace repair
} // namespace videoeye
