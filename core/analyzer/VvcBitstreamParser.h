#pragma once

#include <vector>
#include <cstdint>
#include "core/model/BitstreamInfo.h"

namespace videoeye {
namespace analyzer {

// VVC (H.266) 码流解析器 - 简化版
// 
// 职责：
//   1. 从 extradata 中解析 VVC VPS/SPS 基本参数
//   2. 提取 profile、level、tier、bit depth 等关键信息
// ==========================================================================
class VvcBitstreamParser {
public:
    // 从 extradata 开始解析
    static model::VvcVpsInfo ParseVps(const uint8_t* extradata, size_t size);
    
    static model::VvcSpsInfo ParseSpf(const uint8_t* extradata, size_t size);
    
private:
    // Profile/Tier 名称转换（简化版）
    static std::string GetProfileName(int general_profile_idc);
    static std::string GetTierName(int general_tier_flag);
};

} // namespace analyzer
} // namespace videoeye
