#pragma once

#include <vector>
#include <cstdint>
#include "core/model/BitstreamInfo.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

// AV1 码流解析器
// 
// 职责：
//   1. 从 extradata 中解析 AV1 Sequence Header OBU
//   2. 提取 profile、level、bit depth、color config 等关键参数
//   3. 解析 Film Grain、HDR metadata
// ==========================================================================
class Av1BitstreamParser {
public:
    // 从 extradata 开始解析
    static model::Av1SequenceHeaderInfo ParseSequenceHeader(const uint8_t* extradata, size_t size);
    
    // 从 OBU 单元解析
    static model::Av1SequenceHeaderInfo ParseFromObuUnit(const utils::ObuUnit& obu_unit);
    
    // 判断是否为 Sequence Header OBU
    static bool IsSequenceHeaderObu(const utils::ObuUnit& obu_unit);
    
private:
    // Profile 名称转换
    static std::string GetProfileName(int profile);
};

} // namespace analyzer
} // namespace videoeye
