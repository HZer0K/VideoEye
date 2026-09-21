#include "core/analyzer/BitstreamAnalyzer.h"
#include "core/analyzer/Av1BitstreamParser.h"
#include "core/analyzer/H264BitstreamParser.h"
#include "core/analyzer/HevcBitstreamParser.h"
#include "utils/ExtradataParser.h"

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
// 目前是 false：H264BitstreamParser / HevcBitstreamParser 的 SPS 字段顺序还没按
// 规范对齐（ReadUWord 当成第一个字段，实际规范里第一个是 profile_idc），
// tests/unit/test_h264_bitstream_parser.cpp 与 test_hevc_bitstream_parser.cpp
// 里那 9 个仍然标红的用例就是证据。现在打开对比会刷出一堆假告警，
// 所以先把结果存下来（UI 将来可以直接用），等解析器修好再翻成 true。
constexpr bool kCompareParameterSetFields = false;

// 每种编码在 MP4 里的 extradata 配置记录类型。
// 自动检测只能可靠地认出 AnnexB 与 avcC，hvcC / av1C 的签名太模糊，
// 而调用方本来就知道 codec —— 直接按 codec 指定格式比猜更准。
utils::ExtradataFormat ConfigFormatForCodec(int codec_id) {
    switch (static_cast<AVCodecID>(codec_id)) {
        case AV_CODEC_ID_H264: return utils::ExtradataFormat::AvcC;
        case AV_CODEC_ID_HEVC: return utils::ExtradataFormat::HvcC;
        case AV_CODEC_ID_AV1:  return utils::ExtradataFormat::Av1C;
        default:               return utils::ExtradataFormat::Unknown;
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
        (cfg != utils::ExtradataFormat::Unknown && extradata[0] == 1 && size >= 8)
            ? utils::ExtradataParser::ParseWithFormat(cfg, extradata, size)
            : utils::ExtradataParser::Parse(extradata, size);
    result_.nal_units = parsed.nal_units;
    result_.obu_units = parsed.obu_units;

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
            result_.h264_pps = H264BitstreamParser::ParsePps(data, size);
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

void BitstreamAnalyzer::ApplyAv1Summary() {
    const model::Av1SequenceHeaderInfo& sh = result_.av1_seq_header;
    if (!sh.present) return;

    if (sh.FrameWidth() > 0) result_.width = sh.FrameWidth();
    if (sh.FrameHeight() > 0) result_.height = sh.FrameHeight();

    // AV1 的 color_primaries / transfer / matrix 与 H.264 VUI 用的是同一套
    // ISO/IEC 23001-8 枚举值，可以直接和 AVCodecParameters 里的字段比对。
    int depth = sh.color_config.color_bit_depth;
    if (depth <= 0) depth = sh.input_bit_depth;
    if (depth <= 0) depth = sh.bit_depth_minus_8 + 8;
    if (depth > 0) result_.bit_depth = depth;

    if (sh.color_config.color_description_present_flag) {
        result_.color_primaries = sh.color_config.color_primaries;
        result_.transfer_characteristics = sh.color_config.transfer_characteristics;
        result_.matrix_coefficients = sh.color_config.matrix_coefficients;
    }
}

void BitstreamAnalyzer::ParseVvc(const uint8_t* data, size_t size) {
    (void)data;
    (void)size;
    // VVC (H.266) 只有 VvcBitstreamParser 的声明，没有实现：解析 VPS/SPS 需要完整的
    // profile_tier_level 与多组语法元素，工作量与当前收益不匹配。
    // 这里只做「识别到是 VVC」，不填任何码流字段，并显式记一条 info，
    // 避免上层把默认值 0 当成真实码流参数去和容器层比对。
    model::BitstreamAnalysisResult::Inconsistency note;
    note.field = "VVC 参数集解析";
    note.container_value = "-";
    note.bitstream_value = "-";
    note.severity = "info";
    note.description = "已识别为 VVC (H.266)，但参数集解析尚未实现";
    note.suggestion = "码流层参数不可用，请以容器层 (AVCodecParameters) 为准";
    result_.inconsistencies.push_back(std::move(note));
}

bool BitstreamAnalyzer::AnyParameterSet() const {
    return result_.h264_sps.present || result_.hevc_sps.present ||
           result_.hevc_vps.present || result_.av1_seq_header.present;
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
    if (result.width > 0) {
        if (container_metadata_.width > 0 && result.width != container_metadata_.width) {
            AddInconsistency(result, "Width",
                            std::to_string(container_metadata_.width),
                            std::to_string(result.width),
                            "Container width differs from bitstream width");
        }
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
                                         const std::string& suggestion) {
    model::BitstreamAnalysisResult::Inconsistency inconsistency;
    inconsistency.field = field;
    inconsistency.container_value = container_value;
    inconsistency.bitstream_value = bitstream_value;
    inconsistency.severity = "warning";
    inconsistency.description = description;
    inconsistency.suggestion = suggestion;
    
    result.inconsistencies.push_back(std::move(inconsistency));
}

} // namespace analyzer
} // namespace videoeye
