#pragma once

#include "core/domain/model/BitstreamInfo.h"
#include <cstdint>
#include <vector>
// 本 parser 的输入是 media 层解析出的 videoeye::NalUnit；BitstreamInfo.h 不再间接
// 带它（domain 不反向依赖 media），所以这里显式 include。
#include "core/media/codec/BitReader.h"
#include "core/media/codec/ExtradataTypes.h"

namespace videoeye {

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
    static model::H264SpsInfo ParseFromNalUnit(const videoeye::NalUnit& nal_unit);

    // 从 PPS NAL 单元解析（用于实时解析）
    static model::H264PpsInfo ParsePpsFromNalUnit(const videoeye::NalUnit& nal_unit);

    // 零拷贝入口：逐包扫描（参数集跟踪 / POC 跟踪）直接喂 NalView 的
    // payload/size，省掉每包每 NAL 一次 vector 分配 + 整段载荷复制。
    //
    // payload 是**未反转义**的 NAL 载荷（不含起始码、不含 NAL header），
    // 与 NalView 的约定一致；反转义在内部做。
    // nal_type 只为把"这是不是 SPS/PPS"的判断留在解析器内部（误喂也只会返回空）。
    static model::H264SpsInfo ParseFromNal(uint8_t nal_type, const uint8_t* payload, size_t size);
    static model::H264PpsInfo ParsePpsFromNal(uint8_t nal_type, const uint8_t* payload, size_t size);

    // 判断是否为 SPS NAL 单元
    static bool IsSpsNalUnit(const videoeye::NalUnit& nal_unit);

    // 判断是否为 PPS NAL 单元
    static bool IsPpsNalUnit(const videoeye::NalUnit& nal_unit);

    // Profile / Level 名称转换（单元测试亦直接调用）
    static std::string GetProfileName(int profile_idc);
    static std::string GetLevelVersion(int level_idc);

private:
    // SPS 解析辅助
    static void ParseProfileLevelInfo(uint32_t profile_idc, uint32_t level_idc, int& profile, std::string& profile_name,
                                      int& level, std::string& level_version);

    // 取出 RBSP：去掉可能存在的 1 字节 NAL header，再做 emulation prevention 反转义。
    // ExtradataParser 的两条路径对 data 是否含 header 并不一致
    // （AnnexB 剥掉、avcC 保留），这里统一处理。
    static std::vector<uint8_t> GetRbsp(const videoeye::NalUnit& nal_unit);

    // 同上，但从裸载荷开始（NalView 路径）：只做 emulation prevention 反转义。
    static std::vector<uint8_t> GetRbsp(const uint8_t* payload, size_t size);

    // 判断 profile 是否带 chroma_format_idc / bit_depth 等扩展字段
    static bool HasChromaFormatExtension(int profile_idc);

    // 返回 false = delta_scale 超出规范范围 [-128,127]，整份参数集应判为无效。
    static bool SkipScalingList(videoeye::BitReader& reader, int size);
    static void SkipHrdParameters(videoeye::BitReader& reader, model::H264VuiInfo& vui);
    static void ParseVuiParameters(videoeye::BitReader& reader, model::H264VuiInfo& vui);
};

} // namespace videoeye
