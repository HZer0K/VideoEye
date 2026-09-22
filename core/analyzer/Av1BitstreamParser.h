#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "core/model/BitstreamInfo.h"
#include "utils/ExtradataParser.h"
#include "utils/BitReader.h"

namespace videoeye {
namespace analyzer {

// AV1 码流解析器
//
// 职责：
//   1. 解析 Sequence Header OBU（AV1 规范 5.5.1 sequence_header_obu()）
//   2. 提取 profile、level、tier、分辨率、bit depth、color config 等参数
//
// ⚠️ 语法元素顺序以 dav1d 的 parse_seq_hdr() 为准（src/obu.c）。
// 旧实现是把字节拼成 24/16 位整数再移位的“猜字段”写法：
// frame_width_minus_1 按规范是 f(frame_width_bits+1) 的变长字段，
// color_primaries 是 f(8) 而不是 f(3)，且完全没处理
// monochrome / color_description_present_flag / chroma_sample_position /
// separate_uv_delta_q 这些会影响后续位偏移的分支。
// ==========================================================================
class Av1BitstreamParser {
public:
    // 从 extradata 开始解析（av1C 尾部的 configOBUs，或裸 OBU 流）
    static model::Av1SequenceHeaderInfo ParseSequenceHeader(const uint8_t* extradata, size_t size);

    // 从 OBU 单元解析（data 只含 payload，不含 OBU header）
    static model::Av1SequenceHeaderInfo ParseFromObuUnit(const utils::ObuUnit& obu_unit);

    // 判断是否为 Sequence Header OBU
    static bool IsSequenceHeaderObu(const utils::ObuUnit& obu_unit);

    // Profile 名称转换（单元测试亦直接调用）
    static std::string GetProfileName(int profile);

    // 把 seq_level_idx(0..23) 转成 "2.0" / "6.3" 这样的等级串
    static std::string GetLevelString(int seq_level_idx);

private:
    static bool SkipTimingInfo(utils::BitReader& reader,
                              model::Av1SequenceHeaderInfo& sh);
    static bool SkipOperatingPoints(utils::BitReader& reader,
                                   model::Av1SequenceHeaderInfo& sh);
    static bool ParseColorConfig(utils::BitReader& reader,
                                model::Av1SequenceHeaderInfo& sh);
};

} // namespace analyzer
} // namespace videoeye
