#pragma once

#include <vector>
#include <cstdint>
#include "core/model/BitstreamInfo.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

// H.265 (HEVC) 码流解析器
// 
// 职责：
//   1. 从 extradata 中解析 HEVC VPS/SPS/PPS
//   2. 提取 profile tier level、bit depth、chroma、CTU size 等关键参数
//   3. 解析 SEI 消息（Mastering Display、Content Light Level）
//   4. 支持 Annex B 和 length-prefix 两种封装格式
// ==========================================================================
class HevcBitstreamParser {
public:
    // 从 extradata 开始解析
    static model::HevcVpsInfo ParseVps(const uint8_t* extradata, size_t size);
    
    static model::HevcSpsInfo ParseSpf(const uint8_t* extradata, size_t size);
    
    static model::HevcPpsInfo ParsePps(const uint8_t* extradata, size_t size);
    
    // 从 NAL 单元解析（用于实时解析）
    static model::HevcVpsInfo ParseVpsFromNalUnit(const utils::NalUnit& nal_unit);
    static model::HevcSpsInfo ParseSpfFromNalUnit(const utils::NalUnit& nal_unit);
    static model::HevcPpsInfo ParsePpsFromNalUnit(const utils::NalUnit& nal_unit);
    
    // 判断是否为 VPS/SPS/PPS NAL 单元
    static bool IsVpsNalUnit(const utils::NalUnit& nal_unit);
    static bool IsSpsNalUnit(const utils::NalUnit& nal_unit);
    static bool IsPpsNalUnit(const utils::NalUnit& nal_unit);
    
    // 解析 SEI 消息
    static std::vector<model::HevcSeiMessage> ParseSeiMessages(const uint8_t* data, size_t size);
    
    // Profile/Tier 名称转换（单元测试亦直接调用）
    static std::string GetProfileName(int general_profile_idc);
    static std::string GetTierName(int general_tier_flag);
    static std::string GetLevelVersion(uint32_t general_level_idc);
};

} // namespace analyzer
} // namespace videoeye
