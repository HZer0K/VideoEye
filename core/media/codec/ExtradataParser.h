#pragma once

// Extradata 解析器主体。纯结果类型在 core/media/codec/ExtradataTypes.h。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataTypes.h"

namespace videoeye {

// ==========================================================================
// Extradata 解析器
//
// 职责：
//   1. 识别 extradata 封装格式（Annex B / avcC / hvcC / av1C）
//   2. 统一抽象为 NAL/OBU 列表
//   3. 提取配置记录中的基本信息
//   4. 支持 Annex B ↔ length-prefix 转换
// ==========================================================================
class ExtradataParser {
public:
    // 从 extradata 开始解析
    static ExtradataResult Parse(const uint8_t* extradata, size_t size);

    // 直接指定格式解析。syntax 只对 AnnexB / 长度前缀流有意义（见 NalSyntax 注释）；
    // 配置记录（avcC/hvcC/vvcC）里 NAL 类型由数组头给出，不需要猜。
    static ExtradataResult ParseWithFormat(ExtradataFormat format,
                                           const uint8_t* data, size_t size,
                                           NalSyntax syntax = NalSyntax::Auto);

    // Annex B 流的 NAL 抽取（VVC 必须传 NalSyntax::Vvc，否则类型会读错）
    static std::vector<NalUnit> ExtractAnnBNalUnits(const uint8_t* data, size_t size,
                                                    NalSyntax syntax = NalSyntax::Auto);

    // VVC NAL 单元解析（2 字节 header，type = (byte1 >> 3) & 0x1F）
    static NalUnit ParseVvcNalUnit(const uint8_t* data, size_t size);

    // 转换封装格式（Annex B ↔ length-prefix）
    static std::vector<uint8_t> ConvertAnnexBToLengthPrefix(
        const std::vector<uint8_t>& annex_b);

    static std::vector<uint8_t> ConvertLengthPrefixToAnnexB(
        const std::vector<uint8_t>& length_prefix,
        int prefix_bytes);

    // 从裸 OBU 流（或 av1C 尾部的 configOBUs）中依次抽出所有 OBU。
    // 对外可见：AV1 的序列头往往在 packet 里而不是 extradata 里，
    // 上层需要自己拿一段 OBU 字节来解析。
    static std::vector<ObuUnit> ExtractObuUnits(const uint8_t* data, size_t size);

private:
    // 格式检测
    static ExtradataFormat DetectFormat(const uint8_t* data, size_t size);

    // 具体格式解析器
    static ExtradataResult ParseAnnexB(const uint8_t* data, size_t size,
                                       NalSyntax syntax = NalSyntax::Auto);
    static ExtradataResult ParseAvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseHvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseVvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseAv1C(const uint8_t* data, size_t size);

    // H.264 NAL 单元解析
    static NalUnit ParseH264NalUnit(const uint8_t* data, size_t size);

    // H.265 NAL 单元解析
    static NalUnit ParseHevcNalUnit(const uint8_t* data, size_t size);

    // AV1 OBU 解析
    static ObuUnit ParseAv1Obu(const uint8_t*& ptr, size_t remaining);

    // Annex B 解析辅助
    static const uint8_t* FindStartCode(const uint8_t* pos, const uint8_t* end);
};

} // namespace videoeye
