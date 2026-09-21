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
//   3. 根据 AVCodecID 分发到具体解析器（H.264/H.265/AV1/VVC）
//   4. 与容器 metadata 对比，生成不一致性警告
//
// codec_id 参数直接就是 FFmpeg 的 AVCodecID（AV_CODEC_ID_H264 ...），
// 不要传 0/1/2/3 之类的自定义编号：历史上这里用过自定义枚举，结果调用方
// 一旦写错就静默落到 default 分支，什么都不解析。
//
// 当前覆盖范围：
//   - H.264 : SPS + PPS（含 VUI 色彩/时基）
//   - HEVC  : VPS + SPS + PPS
//   - AV1   : Sequence Header OBU（含 color_config）
//   - VVC   : 只识别编码类型，参数集解析尚未实现（见 ParseVvc）
// ==========================================================================
class BitstreamAnalyzer {
public:
    BitstreamAnalyzer();
    ~BitstreamAnalyzer();
    
    // 从 extradata 开始解析（主入口）。codec_id 传 AVCodecID。
    // 不支持的编码 / 空 extradata 会返回 analyzed=false，调用方应据此跳过展示。
    model::BitstreamAnalysisResult Analyze(const uint8_t* extradata, 
                                           size_t size,
                                           int codec_id);
    
    // 指定封装格式解析（调用方已经知道是 avcC / hvcC / AnnexB 时用）
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
    // 按 AVCodecID 分发到具体解析器
    void Dispatch(int codec_id, const uint8_t* data, size_t size);

    void ParseH264(const uint8_t* data, size_t size);
    void ParseHevc(const uint8_t* data, size_t size);
    void ParseAv1(const uint8_t* data, size_t size);
    void ParseVvc(const uint8_t* data, size_t size);

    // 把参数集里的宽高/位深/色彩映射到统一字段（供与容器层对比）
    void ApplyH264Summary();
    void ApplyHevcSummary();
    void ApplyAv1Summary();

    // 是否真的解析出了参数集（决定要不要拿默认值去和容器层比对）
    bool AnyParameterSet() const;

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
