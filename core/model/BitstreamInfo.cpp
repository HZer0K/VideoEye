#include "core/model/BitstreamInfo.h"

#include <sstream>

namespace videoeye {
namespace model {

namespace {

// JSON 字符串转义（仅处理常见控制字符与引号/反斜杠）
std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string JsonString(const std::string& in) {
    return "\"" + JsonEscape(in) + "\"";
}

} // namespace

// --------------------------------------------------------------------------
// H264SpsInfo
// --------------------------------------------------------------------------
std::string H264SpsInfo::ProfileName() const {
    switch (profile_idc) {
        case 66:  return "Baseline";
        case 77:  return "Main";
        case 88:  return "Extended";
        case 100: return "High";
        case 110: return "High 10";
        case 122: return "High 4:2:2";
        case 144: return "High 4:4:4";
        case 244: return "High 4:4:4 Predictive";
        case 118: return "Multiview High";
        case 128: return "Stereo High";
        default:  return "Unknown";
    }
}

std::string H264SpsInfo::LevelVersion() const {
    if (level_idc <= 0) {
        return "Unknown";
    }
    return std::to_string(level_idc / 10) + "." + std::to_string(level_idc % 10);
}

// --------------------------------------------------------------------------
// H264PpsInfo
// --------------------------------------------------------------------------
int H264PpsInfo::PicWidth() const {
    // PPS 本身不含图像尺寸，需要结合 SPS 的 pic_width_in_mbs_minus1 才能计算，
    // 这里没有 SPS 上下文，故返回 0 表示"未知"。
    return 0;
}

int H264PpsInfo::PicHeight() const {
    return 0;
}

// --------------------------------------------------------------------------
// HevcSpsInfo
// --------------------------------------------------------------------------
std::string HevcSpsInfo::ProfileName() const {
    switch (general_profile_idc) {
        case 0:  return "Main";
        case 1:  return "Main 10";
        case 2:  return "Main Still Picture";
        case 3:  return "Main Rext";
        case 4:  return "Range Extension";
        case 5:  return "High Throughput";
        case 6:  return "Multiview Main";
        case 7:  return "Scalable Main";
        case 8:  return "3D Main";
        case 9:  return "Screen Content Coding";
        default: return "Unknown";
    }
}

// --------------------------------------------------------------------------
// VVC (H.266)
// --------------------------------------------------------------------------
namespace {

// H.266 A.4：general_level_idc = majorNum * 16 + minorNum * 3
// （依据 RFC 9328 的说明性注释；例：level 3.1 -> 51，level 4.1 -> 67）
std::string VvcLevelString(uint32_t level_idc) {
    const int major = static_cast<int>(level_idc) / 16;
    const int minor = (static_cast<int>(level_idc) % 16) / 3;
    return std::to_string(major) + "." + std::to_string(minor);
}

// H.266 的 general_profile_idc 是位组合：
//   bit0     -> Main 10 基准（值 1）
//   bit5(32) -> 4:4:4 / 4:2:2 支持
//   bit6(64) -> 仅静止图像
// 具体取值见 RFC 9328 7.3.2.1：1 / 33 / 65 / 97
std::string VvcProfileName(int profile_idc) {
    switch (profile_idc) {
        case 1:  return "Main 10";
        case 33: return "Main 10 4:4:4";
        case 65: return "Main 10 Still Picture";
        case 97: return "Main 10 4:4:4 Still Picture";
        default: break;
    }
    // 多层档次用 bit4(16) 标记，按位拼一个可读名，避免返回纯 "Unknown"
    std::string name;
    if (profile_idc & 0x10) name += "Multilayer ";
    name += (profile_idc & 0x20) ? "Main 10 4:4:4" : "Main 10";
    if (profile_idc & 0x40) name += " Still Picture";
    name += " (idc=" + std::to_string(profile_idc) + ")";
    return name;
}

}  // namespace

std::string VvcVpsInfo::ProfileName() const { return VvcProfileName(general_profile_idc); }

std::string VvcVpsInfo::LevelString() const { return VvcLevelString(general_level_idc); }

std::string VvcSpsInfo::ProfileName() const { return VvcProfileName(general_profile_idc); }

std::string VvcSpsInfo::LevelString() const { return VvcLevelString(general_level_idc); }

std::string VvcCodecConfigInfo::ProfileName() const {
    return VvcProfileName(general_profile_idc);
}

std::string VvcCodecConfigInfo::LevelString() const {
    return VvcLevelString(general_level_idc);
}

// VVC 的色度采样换算表（H.266 6.2）：
//   chroma_format_idc: 0=4:0:0  1=4:2:0  2=4:2:2  3=4:4:4
int VvcSpsInfo::SubWidthC() const {
    return (sps_chroma_format_idc == 1 || sps_chroma_format_idc == 2) ? 2 : 1;
}

int VvcSpsInfo::SubHeightC() const { return (sps_chroma_format_idc == 1) ? 2 : 1; }

int VvcSpsInfo::width() const {
    int w = sps_pic_width_max_in_luma_samples;
    if (sps_conformance_window_flag) {
        w -= (sps_conf_win_left_offset + sps_conf_win_right_offset) * SubWidthC();
    }
    return w > 0 ? w : 0;
}

int VvcSpsInfo::height() const {
    int h = sps_pic_height_max_in_luma_samples;
    if (sps_conformance_window_flag) {
        h -= (sps_conf_win_top_offset + sps_conf_win_bottom_offset) * SubHeightC();
    }
    return h > 0 ? h : 0;
}

int VvcPpsInfo::SubWidthC() const {
    return (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
}

int VvcPpsInfo::SubHeightC() const { return (chroma_format_idc == 1) ? 2 : 1; }

int VvcPpsInfo::width() const {
    int w = pps_pic_width_in_luma_samples;
    if (pps_conformance_window_flag) {
        w -= (pps_conf_win_left_offset + pps_conf_win_right_offset) * SubWidthC();
    }
    return w > 0 ? w : 0;
}

int VvcPpsInfo::height() const {
    int h = pps_pic_height_in_luma_samples;
    if (pps_conformance_window_flag) {
        h -= (pps_conf_win_top_offset + pps_conf_win_bottom_offset) * SubHeightC();
    }
    return h > 0 ? h : 0;
}

// --------------------------------------------------------------------------
// BitstreamAnalysisResult
// --------------------------------------------------------------------------
void BitstreamAnalysisResult::AddInconsistency(const std::string& field,
                                              const std::string& container_value,
                                              const std::string& bitstream_value,
                                              const std::string& description,
                                              const std::string& suggestion) {
    Inconsistency item;
    item.field = field;
    item.container_value = container_value;
    item.bitstream_value = bitstream_value;
    item.severity = "warning";
    item.description = description;
    item.suggestion = suggestion;
    inconsistencies.push_back(item);
}

std::string BitstreamAnalysisResult::Summary() const {
    std::ostringstream oss;
    oss << "Codec: " << (codec_name.empty() ? std::string("unknown") : codec_name) << "\n";
    oss << "Stream index: " << stream_index << "\n";
    oss << "Resolution: " << width << "x" << height << "\n";
    oss << "Bit depth: " << bit_depth << "\n";
    oss << "Color: primaries=" << color_primaries
        << " transfer=" << transfer_characteristics
        << " matrix=" << matrix_coefficients << "\n";
    oss << "NAL units: " << nal_units.size()
        << "  OBU units: " << obu_units.size() << "\n";
    oss << "Inconsistencies: " << inconsistencies.size() << "\n";
    return oss.str();
}

std::string BitstreamAnalysisResult::ToJson() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"analyzed\": " << (analyzed ? "true" : "false") << ",\n";
    oss << "  \"stream_index\": " << stream_index << ",\n";
    oss << "  \"codec_name\": " << JsonString(codec_name) << ",\n";
    oss << "  \"width\": " << width << ",\n";
    oss << "  \"height\": " << height << ",\n";
    oss << "  \"bit_depth\": " << bit_depth << ",\n";
    oss << "  \"color_primaries\": " << color_primaries << ",\n";
    oss << "  \"transfer_characteristics\": " << transfer_characteristics << ",\n";
    oss << "  \"matrix_coefficients\": " << matrix_coefficients << ",\n";

    if (has_container) {
        oss << "  \"container\": {\n";
        oss << "    \"width\": " << container_width << ",\n";
        oss << "    \"height\": " << container_height << ",\n";
        oss << "    \"bit_depth\": " << container_bit_depth << ",\n";
        oss << "    \"color_primaries\": " << container_color_primaries << ",\n";
        oss << "    \"transfer_characteristics\": " << container_transfer_characteristics << ",\n";
        oss << "    \"matrix_coefficients\": " << container_matrix_coefficients << ",\n";
        oss << "    \"color_range\": " << container_color_range << "\n";
        oss << "  },\n";
    }

    oss << "  \"nal_unit_count\": " << nal_units.size() << ",\n";
    oss << "  \"obu_unit_count\": " << obu_units.size() << ",\n";

    if (has_h264) {
        oss << "  \"h264\": {\n";
        oss << "    \"sps_present\": " << (h264_sps.present ? "true" : "false") << ",\n";
        oss << "    \"profile\": " << JsonString(h264_sps.ProfileName()) << ",\n";
        oss << "    \"level\": " << JsonString(h264_sps.LevelVersion()) << ",\n";
        oss << "    \"width\": " << h264_sps.width() << ",\n";
        oss << "    \"height\": " << h264_sps.height() << ",\n";
        oss << "    \"chroma_format_idc\": " << h264_sps.chroma_format_idc << ",\n";
        oss << "    \"bit_depth_luma\": " << h264_sps.BitDepthLuma() << ",\n";
        oss << "    \"bit_depth_chroma\": " << h264_sps.BitDepthChroma() << "\n";
        oss << "  },\n";
    }

    if (has_hevc) {
        oss << "  \"hevc\": {\n";
        oss << "    \"sps_present\": " << (hevc_sps.present ? "true" : "false") << ",\n";
        oss << "    \"profile\": " << JsonString(hevc_sps.ProfileName()) << ",\n";
        oss << "    \"width\": " << hevc_sps.width() << ",\n";
        oss << "    \"height\": " << hevc_sps.height() << ",\n";
        oss << "    \"chroma_format_idc\": " << hevc_sps.chroma_format_idc << ",\n";
        oss << "    \"bit_depth_luma\": " << hevc_sps.BitDepthLuma() << ",\n";
        oss << "    \"bit_depth_chroma\": " << hevc_sps.BitDepthChroma() << "\n";
        oss << "  },\n";
    }

    if (has_av1) {
        oss << "  \"av1\": {\n";
        oss << "    \"seq_header_present\": " << (av1_seq_header.present ? "true" : "false") << ",\n";
        oss << "    \"profile\": " << av1_seq_header.profile << ",\n";
        oss << "    \"frame_width\": " << av1_seq_header.FrameWidth() << ",\n";
        oss << "    \"frame_height\": " << av1_seq_header.FrameHeight() << ",\n";
        oss << "    \"bit_depth_minus_8\": " << av1_seq_header.bit_depth_minus_8 << "\n";
        oss << "  },\n";
    }

    if (has_vvc) {
        oss << "  \"vvc\": {\n";
        oss << "    \"sps_present\": " << (vvc_sps.present ? "true" : "false") << ",\n";
        oss << "    \"bit_depth_luma\": " << vvc_sps.BitDepthLuma() << ",\n";
        oss << "    \"bit_depth_chroma\": " << vvc_sps.BitDepthChroma() << "\n";
        oss << "  },\n";
    }

    oss << "  \"inconsistencies\": [\n";
    for (size_t i = 0; i < inconsistencies.size(); ++i) {
        const Inconsistency& item = inconsistencies[i];
        oss << "    {\n";
        oss << "      \"field\": " << JsonString(item.field) << ",\n";
        oss << "      \"container_value\": " << JsonString(item.container_value) << ",\n";
        oss << "      \"bitstream_value\": " << JsonString(item.bitstream_value) << ",\n";
        oss << "      \"severity\": " << JsonString(item.severity) << ",\n";
        oss << "      \"description\": " << JsonString(item.description) << ",\n";
        oss << "      \"suggestion\": " << JsonString(item.suggestion) << "\n";
        oss << "    }" << (i + 1 < inconsistencies.size() ? "," : "") << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

} // namespace model
} // namespace videoeye
