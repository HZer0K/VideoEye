#include "core/analyzer/BitstreamAnalyzer.h"
#include "core/analyzer/H264BitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

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
    
    // 设置 codec 信息
    switch (codec_id) {
        case 0: // AV_CODEC_ID_H264
            result_.codec_name = "h264";
            result_.has_h264 = true;
            ParseH264(extradata, size);
            break;
        case 1: // AV_CODEC_ID_HEVC
            result_.codec_name = "hevc";
            result_.has_hevc = true;
            ParseHevc(extradata, size);
            break;
        case 2: // AV_CODEC_ID_AV1
            result_.codec_name = "av1";
            result_.has_av1 = true;
            ParseAv1(extradata, size);
            break;
        case 3: // AV_CODEC_ID_VVC
            result_.codec_name = "vvc";
            result_.has_vvc = true;
            ParseVvc(extradata, size);
            break;
        default:
            result_.analyzed = false;
            return result_;
    }
    
    // 与容器 metadata 对比
    if (has_container_metadata_) {
        CompareWithContainer(result_, result_);
    }
    
    return result_;
}

model::BitstreamAnalysisResult BitstreamAnalyzer::AnalyzeWithFormat(
    utils::ExtradataFormat format,
    const uint8_t* data, size_t size,
    int codec_id) {
    // Extract NAL/OBU units based on format
    std::vector<utils::NalUnit> nal_units;
    std::vector<utils::ObuUnit> obu_units;
    
    if (format == utils::ExtradataFormat::AnnexB ||
        format == utils::ExtradataFormat::LengthPrefix1 ||
        format == utils::ExtradataFormat::LengthPrefix2 ||
        format == utils::ExtradataFormat::LengthPrefix4) {
        ExtractNalUnits(data, size, nal_units);
    } else {
        ExtractObuUnits(data, size, obu_units);
    }
    
    // ... More implementation
    
    return model::BitstreamAnalysisResult();
}

void BitstreamAnalyzer::ParseH264(const uint8_t* data, size_t size) {
    // Extract NAL units first
    auto result = utils::ExtradataParser::Parse(data, size);
    result_.nal_units = result.nal_units;
    
    // Find and parse SPS
    for (const auto& nal : result.nal_units) {
        if (H264BitstreamParser::IsSpsNalUnit(nal)) {
            result_.h264_sps = H264BitstreamParser::ParseFromNalUnit(nal);
            break;
        }
    }
    
    // Find and parse PPS
    for (const auto& nal : result.nal_units) {
        if (H264BitstreamParser::IsPpsNalUnit(nal)) {
            result_.h264_pps = H264BitstreamParser::ParsePps(data, size);
            break;
        }
    }
}

void BitstreamAnalyzer::ParseHevc(const uint8_t* data, size_t size) {
    // Similar to H.264 but for HEVC
    // TODO: Implement HevcBitstreamParser
}

void BitstreamAnalyzer::ParseAv1(const uint8_t* data, size_t size) {
    // Similar to H.264 but for AV1
    // TODO: Implement Av1BitstreamParser
}

void BitstreamAnalyzer::ParseVvc(const uint8_t* data, size_t size) {
    // Similar to H.264 but for VVC
    // TODO: Implement VvcBitstreamParser
}

void BitstreamAnalyzer::ExtractNalUnits(const uint8_t* data, size_t size,
                                        std::vector<utils::NalUnit>& nal_units) {
    // Implementation to extract NAL units from Annex B or length-prefix
    // This is a simplified version - full implementation would handle both formats
}

void BitstreamAnalyzer::ExtractObuUnits(const uint8_t* data, size_t size,
                                        std::vector<utils::ObuUnit>& obu_units) {
    // Implementation to extract OBU units from AV1/VVC
    // This is a simplified version
}

void BitstreamAnalyzer::CompareWithContainer(const model::BitstreamAnalysisResult& bitstream,
                                             model::BitstreamAnalysisResult& result) {
    // Compare bitstream info with container metadata
    // Add inconsistencies if there are differences
    
    if (!result.has_h264 && !result.has_hevc && !result.has_av1 && !result.has_vvc) {
        return;
    }
    
    // Width/Height comparison
    if (container_metadata_.width > 0 && result.width != container_metadata_.width) {
        AddInconsistency(result, "Width",
                        std::to_string(container_metadata_.width),
                        std::to_string(result.width),
                        "Container width differs from bitstream width");
    }
    
    // Bit depth comparison
    if (container_metadata_.bit_depth > 0 && result.bit_depth != container_metadata_.bit_depth) {
        AddInconsistency(result, "Bit Depth",
                        std::to_string(container_metadata_.bit_depth),
                        std::to_string(result.bit_depth),
                        "Container bit depth differs from bitstream bit depth");
    }
    
    // Color primaries comparison
    if (container_metadata_.color_primaries > 0 && 
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
    
    result.inconsistencies.push_back(inconsistency);
}

} // namespace analyzer
} // namespace videoeye
