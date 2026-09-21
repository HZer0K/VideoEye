#pragma once

#include <vector>
#include <cstdint>
#include "core/model/BitstreamInfo.h"

namespace videoeye {
namespace analyzer {

// H.264 码流解析器
// 
// 职责：
//   1. 从 extradata 中解析 H.264 SPS/PPS
//   2. 提取 profile、level、chroma_format、bit_depth 等关键参数
//   3. 解析 VUI（视频单元信息）包括 timing/color/HRD
//   4. 支持 Annex B 和 length-prefix 两种封装格式
// ==========================================================================
class H264BitstreamParser {
public:
    // 从 extradata 开始解析
    static model::H264SpsInfo ParseSpf(const uint8_t* extradata, size_t size);
    
    // 从 extradata 解析 PPS
    static model::H264PpsInfo ParsePps(const uint8_t* extradata, size_t size);
    
    // 从 extradata 解析 VUI
    static model::H264VuiInfo ParseVui(const uint8_t* extradata, size_t size);
    
    // 从 NAL 单元解析（用于实时解析）
    static model::H264SpsInfo ParseFromNalUnit(const utils::NalUnit& nal_unit);
    
    // 从 PPS NAL 单元解析（用于实时解析）
    static model::H264PpsInfo ParsePpsFromNalUnit(const utils::NalUnit& nal_unit);
    
    // 判断是否为 SPS NAL 单元
    static bool IsSpsNalUnit(const utils::NalUnit& nal_unit);
    
    // 判断是否为 PPS NAL 单元
    static bool IsPpsNalUnit(const utils::NalUnit& nal_unit);
    
    // Profile / Level 名称转换（单元测试亦直接调用）
    static std::string GetProfileName(int profile_idc);
    static std::string GetLevelVersion(int level_idc);
    
private:
    // SPS 解析辅助
    static void ParseProfileLevelInfo(uint32_t profile_idc, 
                                      uint32_t level_idc,
                                      int& profile, 
                                      std::string& profile_name,
                                      int& level,
                                      std::string& level_version);
    
    // 取出 RBSP：去掉可能存在的 1 字节 NAL header，再做 emulation prevention 反转义。
    // ExtradataParser 的两条路径对 data 是否含 header 并不一致
    // （AnnexB 剥掉、avcC 保留），这里统一处理。
    static std::vector<uint8_t> GetRbsp(const utils::NalUnit& nal_unit);
    
    // 判断 profile 是否带 chroma_format_idc / bit_depth 等扩展字段
    static bool HasChromaFormatExtension(int profile_idc);
    
    static void SkipScalingList(utils::BitReader& reader, int size);
    static void SkipHrdParameters(utils::BitReader& reader, model::H264VuiInfo& vui);
    static void ParseVuiParameters(utils::BitReader& reader, model::H264VuiInfo& vui);
};

} // namespace analyzer
} // namespace videoeye
