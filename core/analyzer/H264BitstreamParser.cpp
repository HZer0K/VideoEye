#include "core/analyzer/H264BitstreamParser.h"
#include "utils/BitReader.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

std::string H264BitstreamParser::GetProfileName(int profile_idc) {
    switch (profile_idc) {
        case 66: return "Baseline";
        case 77: return "Main";
        case 88: return "Extended";
        case 100: return "High";
        case 110: return "High 10";
        case 122: return "High 4:2:2";
        case 244: return "High 4:4:4";
        default: return "Unknown";
    }
}

std::string H264BitstreamParser::GetLevelVersion(int level_idc) {
    if (level_idc == 0) return "Undefined";
    
    int major = level_idc / 10;
    int minor = level_idc % 10;
    
    // Level names based on ITU-T H.264
    static const char* levels[] = {
        "1", "1b", "1.1", "1.2", "1.3", "2", "2.1", "2.2",
        "3", "3.1", "3.2", "4", "4.1", "5", "5.1", "5.2",
        "6", "6.1", "6.2"
    };
    
    if (major >= 0 && major <= 18) {
        return std::string(levels[major]) + "." + std::to_string(minor);
    }
    
    return "Unknown";
}

void H264BitstreamParser::ParseProfileLevelInfo(uint32_t profile_idc, 
                                                uint32_t level_idc,
                                                int& profile, 
                                                std::string& profile_name,
                                                int& level,
                                                std::string& level_version) {
    profile = static_cast<int>(profile_idc);
    level = static_cast<int>(level_idc);
    profile_name = GetProfileName(profile);
    level_version = GetLevelVersion(level);
}

bool H264BitstreamParser::IsSpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 7; // SPS NAL type
}

bool H264BitstreamParser::IsPpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 8; // PPS NAL type
}

model::H264SpsInfo H264BitstreamParser::ParseFromNalUnit(const utils::NalUnit& nal_unit) {
    if (!IsSpsNalUnit(nal_unit)) {
        return model::H264SpsInfo();
    }
    
    model::H264SpsInfo sps;
    
    if (nal_unit.data.empty()) {
        return sps;
    }
    
    utils::BitReader reader;
    reader.Reset(nal_unit.data.data(), nal_unit.data.size());
    
    // Skip NAL header (1 byte)
    reader.SkipBits(8);
    
    // Parse SPS data
    sps.seq_parameter_set_id = static_cast<int>(reader.ReadUWord());
    
    if (sps.seq_parameter_set_id > 31) {
        return sps; // Invalid
    }
    
    // Read profile_idc
    sps.profile_idc = static_cast<int>(reader.ReadBits(8));
    
    // Read constraint_flags
    for (int i = 0; i < 5; ++i) {
        reader.ReadBits(8);
    }
    
    // Read level_idc
    sps.level_idc = static_cast<int>(reader.ReadBits(8));
    
    // Advanced frame type
    sps.vui.frame_mbs_only_flag = static_cast<int>(reader.ReadBit());
    
    if (sps.vui.frame_mbs_only_flag == 0) {
        reader.SkipBits(1); // mb_adaptive_frame_field_flag
    }
    
    // Direct 8x8 inference
    sps.direct_8x8_inference_flag = static_cast<int>(reader.ReadBit());
    
    // Chroma format
    if (sps.profile_idc != 100) { // Non-main profiles don't have chroma_format
        sps.chroma_format_idc = 1; // Default to 4:2:0
    } else {
        sps.chroma_format_idc = static_cast<int>(reader.ReadUWord());
        
        if (sps.chroma_format_idc == 3) { // 4:4:4
            sps.separate_colour_plane_flag = static_cast<int>(reader.ReadBit());
        }
        
        // Bit depth
        sps.bit_depth_luma_minus8 = static_cast<int>(reader.ReadUWord());
        sps.bit_depth_chroma_minus8 = static_cast<int>(reader.ReadUWord());
    }
    
    // QP prime
    sps.qpprime_y_zero_transform_bypass_flag = static_cast<int>(reader.ReadBit());
    
    // Scaling matrix
    sps.seq_scaling_matrix_present_flag = static_cast<int>(reader.ReadBit());
    if (sps.seq_scaling_matrix_present_flag) {
        int scaling_list_count = (sps.chroma_format_idc != 2) ? 8 : 12;
        for (int i = 0; i < scaling_list_count; ++i) {
            if (reader.ReadBit()) {
                // Skip scaling list data
                int size = (i < 6) ? 16 : 64;
                for (int j = 0; j < size; ++j) {
                    reader.ReadUWord();
                }
            }
        }
    }
    
    // Log2 max pic order cnt lsb
    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(reader.ReadUWord());
    
    // Delta picture order count
    sps.gaps_in_frame_val_allowed_flag = static_cast<int>(reader.ReadBit());
    sps.max_num_ref_frames = static_cast<int>(reader.ReadUWord());
    
    // GOP structure
    sps.pic_width_in_mbs_minus1 = static_cast<int>(reader.ReadUWord());
    sps.pic_height_in_mbs_minus1 = static_cast<int>(reader.ReadUWord());
    
    // Frame crop
    sps.frame_cropping_flag = static_cast<int>(reader.ReadBit());
    if (sps.frame_cropping_flag) {
        sps.frame_crop_left_offset = static_cast<int>(reader.ReadUWord());
        sps.frame_crop_right_offset = static_cast<int>(reader.ReadUWord());
        sps.frame_crop_top_offset = static_cast<int>(reader.ReadUWord());
        sps.frame_crop_bottom_offset = static_cast<int>(reader.ReadUWord());
    }
    
    // VUI parameters
    sps.vui.present = static_cast<bool>(reader.ReadBit());
    if (sps.vui.present) {
        sps.vui.timing_info_present = static_cast<int>(reader.ReadBit());
        if (sps.vui.timing_info_present) {
            sps.vui.num_units_in_tick = static_cast<uint32_t>(reader.ReadBits(32));
            sps.vui.time_scale = static_cast<uint32_t>(reader.ReadBits(32));
            sps.vui.fixed_frame_rate = static_cast<bool>(reader.ReadBit());
        }
        
        // Neutral chroma indication
        sps.vui.neutral_chroma_indication = static_cast<bool>(reader.ReadBit());
        
        // Field seq flag
        sps.vui.field_seq_flag = static_cast<bool>(reader.ReadBit());
        
        // VUI color info
        sps.vui.frame_mbs_only_flag = static_cast<bool>(reader.ReadBit());
        
        // Matrix/coefficient/primaries/transfer
        sps.vui.adaptive_vui_flag = static_cast<bool>(reader.ReadBit());
        if (sps.vui.adaptive_vui_flag) {
            // Simplified: skip adaptive VUI data
            reader.AlignToByte();
        }
        
        // HRD
        sps.vui.hrd_present = static_cast<bool>(reader.ReadBit());
        if (sps.vui.hrd_present) {
            sps.vui.cpb_cnt = static_cast<uint8_t>(reader.ReadUWord());
            reader.SkipBits(5); // bit_rate_scale
            reader.SkipBits(4); // cpb_size_scale
            
            for (int i = 0; i < sps.vui.cpb_cnt; ++i) {
                reader.SkipBits(4); // initial_cpb_removal_delay_length
                reader.SkipBits(4); // cpb_removal_delay_length
                reader.SkipBits(5); // dpb_output_delay_length
                reader.SkipBits(5); // time_offset_length
            }
        }
    }
    
    sps.present = true;
    
    return sps;
}

model::H264SpsInfo H264BitstreamParser::ParseSpf(const uint8_t* extradata, size_t size) {
    // First try to extract NAL units from extradata
    auto result = utils::ExtradataParser::Parse(extradata, size);
    
    for (const auto& nal : result.nal_units) {
        if (IsSpsNalUnit(nal)) {
            return ParseFromNalUnit(nal);
        }
    }
    
    return model::H264SpsInfo();
}

model::H264PpsInfo H264BitstreamParser::ParsePps(const uint8_t* extradata, size_t size) {
    auto result = utils::ExtradataParser::Parse(extradata, size);
    
    for (const auto& nal : result.nal_units) {
        if (IsPpsNalUnit(nal)) {
            // Simplified PPS parsing
            model::H264PpsInfo pps;
            pps.present = true;
            return pps;
        }
    }
    
    return model::H264PpsInfo();
}

model::H264VuiInfo H264BitstreamParser::ParseVui(const uint8_t* extradata, size_t size) {
    auto sps = ParseSpf(extradata, size);
    return sps.vui;
}

} // namespace analyzer
} // namespace videoeye
