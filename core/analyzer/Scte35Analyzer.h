#pragma once

// SCTE-35 (ANSI/SCTE 35) splice_info_section 的二进制解析。
//
// 为什么单列一个模块：广告插入点是直播/OTT 交付里最"错了要赔钱"的元数据 ——
// splice_time 差一帧、out/in 弄反、缺 duration，下游 SCTE-104 → SCTE-35 的
// 拼接就全乱。这一段语法纯比特流解析，跟 FFmpeg 无关，所以做成无状态静态函数，
// 单测可以直接喂一段字节验证。
//
// 规范要点（SCTE-35 2023）:
//   splice_info_section: table_id=0xFC，section_length 从 protocol_version 起算，
//   splice_command_type 决定命令体；末尾是 CRC_32(MPEG-2)。
//   splice_time 的 pts_time 是 33 bit，单位是 1/90000 秒，
//   实际时间 = (pts_adjustment + pts_time) mod 2^33 / 90000。
//
// FFmpeg 侧（枚举 data 流、喂包、metadata 采集）在 core/analyzer/AuxDataAnalyzer.h。

#include <cstddef>
#include <cstdint>

#include "core/model/AuxiliaryDataInfo.h"

namespace videoeye {
namespace analyzer {

struct Scte35Options {
    // 校验 section 末尾的 CRC_32（MPEG-2 多项式）。关闭后仍会解析字段。
    bool verify_crc = true;
    // 包载荷不是从 0xFC 开始时（前面有 pointer_field / 对齐字节），
    // 是否在前 16 字节内找 table_id 0xFC。
    bool scan_for_section_start = true;
};

class Scte35Analyzer {
public:
    // 解析一段 splice_info_section。成功返回 true（cue.valid=true）。
    // 失败时 cue.parse_error 是可读原因，cue.valid=false。
    static bool ParseSection(const uint8_t* data, size_t size, model::Scte35Cue& cue,
                             const Scte35Options& options = Scte35Options{});

    // MPEG-2 CRC-32（多项式 0x04C11DB7，初值全 1，不反转、不取反）
    static uint32_t Crc32Mpeg2(const uint8_t* data, size_t size);

private:
    Scte35Analyzer() = delete;
};

}  // namespace analyzer
}  // namespace videoeye
