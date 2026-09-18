#pragma once

#include <string>
#include <vector>

#include "core/model/ColorInfo.h"

namespace videoeye {
namespace model {

// ==========================================================================
// HDR 元数据模型（不依赖 FFmpeg / Qt）
//
// 数据来源（由 core/analyzer/ColorHdrAnalyzer 负责汇总）：
//   1) AVCodecParameters::color_*            -> ColorInfo
//   2) 容器/码流的 coded side data           -> MasteringDisplay / ContentLight / DolbyVision
//   3) 解码首帧的 AVFrame side data          -> HDR10+ / DV RPU / Mastering / CLL 的兜底与补充
// ==========================================================================

// 综合出来的 HDR 格式结论
enum class HdrFormat {
    Unknown = 0,
    Sdr,          // 非 HDR：SDR 传递函数，或无任何 HDR 线索
    Hlg,          // HLG（ARIB STD-B67）：广播电视 HDR，不需要静态元数据
    Hdr10,        // PQ + mastering display / MaxCLL-MaxFALL 的静态 HDR10
    Hdr10Basic,   // PQ 但缺少静态元数据（“裸 PQ”，播放端只能猜）
    Hdr10Plus,    // HDR10 + SMPTE ST 2094-40 动态元数据
    DolbyVision,  // Dolby Vision（含 DV profile / level 信息）
    HdrVivid,     // HDR Vivid（CUVA 005）
};

const char* ToString(HdrFormat format);

// SMPTE ST 2086 母版显示信息（对应 AV_PKT_DATA_MASTERING_DISPLAY_METADATA）
struct MasteringDisplayMetadata {
    bool present = false;
    bool has_primaries = false;
    bool has_luminance = false;

    // CIE 1931 xy 色度坐标
    double red_x = 0.0, red_y = 0.0;
    double green_x = 0.0, green_y = 0.0;
    double blue_x = 0.0, blue_y = 0.0;
    double white_x = 0.0, white_y = 0.0;

    double max_luminance = 0.0;  // cd/m^2
    double min_luminance = 0.0;  // cd/m^2

    // 三者齐备才算一份可用的 SMPTE ST 2086 数据
    bool Complete() const { return has_primaries && has_luminance; }
    // 数值是否落在合理范围（0..1 的色度 + 正的最大亮度）
    bool Plausible() const;

    std::string PrimariesText() const;
    std::string LuminanceText() const;
};

// CTA-861.3 内容光level（对应 AV_PKT_DATA_CONTENT_LIGHT_LEVEL）
struct ContentLightLevelMetadata {
    bool present = false;
    unsigned max_cll = 0;    // MaxCLL  cd/m^2
    unsigned max_fall = 0;   // MaxFALL cd/m^2

    bool HasAny() const { return max_cll > 0 || max_fall > 0; }
    bool Complete() const { return max_cll > 0 && max_fall > 0; }
};

// Dolby Vision 配置记录（对应 AV_PKT_DATA_DOVI_CONF / AVDOVIDecoderConfigurationRecord）
struct DolbyVisionMetadata {
    bool present = false;
    int profile = 0;
    int level = 0;
    bool rpu_present = false;   // dv_rpu_present_flag：是否带 RPU（动态元数据）
    bool el_present = false;    // dv_el_present_flag：是否有增强层
    bool bl_present = false;    // dv_bl_present_flag：是否有基础层
    int compatibility_id = 0;   // dv_bl_signal_compatibility_id：0 = 不兼容 HDR10/SDR（如 Profile 5）

    // 常见 profile 的兼容层含义：
    //   id 0 -> Profile 5/7 的一部分，非 DV 设备无法直接播放
    //   id 1 -> 兼容 SDR(HLG/PQ 取决于 profile)
    //   ...  详见 Dolby Vision Streams Within ISOBMFF 规范
    std::string CompatibilityText() const;
    // "8.1" 形式的 profile 名（考虑到 bl_signal_compatibility_id）
    std::string ProfileText() const;
};

// 单条视频流的 HDR 元数据汇总
struct HdrMetadataInfo {
    bool analyzed = false;
    int stream_index = -1;

    HdrFormat format = HdrFormat::Unknown;
    std::string format_name;         // "HDR10" / "HLG" / "Dolby Vision 8.1" ...
    bool hdr = false;                // 是否为 HDR 内容（PQ / HLG / DV）

    MasteringDisplayMetadata mastering_display;
    ContentLightLevelMetadata content_light;
    DolbyVisionMetadata dolby_vision;

    // 动态元数据
    bool has_hdr10_plus = false;     // SMPTE ST 2094-40（AV_FRAME_DATA_DYNAMIC_HDR_PLUS）
    bool has_hdr_vivid = false;      // HDR Vivid（AV_FRAME_DATA_DYNAMIC_HDR_VIVID）
    bool has_ambient_viewing_env = false;

    // 数据来源轨迹，便于 UI 说明"从哪读到的"
    std::vector<std::string> sources;    // 如 "容器 (stream coded_side_data)" / "解码首帧 AVFrame"
    std::vector<std::string> notes;      // 降级/提示信息

    std::string ToString() const;
};

// 综合判别 HDR 格式（纯函数，便于单元测试直接构造样本）
//
// 判定顺序：
//   DolbyVision > HDR Vivid > HDR10+ > HLG > HDR10(PQ+完整静态元数据) > 裸 PQ > SDR
// HLG 不需要静态元数据（不含 MaxCLL/MaxFALL 也正常），PQ 才算 HDR10。
HdrFormat ClassifyHdrFormat(const ColorInfo& color, const HdrMetadataInfo& hdr);

}  // namespace model
}  // namespace videoeye
