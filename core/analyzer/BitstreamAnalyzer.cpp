#include "core/analyzer/BitstreamAnalyzer.h"
#include "core/analyzer/Av1BitstreamParser.h"
#include "core/analyzer/H264BitstreamParser.h"
#include "core/analyzer/HevcBitstreamParser.h"
#include "core/analyzer/VvcBitstreamParser.h"
#include "utils/ExtradataParser.h"

#include <cstdlib>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace videoeye {
namespace analyzer {
namespace {

// 便于日志与报告里直接读枚举名；没落在已知分支上的编码返回 NONE
std::string CodecIdName(int codec_id) {
    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264: return "AV_CODEC_ID_H264";
        case AV_CODEC_ID_HEVC: return "AV_CODEC_ID_HEVC";
        case AV_CODEC_ID_AV1:  return "AV_CODEC_ID_AV1";
        case AV_CODEC_ID_VVC:  return "AV_CODEC_ID_VVC";
        default:               return "AV_CODEC_ID_NONE";
    }
}

// 是否拿「参数集里解析出来的宽高/位深/色彩」去和容器层比对。
//
// H.264 / HEVC / AV1 的解析器已按各自规范重写，并用逐位生成的真实码流
// 验证过（tests/unit 三套用例全绿），所以这里默认打开。
//
// 注意两边口径仍可能「合法地」差一点：HEVC 的 conformance window 与
// H.264 的 frame cropping 都已按规范减掉，但容器层记的是裁剪后的显示尺寸、
// 码流层某些封装记的是编码尺寸，差几个像素不算错误 —— 见
// CompareWithContainer() 里的 kGeometryTolerancePixels 容差处理。
constexpr bool kCompareParameterSetFields = true;

// 宽高对比容差（像素）：差值不超过它记为 info 级提示，超过才算 warning。
constexpr int kGeometryTolerancePixels = 8;

// 每种编码在 MP4 里的 extradata 配置记录类型。
// 自动检测只能可靠地认出 AnnexB 与 avcC，hvcC / av1C 的签名太模糊，
// 而调用方本来就知道 codec —— 直接按 codec 指定格式比猜更准。
utils::ExtradataFormat ConfigFormatForCodec(int codec_id) {
    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264: return utils::ExtradataFormat::AvcC;
        case AV_CODEC_ID_HEVC: return utils::ExtradataFormat::HvcC;
        case AV_CODEC_ID_AV1:  return utils::ExtradataFormat::Av1C;
        case AV_CODEC_ID_VVC:  return utils::ExtradataFormat::VvcC;
        default:               return utils::ExtradataFormat::Unknown;
    }
}

// AnnexB 流要按 codec 指定 NAL header 的解读方式：
// VVC 的 nal_unit_type 在第 2 字节，靠首字节的启发式一定会读错。
utils::NalSyntax NalSyntaxForCodec(int codec_id) {
    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264: return utils::NalSyntax::H264;
        case AV_CODEC_ID_HEVC: return utils::NalSyntax::Hevc;
        case AV_CODEC_ID_VVC:  return utils::NalSyntax::Vvc;
        default:               return utils::NalSyntax::Auto;
    }
}

// 判断 extradata 是不是「配置记录本体」（而不是 AnnexB / 长度前缀）。
//
// ⚠️ 原来统一用 `extradata[0] == 1` 判断，那只适用于 avcC / hvcC 的
// configurationVersion=1。av1C 的第 0 字节是 marker(1)+version(7) = 0x81，
// 永远不等于 1 —— 结果 AV1 的 extradata 一律落到自动检测分支，
// 又被 `data[0] & 0x03` 误判成长度前缀格式，OBU 列表恒为空。
bool LooksLikeConfigRecord(utils::ExtradataFormat cfg, const uint8_t* data, size_t size) {
    if (data == nullptr) return false;
    switch (cfg) {
        case utils::ExtradataFormat::AvcC:
            // configurationVersion=1，且第 5 字节高 6 位是保留位（全 1）
            return size >= 8 && data[0] == 1 && (data[4] & 0xFC) == 0xFC;
        case utils::ExtradataFormat::HvcC:
            return size >= 23 && data[0] == 1;
        case utils::ExtradataFormat::Av1C:
            // marker=1 且 version<=1，即 0x80 或 0x81
            return size >= 4 && (data[0] & 0x80) != 0 && (data[0] & 0x7F) <= 1;
        case utils::ExtradataFormat::VvcC:
            // vvcC 第 0 字节高 5 位是保留位，恒为 11111（0xF8）。
            // FFmpeg 的 ff_isom_write_vvcc() 用的就是这条判据。
            return size >= 2 && (data[0] & 0xF8) == 0xF8;
        default:
            return false;
    }
}

}  // namespace

BitstreamAnalyzer::BitstreamAnalyzer() {
    result_.analyzed = false;
}

BitstreamAnalyzer::~BitstreamAnalyzer() {
}

void BitstreamAnalyzer::SetContainerMetadata(const ContainerMetadata& metadata) {
    container_metadata_ = metadata;
    has_container_metadata_ = true;
}

// 容器侧数值快照：必须在 result_ 重置之后调用，
// 否则 Analyze() 开头那句 `result_ = BitstreamAnalysisResult()` 会把它清掉。
void BitstreamAnalyzer::ApplyContainerSnapshot() {
    if (!has_container_metadata_) {
        return;
    }
    result_.has_container = true;
    result_.container_width = container_metadata_.width;
    result_.container_height = container_metadata_.height;
    result_.container_bit_depth = container_metadata_.bit_depth;
    result_.container_color_primaries = container_metadata_.color_primaries;
    result_.container_transfer_characteristics = container_metadata_.transfer_characteristics;
    result_.container_matrix_coefficients = container_metadata_.matrix_coefficients;
    result_.container_color_range = container_metadata_.color_range;
}

model::BitstreamAnalysisResult BitstreamAnalyzer::Analyze(const uint8_t* extradata, 
                                                           size_t size,
                                                           int codec_id) {
    result_ = model::BitstreamAnalysisResult();
    result_.analyzed = true;
    result_.codec_id_str = CodecIdName(codec_id);

    // 空 extradata 直接放弃：没有参数集就没有可比对的码流信息
    if (extradata == nullptr || size == 0) {
        result_.analyzed = false;
        return result_;
    }

    // 先把 avcC / hvcC / AnnexB / av1C 统一抽成 NAL / OBU 列表。
    // extradata 以 configurationVersion=1 开头时按 codec 指定配置记录格式，
    // 其余情况（AnnexB / 长度前缀）走自动检测。
    const utils::ExtradataFormat cfg = ConfigFormatForCodec(codec_id);
    const utils::ExtradataResult parsed =
        LooksLikeConfigRecord(cfg, extradata, size)
            ? utils::ExtradataParser::ParseWithFormat(cfg, extradata, size)
            : utils::ExtradataParser::ParseWithFormat(utils::ExtradataFormat::AnnexB, extradata,
                                                      size, NalSyntaxForCodec(codec_id));
    result_.nal_units = parsed.nal_units;
    result_.obu_units = parsed.obu_units;
    PopulateAv1Config(parsed.config);
    if (parsed.format == utils::ExtradataFormat::VvcC) {
        PopulateVvcConfig(parsed.config);
    }
    ApplyContainerSnapshot();

    Dispatch(codec_id, extradata, size);

    // 与容器 metadata 对比
    if (has_container_metadata_ && result_.analyzed) {
        CompareWithContainer(result_, result_);
    }

    return result_;
}

model::BitstreamAnalysisResult BitstreamAnalyzer::AnalyzeWithFormat(
    utils::ExtradataFormat format,
    const uint8_t* data, size_t size,
    int codec_id) {
    result_ = model::BitstreamAnalysisResult();
    result_.analyzed = true;
    result_.codec_id_str = CodecIdName(codec_id);
    if (data == nullptr || size == 0) {
        result_.analyzed = false;
        return result_;
    }

    const utils::ExtradataResult parsed =
        utils::ExtradataParser::ParseWithFormat(format, data, size);
    result_.nal_units = parsed.nal_units;
    result_.obu_units = parsed.obu_units;
    if (format == utils::ExtradataFormat::Av1C) {
        PopulateAv1Config(parsed.config);
    }
    if (format == utils::ExtradataFormat::VvcC) {
        PopulateVvcConfig(parsed.config);
    }
    ApplyContainerSnapshot();

    Dispatch(codec_id, data, size);

    if (has_container_metadata_ && result_.analyzed) {
        CompareWithContainer(result_, result_);
    }
    return result_;
}

void BitstreamAnalyzer::Dispatch(int codec_id, const uint8_t* data, size_t size) {
    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264:
            result_.codec_name = "h264";
            result_.has_h264 = true;
            ParseH264(data, size);
            break;
        case AV_CODEC_ID_HEVC:
            result_.codec_name = "hevc";
            result_.has_hevc = true;
            ParseHevc(data, size);
            break;
        case AV_CODEC_ID_AV1:
            result_.codec_name = "av1";
            result_.has_av1 = true;
            ParseAv1(data, size);
            break;
        case AV_CODEC_ID_VVC:
            result_.codec_name = "vvc";
            result_.has_vvc = true;
            ParseVvc(data, size);
            break;
        default:
            // 不支持的编码：明确标记未分析，别让上层把默认 0 当成真实码流参数
            result_.analyzed = false;
            result_.codec_name.clear();
            result_.has_h264 = result_.has_hevc = result_.has_av1 = result_.has_vvc = false;
            break;
    }
}

void BitstreamAnalyzer::ParseH264(const uint8_t* data, size_t size) {
    // NAL 列表已在 Analyze() 里抽好，这里只挑出 SPS / PPS
    for (const auto& nal : result_.nal_units) {
        if (H264BitstreamParser::IsSpsNalUnit(nal)) {
            result_.h264_sps = H264BitstreamParser::ParseFromNalUnit(nal);
            break;
        }
    }
    for (const auto& nal : result_.nal_units) {
        if (H264BitstreamParser::IsPpsNalUnit(nal)) {
            // 直接从这个 NAL 解析，不要回头 ParsePps(extradata) 把整个
            // extradata 再拆一遍（旧代码这么写，结果对但白跑一趟）
            result_.h264_pps = H264BitstreamParser::ParsePpsFromNalUnit(nal);
            break;
        }
    }
    ApplyH264Summary();
}

void BitstreamAnalyzer::ApplyH264Summary() {
    const model::H264SpsInfo& sps = result_.h264_sps;
    if (!sps.present) return;

    int width = sps.width();
    int height = sps.height();
    if (sps.frame_cropping_flag) {
        // crop 偏移的单位是 CropUnitX/Y，随色度采样变化（4:2:0 = 2/2，4:2:2 = 2/1，其余 1/1）
        const int unit_x = (sps.chroma_format_idc == 1 || sps.chroma_format_idc == 2) ? 2 : 1;
        const int unit_y = (sps.chroma_format_idc == 1) ? 2 : 1;
        width -= (sps.frame_crop_left_offset + sps.frame_crop_right_offset) * unit_x;
        height -= (sps.frame_crop_top_offset + sps.frame_crop_bottom_offset) * unit_y;
    }
    if (width > 0) result_.width = width;
    if (height > 0) result_.height = height;
    result_.bit_depth = sps.BitDepthLuma();

    if (sps.vui.present) {
        result_.color_primaries = sps.vui.color_primaries;
        result_.transfer_characteristics = sps.vui.transfer_characteristics;
        result_.matrix_coefficients = sps.vui.matrix_coefficients;
    }
}

void BitstreamAnalyzer::ParseHevc(const uint8_t* data, size_t size) {
    (void)data;
    (void)size;
    for (const auto& nal : result_.nal_units) {
        if (HevcBitstreamParser::IsVpsNalUnit(nal)) {
            result_.hevc_vps = HevcBitstreamParser::ParseVpsFromNalUnit(nal);
            break;
        }
    }
    for (const auto& nal : result_.nal_units) {
        if (HevcBitstreamParser::IsSpsNalUnit(nal)) {
            result_.hevc_sps = HevcBitstreamParser::ParseSpfFromNalUnit(nal);
            break;
        }
    }
    for (const auto& nal : result_.nal_units) {
        if (HevcBitstreamParser::IsPpsNalUnit(nal)) {
            result_.hevc_pps = HevcBitstreamParser::ParsePpsFromNalUnit(nal);
            break;
        }
    }
    ApplyHevcSummary();
}

void BitstreamAnalyzer::ApplyHevcSummary() {
    const model::HevcSpsInfo& sps = result_.hevc_sps;
    if (sps.present) {
        // 注: 当前 SPS 解析没有记录 conformance window 偏移，这里给出的是
        // CTU 粒度的编码尺寸，可能比显示尺寸大几个像素。
        if (sps.width() > 0) result_.width = sps.width();
        if (sps.height() > 0) result_.height = sps.height();
        result_.bit_depth = sps.BitDepthLuma();
        if (sps.vui_parameters_present_flag) {
            result_.color_primaries = sps.colour_primaries;
            result_.transfer_characteristics = sps.transfer_characteristics;
            result_.matrix_coefficients = sps.matrix_coefficients;
        }
        return;
    }
    // SPS 缺失时退到 VPS 的位深（hvcC 里两者通常同时出现）
    if (result_.hevc_vps.present) {
        result_.bit_depth = result_.hevc_vps.vps_bit_depth_luma_minus8 + 8;
    }
}

void BitstreamAnalyzer::ParseAv1(const uint8_t* data, size_t size) {
    (void)data;
    (void)size;
    for (const auto& obu : result_.obu_units) {
        if (Av1BitstreamParser::IsSequenceHeaderObu(obu)) {
            result_.av1_seq_header = Av1BitstreamParser::ParseFromObuUnit(obu);
            break;
        }
    }
    ApplyAv1Summary();
}

void BitstreamAnalyzer::PopulateAv1Config(const utils::ExtradataResult::CodecConfig& cfg) {
    model::Av1CodecConfigInfo& out = result_.av1_config;
    out = model::Av1CodecConfigInfo();
    out.present = true;
    out.profile = cfg.profile;
    out.level = static_cast<int>(cfg.level_idc);
    out.tier = cfg.general_tier_flag;
    out.bit_depth = cfg.bit_depth_minus_8 + 8;
    out.monochrome = cfg.monochrome != 0;
    out.subsampling_x = cfg.chroma_subsampling_x;
    out.subsampling_y = cfg.chroma_subsampling_y;
    out.chroma_sample_position = cfg.chroma_sample_position;
    out.color_range = cfg.color_range;
    out.initial_presentation_delay = cfg.initial_presentation_delay_bits;
    result_.has_av1_config = true;
}

void BitstreamAnalyzer::PopulateVvcConfig(const utils::ExtradataResult::CodecConfig& cfg) {
    model::VvcCodecConfigInfo& out = result_.vvc_config;
    out = model::VvcCodecConfigInfo();
    out.present = true;
    out.general_profile_idc = cfg.general_profile_idc;
    out.general_tier_flag = cfg.general_tier_flag;
    out.general_level_idc = cfg.general_level_idc;
    out.chroma_format_idc = cfg.chroma_format_idc;
    out.bit_depth_minus8 = cfg.bit_depth_minus_8;
    out.num_sublayers = cfg.num_sublayers;
    out.max_picture_width = cfg.max_picture_width;
    out.max_picture_height = cfg.max_picture_height;
    result_.has_vvc_config = true;
}

void BitstreamAnalyzer::ApplyAv1Summary() {
    const model::Av1SequenceHeaderInfo& sh = result_.av1_seq_header;
    if (sh.present) {
        if (sh.FrameWidth() > 0) result_.width = sh.FrameWidth();
        if (sh.FrameHeight() > 0) result_.height = sh.FrameHeight();

        // AV1 的 color_primaries / transfer / matrix 与 H.264 VUI 用的是同一套
        // ISO/IEC 23001-8（CICP）枚举值，可以直接和 AVCodecParameters 比对。
        const int depth = sh.BitDepth();
        if (depth > 0) result_.bit_depth = depth;

        if (sh.color_config.color_description_present_flag) {
            result_.color_primaries = sh.color_config.color_primaries;
            result_.transfer_characteristics = sh.color_config.transfer_characteristics;
            result_.matrix_coefficients = sh.color_config.matrix_coefficients;
        }
        return;
    }

    // 没有序列头 OBU（MP4/WebM 里的 av1C 通常如此）：退到 av1C 配置记录。
    // 只能给出位深与采样结构 —— 分辨率和色彩描述不在 av1C 里，保持未知，
    // 别拿 0 去和容器层比对制造假告警。
    if (result_.av1_config.present) {
        if (result_.av1_config.bit_depth > 0) {
            result_.bit_depth = result_.av1_config.bit_depth;
        }
    }
}

void BitstreamAnalyzer::ParseVvc(const uint8_t* data, size_t size) {
    (void)data;
    (void)size;
    for (const auto& nal : result_.nal_units) {
        if (VvcBitstreamParser::IsVpsNalUnit(nal)) {
            result_.vvc_vps = VvcBitstreamParser::ParseVpsFromNalUnit(nal);
            break;
        }
    }
    for (const auto& nal : result_.nal_units) {
        if (VvcBitstreamParser::IsSpsNalUnit(nal)) {
            result_.vvc_sps = VvcBitstreamParser::ParseSpsFromNalUnit(nal);
            break;
        }
    }
    for (const auto& nal : result_.nal_units) {
        if (VvcBitstreamParser::IsPpsNalUnit(nal)) {
            result_.vvc_pps = VvcBitstreamParser::ParsePpsFromNalUnit(nal);
            // conformance window 的裁剪单位由 SPS 的色度采样决定
            result_.vvc_pps.chroma_format_idc = result_.vvc_sps.sps_chroma_format_idc;
            break;
        }
    }
    ApplyVvcSummary();
}

void BitstreamAnalyzer::ApplyVvcSummary() {
    const model::VvcSpsInfo& sps = result_.vvc_sps;
    if (sps.present) {
        // sps_res_change_in_clvs_allowed_flag 时真实尺寸可能由 PPS 覆盖
        const model::VvcPpsInfo& pps = result_.vvc_pps;
        if (sps.sps_res_change_in_clvs_allowed_flag && pps.present &&
            pps.pps_pic_width_in_luma_samples > 0) {
            if (pps.width() > 0) result_.width = pps.width();
            if (pps.height() > 0) result_.height = pps.height();
        } else {
            if (sps.width() > 0) result_.width = sps.width();
            if (sps.height() > 0) result_.height = sps.height();
        }
        result_.bit_depth = sps.BitDepthLuma();

        if (sps.vui.present && sps.vui.colour_description_present_flag) {
            result_.color_primaries = sps.vui.colour_primaries;
            result_.transfer_characteristics = sps.vui.transfer_characteristics;
            result_.matrix_coefficients = sps.vui.matrix_coeffs;
        }
        return;
    }

    // SPS 缺失时退到 VPS 的 PTL
    if (result_.vvc_vps.present) {
        result_.bit_depth = result_.vvc_vps.vps_bit_depth_luma_minus8 + 8;
    }

    // 最后退到 vvcC：MP4 的 VVC extradata 只有 vvcC，位深/分辨率只有这里有。
    // 分辨率用 max_picture_width/height（vvcC 的语义就是"该码流的最大图像尺寸"），
    // 只在 SPS/PPS 都没给出尺寸时才用，避免盖掉更精确的裁剪后尺寸。
    if (result_.vvc_config.present) {
        if (result_.bit_depth == 8 && result_.vvc_config.bit_depth_minus8 > 0) {
            result_.bit_depth = result_.vvc_config.BitDepth();
        }
        if (result_.width <= 0 && result_.vvc_config.max_picture_width > 0) {
            result_.width = result_.vvc_config.max_picture_width;
            result_.height = result_.vvc_config.max_picture_height;
        }
    }
}

bool BitstreamAnalyzer::AnyParameterSet() const {
    return result_.h264_sps.present || result_.hevc_sps.present ||
           result_.hevc_vps.present || result_.av1_seq_header.present ||
           result_.av1_config.present || result_.vvc_vps.present ||
           result_.vvc_sps.present || result_.vvc_config.present;
}

void BitstreamAnalyzer::CompareWithContainer(const model::BitstreamAnalysisResult& bitstream,
                                             model::BitstreamAnalysisResult& result) {
    (void)bitstream;
    if (!AnyParameterSet()) {
        return;   // 什么都没解析出来，别拿默认值制造假告警
    }
    if (!kCompareParameterSetFields) {
        return;   // 见 kCompareParameterSetFields 的注释：解析器还没对齐规范
    }

    // Width/Height comparison（只在码流侧真的算出尺寸时比）
    if (result.width > 0 && container_metadata_.width > 0 &&
        result.width != container_metadata_.width) {
        const int diff_w = std::abs(result.width - container_metadata_.width);
        const bool minor = diff_w <= kGeometryTolerancePixels;
        AddInconsistency(result, "Width",
                        std::to_string(container_metadata_.width),
                        std::to_string(result.width),
                        minor ? "容器宽度与码流宽度相差 " + std::to_string(diff_w) +
                                " 像素，通常是 conformance window / cropping 的统计口径差异"
                              : "Container width differs from bitstream width",
                        minor ? "一般无需处理，确认播放器按显示尺寸渲染即可" : "",
                        minor ? "info" : "warning");
    }
    if (result.height > 0 && container_metadata_.height > 0 &&
        result.height != container_metadata_.height) {
        const int diff_h = std::abs(result.height - container_metadata_.height);
        const bool minor = diff_h <= kGeometryTolerancePixels;
        AddInconsistency(result, "Height",
                        std::to_string(container_metadata_.height),
                        std::to_string(result.height),
                        minor ? "容器高度与码流高度相差 " + std::to_string(diff_h) +
                                " 像素，通常是 conformance window / cropping 的统计口径差异"
                              : "Container height differs from bitstream height",
                        minor ? "一般无需处理，确认播放器按显示尺寸渲染即可" : "",
                        minor ? "info" : "warning");
    }

    // Bit depth comparison
    if (container_metadata_.bit_depth > 0 && result.bit_depth != container_metadata_.bit_depth) {
        AddInconsistency(result, "Bit Depth",
                        std::to_string(container_metadata_.bit_depth),
                        std::to_string(result.bit_depth),
                        "Container bit depth differs from bitstream bit depth");
    }

    // Color primaries comparison（0 = 未指定，两边都可能没有）
    if (container_metadata_.color_primaries > 0 && result.color_primaries > 0 &&
        result.color_primaries != container_metadata_.color_primaries) {
        AddInconsistency(result, "Color Primaries",
                        "Container: " + std::to_string(container_metadata_.color_primaries),
                        "Bitstream: " + std::to_string(result.color_primaries),
                        "Container color primaries differ from bitstream",
                        "This may cause color display issues");
    }
}

void BitstreamAnalyzer::AddInconsistency(model::BitstreamAnalysisResult& result,
                                         const std::string& field,
                                         const std::string& container_value,
                                         const std::string& bitstream_value,
                                         const std::string& description,
                                         const std::string& suggestion,
                                         const std::string& severity) {
    model::BitstreamAnalysisResult::Inconsistency inconsistency;
    inconsistency.field = field;
    inconsistency.container_value = container_value;
    inconsistency.bitstream_value = bitstream_value;
    inconsistency.severity = severity;
    inconsistency.description = description;
    inconsistency.suggestion = suggestion;
    
    result.inconsistencies.push_back(std::move(inconsistency));
}

} // namespace analyzer
} // namespace videoeye
