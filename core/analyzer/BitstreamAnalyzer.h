#pragma once

#include <vector>
#include <cstdint>
#include "core/model/BitstreamInfo.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

// 容器 metadata 信息
struct ContainerMetadata {
    std::string codec_name;
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int color_primaries = 0;
    int transfer_characteristics = 0;
    int matrix_coefficients = 0;
    int color_range = 0;
    
    // Profile/Level (H.264/H.265)
    std::string profile_name;
    std::string level_version;
};

// ==========================================================================
// Bitstream Analyzer - 统一码流解析器
// 
// 职责：
//   1. 从 AVCodecParameters::extradata 开始解析
//   2. 抽象 Annex B 和 length-prefix 两种封装为 NAL/OBU 列表
//   3. 根据 codec ID 分发到具体解析器（H.264/H.265/AV1/VVC）
//   4. 与容器 metadata 对比，生成不一致性警告
// ==========================================================================
class BitstreamAnalyzer {
public:
    BitstreamAnalyzer();
    ~BitstreamAnalyzer();
    
    // 从 extradata 开始解析（主入口）
    model::BitstreamAnalysisResult Analyze(const uint8_t* extradata, 
                                           size_t size,
                                           int codec_id);
    
    // 指定格式解析
    model::BitstreamAnalysisResult AnalyzeWithFormat(
        utils::ExtradataFormat format,
        const uint8_t* data, size_t size,
        int codec_id);
    
    // 设置容器 metadata 用于对比
    void SetContainerMetadata(const ContainerMetadata& metadata);
    
    // 获取分析结果
    const model::BitstreamAnalysisResult& result() const { return result_; }
    
    // 是否完成分析
    bool finished() const { return result_.analyzed; }
    
private:
    // 分发到具体解析器
    void ParseH264(const uint8_t* data, size_t size);
    void ParseHevc(const uint8_t* data, size_t size);
    void ParseAv1(const uint8_t* data, size_t size);
    void ParseVvc(const uint8_t* data, size_t size);
    
    // 提取 NAL/OBU 单元
    void ExtractNalUnits(const uint8_t* data, size_t size,
                        std::vector<utils::NalUnit>& nal_units);
    
    void ExtractObuUnits(const uint8_t* data, size_t size,
                        std::vector<utils::ObuUnit>& obu_units);
    
    // 对比容器和码流 metadata
    void CompareWithContainer(const model::BitstreamAnalysisResult& bitstream,
                             model::BitstreamAnalysisResult& result);
    
    // 添加不一致警告
    void AddInconsistency(model::BitstreamAnalysisResult& result,
                         const std::string& field,
                         const std::string& container_value,
                         const std::string& bitstream_value,
                         const std::string& description,
                         const std::string& suggestion = "");
    
    model::BitstreamAnalysisResult result_;
    ContainerMetadata container_metadata_;
    bool has_container_metadata_ = false;
};

} // namespace analyzer
} // namespace videoeye
