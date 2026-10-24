#pragma once

// Extradata 解析的公共结果类型。
//
// 放在这里是为了让**结果的使用者**（如 core/domain/model/BitstreamInfo.h）不必
// 包含 ExtradataParser 这个解析器实现头 —— 反过来也就是让 model 层不再依赖
// codec 层。解析器与本文件同属 videoeye::utils，调用方无需改名。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace videoeye {

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
    AnnexB,        // H.264/H.265 Annex B (00 00 00 01 或 00 00 01 起始码)
    LengthPrefix1, // 1 字节长度前缀
    LengthPrefix2, // 2 字节长度前缀
    LengthPrefix4, // 4 字节长度前缀
    AvcC,          // MP4 avcC 配置记录（H.264）
    HvcC,          // MP4 hvcC 配置记录（H.265）
    Av1C,          // MP4 av1C 配置记录（AV1）
    VvcC,          // MP4 vvcC 配置记录（VVC）
    VpcC,          // MP4 vpcC 配置记录（VP9，ISO/IEC 14496-30）
                   //   注意：WebM/Matroska 的 VP9 **没有 codec private**，
                   //   那种文件只能从关键帧带内解析（Vp9BitstreamParser）
};

// NAL header 的解读方式。
//
// 三种封装的 NAL header 长度与 type 位域都不一样，AnnexB 只看首字节无法区分
// H.264 与 VVC（VVC 首字节是 nuh_layer_id，type 在第 2 字节），所以调用方
// 知道 codec 时必须显式指定，别让启发式去猜。
enum class NalSyntax {
    Auto = 0, // 按首字节启发式猜（H.264 / HEVC 二选一，见 ExtractAnnBNalUnits）
    H264,     // 1 字节 header，type = byte0 & 0x1F
    Hevc,     // 2 字节 header，type = (byte0 >> 1) & 0x3F
    Vvc,      // 2 字节 header，type = (byte1 >> 3) & 0x1F
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
    bool is_idr;               // H.264: IDR 帧标记
    bool is_keyframe;          // 通用关键帧标记
    // H.264 的 nal_ref_idc（header 的 bit6..5）。slice_header() 里
    // dec_ref_pic_marking() 只在 nal_ref_idc != 0 时存在 —— 少了它，非参考帧
    // 会少读 1 位（IDS 分支还要多 1 位），后面所有字段整体错位。
    // -1 = 未知（非 H.264 NAL，或构造方没给）。
    int nal_ref_idc = -1;

    NalUnit() : type(0), size(0), is_idr(false), is_keyframe(false) {
    }

    // 便于测试与手工构造：(type, size, data, is_idr, is_keyframe)
    NalUnit(uint8_t t, size_t s, std::vector<uint8_t> d, bool idr, bool keyframe)
        : type(t), size(static_cast<uint32_t>(s)), data(std::move(d)), is_idr(idr), is_keyframe(keyframe) {
    }
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

    ObuUnit() : type(0), size(0), has_extension_header(false), is_sequence_header(false) {
    }

    // 便于测试与手工构造：(type, size, data, has_extension_header, is_sequence_header)
    ObuUnit(uint8_t t, size_t s, std::vector<uint8_t> d, bool ext_header, bool seq_header)
        : type(t), size(static_cast<uint32_t>(s)), data(std::move(d)), has_extension_header(ext_header),
          is_sequence_header(seq_header) {
    }
};

// ==========================================================================
// vpcC（VP Codec Configuration Box，ISO/IEC 14496-30）布局判定
//
// vpcC 主体恒为 8 字节：
//   profile(1) | level(1) | bit_depth(4)+chroma_subsampling(3)+full_range(1)
//   | colour_primaries(1) | transfer_characteristics(1) | matrix_coefficients(1)
//   | codec_initialization_data_size(2)
//
// 但 **extradata 从哪里开始** 有两种写法：拿到完整 'vpcC' box（含 size+fourcc），
// 或只拿到剥掉 size+fourcc 后的 payload（含 version/flags）。这里统一判定主体偏移，
// 返回值 == size 表示两种布局都不像。
//
// 判定顺序不能换：形式 A 的 [4..7] 是固定的 fourcc，而形式 B 的 [4] 是 profile
// （碰巧也可能等于 'v'）—— 先查 fourcc 才不会张冠李戴。
// ==========================================================================
inline size_t VpccPayloadOffset(const uint8_t* data, size_t size) {
    // 形式 A：完整 box —— [0..3] 是 box size（不会全 0），[4..7] 是 'vpcC'
    if (size >= 20 && data[4] == 'v' && data[5] == 'p' && data[6] == 'c' && data[7] == 'C') {
        return 12; // size(4) + fourcc(4) + version/flags(4)
    }
    // 形式 B：demuxer 已剥掉 box 头 —— [0..3] 是 version/flags（vpcC 恒为 0）
    if (size >= 12 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
        return 4;
    }
    // 形式 C：只有 8 字节主体（version/flags 也被剥了）
    if (size >= 8) {
        return 0;
    }
    return size;
}

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
        int profile_idc = 0;           // H.264 profile
        int profile_compatibility = 0; // H.264 profile compatibility flag
        uint32_t level_idc = 0;        // H.264 level
        // NAL 长度前缀宽度 - 1（ISO/IEC 14496-15 avcC/hvcC 的 lengthSizeMinusOne）。
        // 合法值 0..3，即前缀宽度 1..4 字节；**默认 4 字节 → 3**。
        // 旧默认值写的 31（把"位数"和"minus_one"混了），谁按它算宽度都会越界。
        uint32_t length_size_minus_one = 3;

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
        int profile = 0; // seq_profile 0..2
        int high_bitdepth = 0;
        int twelve_bit = 0;
        int monochrome = 0;
        int bit_depth_minus_8 = 0; // 实际位深 = 值 + 8
        int chroma_subsampling_x = 1;
        int chroma_subsampling_y = 1;
        int chroma_sample_position = 0; // 0=UNKNOWN 1=VERTICAL 2=COLOCATED 3=RESERVED
        int color_range = 1;
        int color_primaries = 9;           // BT.2020
        int transfer_characteristics = 14; // PQ
        int matrix_coefficients = 9;       // BT.2020 NCL
        int initial_presentation_delay_bits = 0;

        // VVC（来自 vvcC 配置记录；num_sublayers 是 vvcC 里的 3 位字段）
        int chroma_format_idc = 0; // 0=4:0:0 1=4:2:0 2=4:2:2 3=4:4:4
        int num_sublayers = 0;
        int max_picture_width = 0;
        int max_picture_height = 0;

        // VP9（来自 vpcC 配置记录）
        // profile 复用上面的 `profile` 字段；level_idc 复用上面的 `level_idc`
        // —— vpcC 的 level 本身就用的是同一套整数编码（10=Level 1、11=1.1 ...）。
        int vp9_chroma_subsampling = 2; // 0=4:4:4 1=4:2:2 2=4:2:0 3=4:4:0
        int vp9_video_full_range_flag = 0;
        bool vp9_color_description_present = false; // CICP 三元组非全 0 才算写了
    } config;

    // 视频尺寸（如果可从 extradata 推断）
    int width = 0;
    int height = 0;
};

} // namespace videoeye
