#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/BitstreamInfo.h"
#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataTypes.h"

namespace videoeye {

// ==========================================================================
// VVC (H.266) 码流解析器
//
// 职责：
//   1. 从 VPS / SPS / PPS NAL 单元解析编码参数（宽高、位深、色度、CTU、profile/level）
//   2. 解析 VUI 里的色彩描述（primaries / transfer / matrix / full range）
//
// 语法顺序以 FFmpeg libavcodec/cbs_h266_syntax_template.c 为准（与 H.266 7.3.2.x
// 逐条对应），不要凭印象改字段顺序。
//
// ⚠️ 与 H.264 / HEVC 的关键差异：VVC 的 NAL header 是 2 字节，
//    nal_unit_type = (byte1 >> 3) & 0x1F（HEVC 是 (byte0 >> 1) & 0x3F）。
//    VPS=14 / SPS=15 / PPS=16。
//
// 覆盖范围：单层码流完整解析；多层码流解析到 PTL 为止（多层 + 非
// each_layer_is_an_ols 的 OLS/DPB/HRD 部分需要 num_multi_layer_olss 推导，
// 使用场景极少，遇到时保留已解析字段并停止）。
// ==========================================================================
class VvcBitstreamParser {
public:
    static model::VvcVpsInfo ParseVpsFromNalUnit(const videoeye::NalUnit& nal_unit);
    static model::VvcSpsInfo ParseSpsFromNalUnit(const videoeye::NalUnit& nal_unit);
    static model::VvcPpsInfo ParsePpsFromNalUnit(const videoeye::NalUnit& nal_unit);

    static bool IsVpsNalUnit(const videoeye::NalUnit& nal_unit);
    static bool IsSpsNalUnit(const videoeye::NalUnit& nal_unit);
    static bool IsPpsNalUnit(const videoeye::NalUnit& nal_unit);

    // Profile / Level 名称转换（单元测试亦直接调用）
    static std::string GetProfileName(int general_profile_idc);
    // general_level_idc -> "3.1"（H.266 A.4：major * 16 + minor * 3）
    static std::string GetLevelVersion(uint32_t general_level_idc);

    // 取出 RBSP：NalUnit::data 只含 payload，只需做 emulation prevention 反转义。
    // 公开是因为凡"解 VVC 位流"的调用方都要先过这一步（PH 前缀、slice header），
    // 各抄一份 unescape 迟早会漏掉 00 00 03 的边界处理。
    static std::vector<uint8_t> GetRbsp(const videoeye::NalUnit& nal_unit);

private:
    struct TimingHrdContext {
        bool nal_hrd_present = false;
        bool vcl_hrd_present = false;
        bool du_hrd_present = false;
        uint32_t hrd_cpb_cnt_minus1 = 0;
    };

    static void SkipGeneralConstraintsInfo(videoeye::BitReader& reader);
    static void SkipProfileTierLevel(videoeye::BitReader& reader, bool profile_tier_present,
                                     int max_num_sub_layers_minus1);
    // 返回解析出的 PTL 关键字段（profile_tier_present 时有效）
    static void ParseProfileTierLevel(videoeye::BitReader& reader, bool profile_tier_present,
                                      int max_num_sub_layers_minus1, int* profile_idc, int* tier_flag,
                                      uint32_t* level_idc, int* num_sub_profiles);
    static void SkipDpbParameters(videoeye::BitReader& reader, int max_sublayers_minus1, bool sublayer_info_flag);
    static void SkipGeneralTimingHrdParameters(videoeye::BitReader& reader, TimingHrdContext* ctx);
    static void SkipSubLayerHrdParameters(videoeye::BitReader& reader, const TimingHrdContext& ctx);
    static void SkipOlsTimingHrdParameters(videoeye::BitReader& reader, int first_sublayer, int max_sublayers_minus1,
                                           const TimingHrdContext& ctx);
    // record != nullptr 时把这条 RPL 的结构一并记下来（slice 侧 ref_pic_lists()
    // 要靠它算 num_ltrp_entries）。读出错或超出记录上限时 record->recorded = false，
    // 调用方据此知道"这段不可信"，而不是拿到一份半截数据。
    static void SkipRefPicListStruct(videoeye::BitReader& reader, int poc_lsb_bits, bool long_term_ref_pics,
                                     bool inter_layer_prediction, bool weighted_pred,
                                     model::VvcRefPicListStruct* record = nullptr);
    static void ParseVuiParameters(videoeye::BitReader& reader, model::VvcVuiInfo& vui);
};

} // namespace videoeye
