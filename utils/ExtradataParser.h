#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <utility>
#include "utils/BitReader.h"

namespace videoeye {
namespace utils {

// NAL 单元类型（H.264）
enum class H264NalType {
    UNSPECIFIED = 0,
    CodedSliceNonIdr = 1,
    CodedSlicePartA = 2,
    CodedSlicePartB = 3,
    CodedSlicePartC = 4,
    CodedSliceDciIdr = 5,
    AspSEI = 6,
    AspSPS = 7,
    AspPPS = 8,
    AspSeqParamSetEnd = 9,
    AspPicParamSet = 10,
    Delimiter = 11,
    RedundantPic = 12,
    SeqExt = 13,
    SeqExtData = 14,
    SeqExtEnd = 15,
    StSubLayerPic = 16,
    StRefPic = 17,
    StExtension = 18,
    FuIndicator = 19,
    FuSlice = 20,
    SpsExt = 21,
    SpsScalable = 22,
    PpsScalable = 23,
    StSps = 33,
    StPps = 34,
    StAsc = 35,
    StRefPicFlag = 36,
    StExtensionData = 37,
    ScalableNalExt = 38,
    Seis = 39,
    SpsExt2 = 40,
    SpsExt3 = 41,
};

// HEVC NAL 单元类型
enum class HevcNalType {
    CodedSliceTraiR = 0,
    CodedSliceTlaN = 1,
    CodedSliceTsaN = 2,
    CodedSliceTraiN = 3,
    CodedSliceTlaR = 4,
    CodedSliceTraiP = 5,
    VidParamSet = 32,
    SeqParamSet = 33,
    PicParamSet = 34,
    AccessUnitDelimiter = 35,
    EndOfSequence = 36,
    EndOfStream = 37,
    SupplementalEnhancementInfo = 39,
    PrefixNalUnit = 40,
    SubSeqSeqParamSet = 41,
    SubSeqInfo = 42,
    SpsExt = 43,
    ScalableNalExt = 44,
    VpsExt = 45,
};

// AV1 OBU 类型（AV1 规范 6.2.2 obu_type 取值表）
//
// ⚠️ 旧版本的取值是照着 H.264 NAL 类型表抄的，序列头写成了 0。
// 规范里 OBU_SEQUENCE_HEADER = 1，OBU_PADDING = 15，其余按表排列。
enum class Av1ObuType {
    Reserved0 = 0,
    SequenceHeader = 1,
    TemporalDelimiter = 2,
    FrameHeader = 3,
    TileGroup = 4,
    Metadata = 5,
    Frame = 6,
    RedundantFrameHeader = 7,
    TileList = 8,
    Reserved9 = 9,
    Reserved10 = 10,
    Reserved11 = 11,
    Reserved12 = 12,
    Reserved13 = 13,
    Reserved14 = 14,
    Padding = 15,
};

// 封装格式枚举
enum class ExtradataFormat {
    Unknown = 0,
    AnnexB,           // H.264/H.265 Annex B (00 00 00 01 或 00 00 01 起始码)
    LengthPrefix1,    // 1 字节长度前缀
    LengthPrefix2,    // 2 字节长度前缀
    LengthPrefix4,    // 4 字节长度前缀
    AvcC,            // MP4 avcC 配置记录（H.264）
    HvcC,            // MP4 hvcC 配置记录（H.265）
    Av1C,            // MP4 av1C 配置记录（AV1）
    VvcC,            // MP4 vvcC 配置记录（VVC）
};

// NAL header 的解读方式。
//
// 三种封装的 NAL header 长度与 type 位域都不一样，AnnexB 只看首字节无法区分
// H.264 与 VVC（VVC 首字节是 nuh_layer_id，type 在第 2 字节），所以调用方
// 知道 codec 时必须显式指定，别让启发式去猜。
enum class NalSyntax {
    Auto = 0,   // 按首字节启发式猜（H.264 / HEVC 二选一，见 ExtractAnnBNalUnits）
    H264,       // 1 字节 header，type = byte0 & 0x1F
    Hevc,       // 2 字节 header，type = (byte0 >> 1) & 0x3F
    Vvc,        // 2 字节 header，type = (byte1 >> 3) & 0x1F
};

// NAL/OBU 单元结构
//
// 不变量：`data` 只含 RBSP payload **不带 NAL header**，也不带起始码。
// AnnexB 路径由 ParseH264NalUnit / ParseHevcNalUnit 剥离（分别 1 / 2 字节），
// avcC 路径在 ParseAvcC 里剥离 1 字节。上层 parser 不要再 SkipBits(header)。
struct NalUnit {
    uint8_t type;              // NAL 单元类型
    uint32_t size;             // payload 大小（= data.size()）
    std::vector<uint8_t> data; // RBSP payload（不含起始码、不含 NAL header）
    bool is_idr;              // H.264: IDR 帧标记
    bool is_keyframe;         // 通用关键帧标记
    
    NalUnit() : type(0), size(0), is_idr(false), is_keyframe(false) {}

    // 便于测试与手工构造：(type, size, data, is_idr, is_keyframe)
    NalUnit(uint8_t t, size_t s, std::vector<uint8_t> d, bool idr, bool keyframe)
        : type(t), size(static_cast<uint32_t>(s)), data(std::move(d)),
          is_idr(idr), is_keyframe(keyframe) {}
};

// OBU 结构（AV1）
//
// 不变量：与 NalUnit 保持一致 —— `data` 只含 **OBU payload**，
// 不含 obu_header(1B)、不含 leb128 长度、不含 extension header。
// 上层 parser 可以直接从 data[0] 的最高位开始按 MSB-first 读。
struct ObuUnit {
    uint8_t type;              // obu_type（1 = OBU_SEQUENCE_HEADER）
    uint32_t size;             // payload 大小（= data.size()）
    std::vector<uint8_t> data; // OBU payload（不含 header / 长度 / 扩展头）
    bool has_extension_header; // obu_extension_flag
    bool is_sequence_header;   // 是否为序列头
    uint8_t temporal_id = 0;   // 扩展头里的 temporal_id（无扩展头时为 0）
    uint8_t spatial_id = 0;    // 扩展头里的 spatial_id
    
    ObuUnit() : type(0), size(0), has_extension_header(false), 
                is_sequence_header(false) {}

    // 便于测试与手工构造：(type, size, data, has_extension_header, is_sequence_header)
    ObuUnit(uint8_t t, size_t s, std::vector<uint8_t> d, bool ext_header, bool seq_header)
        : type(t), size(static_cast<uint32_t>(s)), data(std::move(d)),
          has_extension_header(ext_header), is_sequence_header(seq_header) {}
};

// 提取器结果
struct ExtradataResult {
    ExtradataFormat format = ExtradataFormat::Unknown;
    bool valid = false;
    std::string error_message;
    
    // 解析出的 NAL/OBU 列表
    std::vector<NalUnit> nal_units;
    std::vector<ObuUnit> obu_units;
    
    // 配置记录信息（avcC/hvcC/av1C）
    struct CodecConfig {
        int profile_idc = 0;          // H.264 profile
        int profile_compatibility = 0; // H.264 profile compatibility flag
        uint32_t level_idc = 0;       // H.264 level
        uint32_t length_size_minus_one = 31; // NAL 长度字段位数（3 字节=31，4 字节=31）
        
        // H.265
        int general_profile_space = 0;
        int general_tier_flag = 0;
        int general_profile_idc = 0;
        uint32_t general_level_idc = 0;
        int vps_bit_depth_luma_minus8 = 0;
        int vps_bit_depth_chroma_minus8 = 0;
        
        // AV1（来自 av1C 配置记录）
        // 注：level_idc 复用上面 H.264 那个字段；AV1 里存的是 seq_level_idx_0(0..23)，
        // general_tier_flag 复用为 seq_tier_0。
        int profile = 0;                // seq_profile 0..2
        int high_bitdepth = 0;
        int twelve_bit = 0;
        int monochrome = 0;
        int bit_depth_minus_8 = 0;      // 实际位深 = 值 + 8
        int chroma_subsampling_x = 1;
        int chroma_subsampling_y = 1;
        int chroma_sample_position = 0; // 0=UNKNOWN 1=VERTICAL 2=COLOCATED 3=RESERVED
        int color_range = 1;
        int color_primaries = 9;  // BT.2020
        int transfer_characteristics = 14; // PQ
        int matrix_coefficients = 9; // BT.2020 NCL
        int initial_presentation_delay_bits = 0;

        // VVC（来自 vvcC 配置记录；num_sublayers 是 vvcC 里的 3 位字段）
        int chroma_format_idc = 0;      // 0=4:0:0 1=4:2:0 2=4:2:2 3=4:4:4
        int num_sublayers = 0;
        int max_picture_width = 0;
        int max_picture_height = 0;
    } config;
    
    // 视频尺寸（如果可从 extradata 推断）
    int width = 0;
    int height = 0;
};

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

} // namespace utils
} // namespace videoeye
