#pragma once

// 码流分析的**统一入口头**（umbrella）。
//
// 2026-10-07 按 codec 做了一次纯物理拆分：原先 H.264 / HEVC / AV1 / VVC 的所有
// 结构体都挤在本头里（约 991 行），现按编解码器拆到同目录的四个子头：
//   * H264BitstreamInfo.h —— H264VuiInfo / H264SpsInfo / H264PpsInfo
//   * HevcBitstreamInfo.h —— HevcSeiMessage / HevcVpsInfo / HevcSpsInfo / HevcPpsInfo
//   * Av1BitstreamInfo.h  —— Av1FilmGrainInfo / Av1ColorConfigInfo /
//                            Av1SequenceHeaderInfo / Av1CodecConfigInfo
//   * VvcBitstreamInfo.h  —— VvcNalUnitInfo / VvcVuiInfo / VvcVpsInfo /
//                            VvcSpsInfo / VvcPpsInfo / VvcCodecConfigInfo
// 本头保留下来，继续提供跨 codec 的统一汇总类型 BitstreamAnalysisResult，并
// 重新 include 上述子头。类型名 / 字段名 / 语义一律不变，只挪了物理位置，因此
// 现有所有 #include "core/domain/model/BitstreamInfo.h" 的 TU 无需改动。

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/BitstreamUnits.h"
#include "core/domain/model/H264BitstreamInfo.h"
#include "core/domain/model/HevcBitstreamInfo.h"
#include "core/domain/model/Av1BitstreamInfo.h"
#include "core/domain/model/VvcBitstreamInfo.h"

namespace videoeye {
namespace model {

// --------------------------------------------------------------------------
// 统一码流信息汇总
// --------------------------------------------------------------------------
struct BitstreamAnalysisResult {
    bool analyzed = false;
    int stream_index = -1;
    std::string codec_name;           // "h264", "hevc", "av1", "vvc"
    std::string codec_id_str;         // "AV_CODEC_ID_H264" 等
    
    // 基本视频属性（从码流解析）
    int width = 0;
    int height = 0;
    int bit_depth = 8;
    int color_primaries = 0;
    int transfer_characteristics = 0;
    int matrix_coefficients = 0;

    // 容器层（AVCodecParameters）的同名字段快照，供 UI 对比表两侧并列展示。
    // 由 BitstreamAnalyzer::SetContainerMetadata() 在 Analyze() 时写入；
    // 调用方没设置容器 metadata 时 has_container 为 false，各字段保持 0。
    bool has_container = false;
    int container_width = 0;
    int container_height = 0;
    int container_bit_depth = 0;
    int container_color_primaries = 0;
    int container_transfer_characteristics = 0;
    int container_matrix_coefficients = 0;
    int container_color_range = 0;
    
    // 各编码类型的解析结果（最多只有一个为 true）
    bool has_h264 = false;
    bool has_hevc = false;
    bool has_av1 = false;
    bool has_vvc = false;
    
    // H.264 信息
    H264SpsInfo h264_sps;
    H264PpsInfo h264_pps;
    
    // H.265 信息
    HevcVpsInfo hevc_vps;
    HevcSpsInfo hevc_sps;
    HevcPpsInfo hevc_pps;
    
    // AV1 信息
    Av1SequenceHeaderInfo av1_seq_header;
    Av1CodecConfigInfo av1_config;      // av1C 里解析出的（序列头缺失时的兜底）
    bool has_av1_config = false;
    
    // VVC 信息
    VvcVpsInfo vvc_vps;
    VvcSpsInfo vvc_sps;
    VvcPpsInfo vvc_pps;
    VvcCodecConfigInfo vvc_config;  // vvcC 里解析出的（SPS/VPS 都缺 PTL 时的兜底）
    bool has_vvc_config = false;
    
    // 提取的 NAL/OBU 列表
    std::vector<NalUnit> nal_units;
    std::vector<ObuUnit> obu_units;
    
    // 不一致警告（与容器 metadata 对比）
    struct Inconsistency {
        std::string field;             // 字段名
        std::string container_value;   // 容器中的值
        std::string bitstream_value;   // 码流中的值
        std::string severity;          // "warning" / "error" / "info"
        std::string description;       // 详细描述
        std::string suggestion;        // 建议
    };
    
    std::vector<Inconsistency> inconsistencies;
    
    // 添加不一致警告
    void AddInconsistency(const std::string& field,
                         const std::string& container_value,
                         const std::string& bitstream_value,
                         const std::string& description,
                         const std::string& suggestion = "");
    
    // 摘要信息
    std::string Summary() const;
    
    // JSON 格式输出
    std::string ToJson() const;
};

} // namespace model
} // namespace videoeye