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

// AV1 OBU 类型
enum class Av1ObuType {
    SequenceHeader = 0,
    TileGroup = 1,
    ColorSpace = 2,
    UserConfiguration = 3,
    FrameHeader = 4,
    Frame = 5,
    TileList = 6,
    PartitionHeader = 7,
    ConfigRecord = 8,
    FrameTypeDependency = 9,
    MasteringDisplay = 10,
    ContentLight = 11,
    FilmGrain = 12,
    SyncMarker = 13,
    ObuNoEdl = 14,
    Resolution = 15,
    TemporalDelayer = 16,
    OpusTag = 17,
    TimeCode = 18,
    ColorConfig = 19,
    ObuWithEbs = 20,
    RedundantFrameHeader = 21,
    AdditionalFrameReference = 22,
    ObjectDivisions = 23,
    MotionOverflow = 24,
    GlobalMotion = 25,
    FrameEdge = 26,
    BitstreamAnnotation = 27,
    VideoToolboxOutputBuffer = 28,
    VideoToolboxInputBuffer = 29,
    VideoToolboxFlush = 30,
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

// OBU 结构（AV1/VVC）
struct ObuUnit {
    uint8_t type;              // OBU 类型
    uint32_t size;             // 数据大小
    std::vector<uint8_t> data; // OBU 数据
    bool has_extension_header; // 是否有扩展头
    bool is_sequence_header;   // 是否为序列头
    
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
        
        // AV1
        int profile = 0;
        int high_bitdepth = 0;
        int twelve_bit = 0;
        int bit_depth_minus_8 = 0;     // 实际位深 = 值 + 8
        int chroma_subsampling_x = 1;
        int chroma_subsampling_y = 1;
        int color_range = 1;
        int color_primaries = 9;  // BT.2020
        int transfer_characteristics = 14; // PQ
        int matrix_coefficients = 9; // BT.2020 NCL
        int initial_presentation_delay_bits = 0;
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
    
    // 直接指定格式解析
    static ExtradataResult ParseWithFormat(ExtradataFormat format,
                                           const uint8_t* data, size_t size);
    
    // 转换封装格式（Annex B ↔ length-prefix）
    static std::vector<uint8_t> ConvertAnnexBToLengthPrefix(
        const std::vector<uint8_t>& annex_b);
    
    static std::vector<uint8_t> ConvertLengthPrefixToAnnexB(
        const std::vector<uint8_t>& length_prefix,
        int prefix_bytes);
    
private:
    // 格式检测
    static ExtradataFormat DetectFormat(const uint8_t* data, size_t size);
    
    // 具体格式解析器
    static ExtradataResult ParseAnnexB(const uint8_t* data, size_t size);
    static ExtradataResult ParseAvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseHvcC(const uint8_t* data, size_t size);
    static ExtradataResult ParseAv1C(const uint8_t* data, size_t size);
    
    // Annex B 解析辅助
    static std::vector<NalUnit> ExtractAnnBNalUnits(const uint8_t* data, size_t size);
    
    // H.264 NAL 单元解析
    static NalUnit ParseH264NalUnit(const uint8_t* data, size_t size);
    
    // H.265 NAL 单元解析
    static NalUnit ParseHevcNalUnit(const uint8_t* data, size_t size);
    
    // AV1 OBU 解析
    static ObuUnit ParseAv1Obu(const uint8_t*& ptr, size_t remaining);
    
    // 查找下一个起始码（Annex B）
    static const uint8_t* FindStartCode(const uint8_t* pos, const uint8_t* end);
};

} // namespace utils
} // namespace videoeye
