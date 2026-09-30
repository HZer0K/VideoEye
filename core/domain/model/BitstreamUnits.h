#pragma once

// 码流单元（NAL / OBU）的**纯结果类型**。
//
// 与 core/media/codec/ExtradataTypes.h 里那两个同名结构的区别：
//   * media 层那两个是**解析器内部**的载体（utils::NalUnit / utils::ObuUnit），
//     由 ExtradataParser 生产，也只喂给 codec parser；
//   * 这里是**对外暴露**的结果快照，字段相同但归 domain 所有，
//     所以 BitstreamInfo.h（以及报告导出、UI）不必反向 include media。
//
// 转换方向固定为 media -> domain，由分析器（BitstreamAnalyzer）在把解析结果
// 写进 model::BitstreamAnalysisResult 时完成；domain 不知道 media 的存在。
//
// 不变量与 utils 版保持一致：`data` 只含 payload，不带起始码、不带 NAL/OBU header。

#include <cstdint>
#include <utility>
#include <vector>

namespace videoeye {
namespace model {

// NAL 单元（H.264 / HEVC / VVC）
struct NalUnit {
    uint8_t type = 0;              // NAL 单元类型
    uint32_t size = 0;             // payload 大小（= data.size()）
    std::vector<uint8_t> data;     // RBSP payload（不含起始码、不含 NAL header）
    bool is_idr = false;           // H.264: IDR 帧标记
    bool is_keyframe = false;      // 通用关键帧标记

    NalUnit() = default;

    // 便于测试与手工构造：(type, size, data, is_idr, is_keyframe)
    NalUnit(uint8_t t, size_t s, std::vector<uint8_t> d, bool idr, bool keyframe)
        : type(t), size(static_cast<uint32_t>(s)), data(std::move(d)),
          is_idr(idr), is_keyframe(keyframe) {}
};

// OBU 单元（AV1）
struct ObuUnit {
    uint8_t type = 0;              // obu_type（1 = OBU_SEQUENCE_HEADER）
    uint32_t size = 0;             // payload 大小（= data.size()）
    std::vector<uint8_t> data;     // OBU payload（不含 header / 长度 / 扩展头）
    bool has_extension_header = false;  // obu_extension_flag
    bool is_sequence_header = false;
    uint8_t temporal_id = 0;       // 扩展头里的 temporal_id（无扩展头时为 0）
    uint8_t spatial_id = 0;        // 扩展头里的 spatial_id

    ObuUnit() = default;

    // 便于测试与手工构造：(type, size, data, has_extension_header, is_sequence_header)
    ObuUnit(uint8_t t, size_t s, std::vector<uint8_t> d, bool ext_header, bool seq_header)
        : type(t), size(static_cast<uint32_t>(s)), data(std::move(d)),
          has_extension_header(ext_header), is_sequence_header(seq_header) {}
};

} // namespace model
} // namespace videoeye
