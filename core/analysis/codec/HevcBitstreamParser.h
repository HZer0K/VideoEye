#pragma once

#include "core/domain/model/BitstreamInfo.h"
#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataTypes.h"
#include <cstdint>
#include <vector>

namespace videoeye {

// H.265 (HEVC) 码流解析器
//
// 职责：
//   1. 从 extradata 中解析 HEVC VPS/SPS/PPS
//   2. 提取 profile tier level、bit depth、chroma、CTU size 等关键参数
//   3. 支持 Annex B 和 length-prefix 两种封装格式
//
// 不做的：SEI 消息解析。HDR 静态/动态元数据（ST 2086、MaxCLL、HDR10+、DV）
// 一律走 FFmpeg 的 side data，不自研 SEI 解析（见 README 的"不自研"清单）。
// 历史上这里有个 ParseSeiMessages()，全项目零调用、产出的 sei_messages 恒空，
// 看起来"支持 SEI"实则不产出任何结论，已删除。
// ==========================================================================
class HevcBitstreamParser {
public:
    // 从 extradata 开始解析
    static model::HevcVpsInfo ParseVps(const uint8_t* extradata, size_t size);

    static model::HevcSpsInfo ParseSpf(const uint8_t* extradata, size_t size);

    static model::HevcPpsInfo ParsePps(const uint8_t* extradata, size_t size);

    // 从 NAL 单元解析（用于实时解析）
    static model::HevcVpsInfo ParseVpsFromNalUnit(const videoeye::NalUnit& nal_unit);
    static model::HevcSpsInfo ParseSpfFromNalUnit(const videoeye::NalUnit& nal_unit);
    static model::HevcPpsInfo ParsePpsFromNalUnit(const videoeye::NalUnit& nal_unit);

    // 零拷贝入口（逐包扫描用，理由同 H264BitstreamParser::ParseFromNal）：
    // payload 是未反转义的 NAL 载荷，不含起始码与 NAL header。
    static model::HevcVpsInfo ParseVpsFromNal(uint8_t nal_type, const uint8_t* payload, size_t size);
    static model::HevcSpsInfo ParseSpfFromNal(uint8_t nal_type, const uint8_t* payload, size_t size);
    static model::HevcPpsInfo ParsePpsFromNal(uint8_t nal_type, const uint8_t* payload, size_t size);

    // 判断是否为 VPS/SPS/PPS NAL 单元
    static bool IsVpsNalUnit(const videoeye::NalUnit& nal_unit);
    static bool IsSpsNalUnit(const videoeye::NalUnit& nal_unit);
    static bool IsPpsNalUnit(const videoeye::NalUnit& nal_unit);

    // Profile/Tier 名称转换（单元测试亦直接调用）
    static std::string GetProfileName(int general_profile_idc);
    static std::string GetTierName(int general_tier_flag);
    static std::string GetLevelVersion(uint32_t general_level_idc);

private:
    // NalUnit::data 已不含 NAL header，这里只做 emulation prevention 反转义
    static std::vector<uint8_t> GetRbsp(const videoeye::NalUnit& nal_unit);
    // 同上，但从裸载荷开始（NalView 路径）
    static std::vector<uint8_t> GetRbsp(const uint8_t* payload, size_t size);

    // profile_tier_level() —— 7.3.3
    static void ParseProfileTierLevel(videoeye::BitReader& reader, int max_sub_layers_minus1, int& profile_space,
                                      int& tier_flag, int& profile_idc, uint32_t& level_idc,
                                      std::vector<bool>& sub_layer_profile_present,
                                      std::vector<bool>& sub_layer_level_present);

    static void SkipScalingListData(videoeye::BitReader& reader);
    static void SkipHrdParameters(videoeye::BitReader& reader, int max_sub_layers_minus1);
    // hrd_parameters 里 nal / vcl 两份 CPB 列表结构完全相同，抽出来避免抄两遍。
    // 返回 false 表示位流已经失效（读越界）。
    static bool SkipOneHrdCpbList(videoeye::BitReader& reader, uint32_t cpb_cnt_minus1, bool sub_pic);
    static void SkipShortTermRefPicSets(videoeye::BitReader& reader, int num_sets);
    static void ParseVuiParameters(videoeye::BitReader& reader, model::HevcSpsInfo& sps, int max_sub_layers_minus1);
};

} // namespace videoeye
