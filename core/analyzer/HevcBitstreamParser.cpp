#include "core/analyzer/HevcBitstreamParser.h"
#include "utils/BitReader.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

std::string HevcBitstreamParser::GetProfileName(int general_profile_idc) {
    switch (general_profile_idc) {
        case 0: return "Main";
        case 1: return "Main 10";
        case 2: return "Main Still Picture";
        case 3: return "TMEE";
        case 4: return "Range Extension";
        case 5: return "Multipoint Extension";
        case 6: return "Scc Extension";
        default: return "Unknown";
    }
}

std::string HevcBitstreamParser::GetTierName(int general_tier_flag) {
    return general_tier_flag ? "High" : "Main";
}

std::string HevcBitstreamParser::GetLevelVersion(uint32_t general_level_idc) {
    if (general_level_idc == 0) return "Undefined";
    
    int level = static_cast<int>(general_level_idc / 30);
    int sub_level = general_level_idc % 30;
    
    // HEVC levels: 1, 2, 2.1, 3, 3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, 6.2
    static const char* levels[] = {
        "1", "2", "2.1", "3", "3.1", "4", "4.1", "5", "5.1", "5.2",
        "6", "6.1", "6.2", "7", "7.1", "8", "8.1", "9", "9.1", "9.2",
        "10", "10.1", "10.2", "10.3", "10.4", "10.5", "10.6", "10.7", "10.8", "10.9",
        "11", "11.1"
    };
    
    if (level >= 0 && level <= 31) {
        return std::string(levels[level]) + "." + std::to_string(sub_level);
    }
    
    return "Unknown";
}

bool HevcBitstreamParser::IsVpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 32; // VPS NAL type
}

bool HevcBitstreamParser::IsSpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 33; // SPS NAL type
}

bool HevcBitstreamParser::IsPpsNalUnit(const utils::NalUnit& nal_unit) {
    return nal_unit.type == 34; // PPS NAL type
}

model::HevcVpsInfo HevcBitstreamParser::ParseVpsFromNalUnit(const utils::NalUnit& nal_unit) {
    if (!IsVpsNalUnit(nal_unit)) {
        return model::HevcVpsInfo();
    }
    
    model::HevcVpsInfo vps;
    
    if (nal_unit.data.empty()) {
        return vps;
    }
    
    utils::BitReader reader;
    reader.ResetFromData(nal_unit.data.data(), nal_unit.data.size());
    
    // Skip NAL header (1 byte)
    reader.SkipBits(8);
    
    // Parse VPS data
    vps.vps_video_parameter_set_id = static_cast<int>(reader.ReadBits(4));
    vps.vps_max_layers = static_cast<int>(reader.ReadBits(6));
    vps.vps_max_sub_layers = static_cast<int>(reader.ReadBits(3));
    vps.vps_temporal_nal_layer_only_flag = static_cast<int>(reader.ReadBit());
    vps.vps_ptl_following_flag = static_cast<int>(reader.ReadBit());
    
    if (vps.vps_ptl_following_flag) {
        vps.general_profile_space = static_cast<int>(reader.ReadBits(2));
        vps.general_tier_flag = static_cast<int>(reader.ReadBit());
        vps.general_profile_idc = static_cast<int>(reader.ReadBits(5));
        
        uint32_t profile_compatibility = reader.ReadBits(32);
        bool general_profile_compatibility_flag = (profile_compatibility & 0x80000000) != 0;
        
        for (int i = 0; i < 31; ++i) {
            if ((profile_compatibility & 0x80000000) != 0) {
                profile_compatibility <<= 1;
            } else {
                break;
            }
        }
        
        vps.general_level_idc = reader.ReadBits(8);
        
        // More PTL parsing...
    }
    
    vps.present = true;
    
    return vps;
}

model::HevcSpsInfo HevcBitstreamParser::ParseSpfFromNalUnit(const utils::NalUnit& nal_unit) {
    if (!IsSpsNalUnit(nal_unit)) {
        return model::HevcSpsInfo();
    }
    
    model::HevcSpsInfo sps;
    
    if (nal_unit.data.empty()) {
        return sps;
    }
    
    utils::BitReader reader;
    reader.ResetFromData(nal_unit.data.data(), nal_unit.data.size());
    
    // Skip NAL header (1 byte)
    reader.SkipBits(8);
    
    // Parse SPS data
    sps.sps_seq_parameter_set_id = static_cast<int>(reader.ReadUWord());
    
    if (sps.sps_seq_parameter_set_id > 63) {
        return sps; // Invalid
    }
    
    // Chroma format
    sps.chroma_format_idc = static_cast<int>(reader.ReadUWord());
    
    if (sps.chroma_format_idc == 3) {
        sps.separate_colour_plane_flag = static_cast<int>(reader.ReadBit());
    }
    
    // Frame size
    sps.pic_width_in_ctu_minus1 = static_cast<int>(reader.ReadUWord());
    sps.pic_height_in_ctu_minus1 = static_cast<int>(reader.ReadUWord());
    
    // Bit depth
    sps.bit_depth_luma_minus8 = static_cast<int>(reader.ReadUWord());
    sps.bit_depth_chroma_minus8 = static_cast<int>(reader.ReadUWord());
    
    // Log2 min coding block size
    sps.log2_min_luma_coding_block_size_minus3 = static_cast<int>(reader.ReadUWord());
    sps.log2_diff_max_min_luma_coding_block_size = static_cast<int>(reader.ReadUWord());
    
    // Scaling list
    sps.scaling_list_enable_flag = static_cast<int>(reader.ReadBit());
    if (sps.scaling_list_enable_flag) {
        // Skip scaling list data
        reader.SkipBits(1); // scaling_list_data_present_flag
        
        for (int i = 0; i < 8; ++i) {
            if (reader.ReadBit()) {
                // Skip scaling list
                int size = (i < 6) ? 16 : 64;
                for (int j = 0; j < size; ++j) {
                    reader.ReadUWord();
                }
            }
        }
    }
    
    // Transform skip
    sps.transform_skip_enabled_flag = static_cast<int>(reader.ReadBit());
    if (sps.transform_skip_enabled_flag) {
        sps.log2_transform_skip_max_size_minus2 = static_cast<int>(reader.ReadUWord());
    }
    
    // Context adaptive entropy
    sps.cabac_init_present_flag = static_cast<int>(reader.ReadBit());
    
    // CU partition
    sps.split_cu_delta_flag = static_cast<int>(reader.ReadBit());
    sps.log2_diff_cu_min_qp_idx_minus1 = static_cast<int>(reader.ReadUWord());
    
    // Long term reference
    sps.long_term_ref_pics_present_flag = static_cast<int>(reader.ReadBit());
    sps.num_long_term_ref_pics_sps = static_cast<int>(reader.ReadUWord());
    
    // STSA
    sps.stref_pic_all_flag = static_cast<int>(reader.ReadBit());
    
    // Temporal motion vectors
    sps.temporal_mvp_enabled_flag = static_cast<int>(reader.ReadBit());
    
    // Stronger smoothing
    sps.strongly_smoothed_img_between_grids_flag = static_cast<int>(reader.ReadBit());
    
    // Sign data hiding
    sps.sign_data_hiding_enabled_flag = static_cast<int>(reader.ReadBit());
    
    // CABAC
    sps.cabac_bypass_alignment_enabled_flag = static_cast<int>(reader.ReadBit());
    
    // SAO
    sps.sample_adaptive_offset_enabled_flag = static_cast<int>(reader.ReadBit());
    
    // PCM
    sps.pcm_enabled_flag = static_cast<int>(reader.ReadBit());
    if (sps.pcm_enabled_flag) {
        sps.pcm_sample_bit_depth_luma_minus1 = static_cast<int>(reader.ReadBits(4));
        sps.pcm_sample_bit_depth_chroma_minus1 = static_cast<int>(reader.ReadBits(4));
        sps.log2_min_pcm_luma_coding_block_size = static_cast<int>(reader.ReadUWord());
        sps.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<int>(reader.ReadUWord());
        reader.SkipBits(1); // pcm_loop_filter_disabled_flag
    }
    
    // FP
    sps.four_twenty_two_log2_conversion_enable_flag = static_cast<int>(reader.ReadBit());
    sps.separate_colour_plane_flag = static_cast<int>(reader.ReadBit());
    
    // VUI
    sps.vui_parameters_present_flag = static_cast<int>(reader.ReadBit());
    if (sps.vui_parameters_present_flag) {
        sps.aspect_ratio_info_present_flag = static_cast<int>(reader.ReadBit());
        if (sps.aspect_ratio_info_present_flag) {
            sps.sar_width = static_cast<int>(reader.ReadBits(16));
            sps.sar_height = static_cast<int>(reader.ReadBits(16));
            
            if (reader.ReadBit()) { // overscan_info_present_flag
                reader.SkipBits(1); // overscan_appropriate_flag
            }
            
            if (reader.ReadBit()) { // video_signal_type_present_flag
                reader.SkipBits(3); // video_format
                sps.video_full_range_flag = static_cast<int>(reader.ReadBit());
                
                if (reader.ReadBit()) { // colour_description_present_flag
                    sps.colour_primaries = static_cast<int>(reader.ReadBits(8));
                    sps.transfer_characteristics = static_cast<int>(reader.ReadBits(8));
                    sps.matrix_coefficients = static_cast<int>(reader.ReadBits(8));
                }
            }
            
            if (reader.ReadBit()) { // chroma_loc_info_present_flag
                sps.chroma_sample_loc_type_top_field = static_cast<int>(reader.ReadUWord());
                sps.chroma_sample_loc_type_bottom_field = static_cast<int>(reader.ReadUWord());
            }
            
            reader.SkipBits(1); // neutral_chroma_indication_present_flag
            
            if (reader.ReadBit()) { // field_seq_flag
                // field_seq_flag handling
            }
            
            reader.SkipBits(1); // frame_field_info_present_flag
            
            if (reader.ReadBit()) { // default_display_window_flag
                sps.def_disp_win_left_offset = static_cast<int>(reader.ReadUWord());
                sps.def_disp_win_right_offset = static_cast<int>(reader.ReadUWord());
                sps.def_disp_win_top_offset = static_cast<int>(reader.ReadUWord());
                sps.def_disp_win_bottom_offset = static_cast<int>(reader.ReadUWord());
            }
            
            reader.SkipBits(1); // vui_timing_info_present_flag
            if (sps.vui_timing_info_present_flag) {
                sps.vui_num_units_in_tick = static_cast<uint32_t>(reader.ReadBits(32));
                sps.vui_time_scale = static_cast<uint32_t>(reader.ReadBits(32));
                reader.SkipBits(1); // vui_poc_proportional_to_tc_flag
                reader.SkipBits(1); // vui_hrd_params_present_flag
            }
            
            reader.SkipBits(1); // bitstream_restriction_flag
            if (sps.bitstream_restriction_flag) {
                reader.SkipBits(1); // tiles_fixed_structure_flag
                reader.SkipBits(1); // moving_images_allowed_flag
                sps.max_bytes_per_pic_denom = static_cast<int>(reader.ReadUWord());
                sps.max_bits_per_min_cu_denom = static_cast<int>(reader.ReadUWord());
                sps.log2_max_mv_length_horizontal = static_cast<int>(reader.ReadUWord());
                sps.log2_max_mv_length_vertical = static_cast<int>(reader.ReadUWord());
            }
        }
    }
    
    sps.present = true;
    
    return sps;
}

model::HevcPpsInfo HevcBitstreamParser::ParsePpsFromNalUnit(const utils::NalUnit& nal_unit) {
    if (!IsPpsNalUnit(nal_unit)) {
        return model::HevcPpsInfo();
    }
    
    model::HevcPpsInfo pps;
    
    if (nal_unit.data.empty()) {
        return pps;
    }
    
    utils::BitReader reader;
    reader.ResetFromData(nal_unit.data.data(), nal_unit.data.size());
    
    // Skip NAL header (1 byte)
    reader.SkipBits(8);
    
    // Parse PPS data
    pps.pic_parameter_set_id = static_cast<int>(reader.ReadUWord());
    pps.seq_parameter_set_id = static_cast<int>(reader.ReadUWord());
    pps.dependent_slice_segments_enabled_flag = static_cast<int>(reader.ReadBit());
    pps.output_flag_present_flag = static_cast<int>(reader.ReadBit());
    pps.num_extra_slice_header_bits = static_cast<int>(reader.ReadBits(3));
    pps.sign_data_hiding_enabled_flag = static_cast<int>(reader.ReadBit());
    pps.cabac_init_present_flag = static_cast<int>(reader.ReadBit());
    pps.num_ref_idx_l0_default_active_minus1 = static_cast<int>(reader.ReadUWord());
    pps.num_ref_idx_l1_default_active_minus1 = static_cast<int>(reader.ReadUWord());
    pps.init_qp_minus26 = static_cast<int>(reader.ReadBits(6));
    pps.constrained_intra_pred_flag = static_cast<int>(reader.ReadBit());
    pps.redundant_pic_cnt_present_flag = static_cast<int>(reader.ReadBit());
    pps.transform_skip_enabled_flag = static_cast<int>(reader.ReadBit());
    pps.cu_qp_offset_enabled_flag = static_cast<int>(reader.ReadBit());
    
    if (pps.cu_qp_offset_enabled_flag) {
        pps.diff_cu_chroma_qp_offset_depth = static_cast<int>(reader.ReadUWord());
        pps.chroma_qp_index_offset = static_cast<int>(reader.ReadBits(7));
        pps.second_chroma_qp_index_offset = static_cast<int>(reader.ReadBits(7));
    }
    
    pps.pps_cb_offset = static_cast<int>(reader.ReadBits(6));
    pps.pps_cr_offset = static_cast<int>(reader.ReadBits(6));
    
    pps.present = true;
    
    return pps;
}

model::HevcVpsInfo HevcBitstreamParser::ParseVps(const uint8_t* extradata, size_t size) {
    auto result = utils::ExtradataParser::Parse(extradata, size);
    
    for (const auto& nal : result.nal_units) {
        if (IsVpsNalUnit(nal)) {
            return ParseVpsFromNalUnit(nal);
        }
    }
    
    return model::HevcVpsInfo();
}

model::HevcSpsInfo HevcBitstreamParser::ParseSpf(const uint8_t* extradata, size_t size) {
    auto result = utils::ExtradataParser::Parse(extradata, size);
    
    for (const auto& nal : result.nal_units) {
        if (IsSpsNalUnit(nal)) {
            return ParseSpfFromNalUnit(nal);
        }
    }
    
    return model::HevcSpsInfo();
}

model::HevcPpsInfo HevcBitstreamParser::ParsePps(const uint8_t* extradata, size_t size) {
    auto result = utils::ExtradataParser::Parse(extradata, size);
    
    for (const auto& nal : result.nal_units) {
        if (IsPpsNalUnit(nal)) {
            return ParsePpsFromNalUnit(nal);
        }
    }
    
    return model::HevcPpsInfo();
}

std::vector<model::HevcSeiMessage> HevcBitstreamParser::ParseSeiMessages(
    const uint8_t* data, size_t size) {
    std::vector<model::HevcSeiMessage> sei_messages;
    
    if (size < 2) {
        return sei_messages;
    }
    
    utils::BitReader reader;
    reader.ResetFromData(data, size);
    
    while (reader.AvailableBytes() > 0) {
        // Read SEI payload size (variable length)
        uint32_t payload_size = 0;
        uint32_t temp_size = 0;
        
        do {
            temp_size = reader.ReadBits(8);
            payload_size += temp_size;
            if (temp_size < 255) break;
        } while (reader.AvailableBytes() > 0);
        
        if (payload_size == 0) break;
        
        // Read SEI message type
        uint32_t sei_message_type = 0;
        do {
            sei_message_type += reader.ReadBits(8);
            if (sei_message_type < 255) break;
        } while (reader.AvailableBytes() > 0);
        
        model::HevcSeiMessage sei;
        sei.present = true;
        sei.type = static_cast<model::HevcSeiMessage::Type>(sei_message_type);
        
        // Read SEI payload
        if (reader.AvailableBytes() >= payload_size) {
            std::vector<uint8_t> payload(payload_size);
            for (uint32_t i = 0; i < payload_size; ++i) {
                payload[i] = static_cast<uint8_t>(reader.ReadBits(8));
            }
            sei.data = payload;
            sei_messages.push_back(sei);
        }
        
        // Align to byte boundary
        reader.AlignToByte();
    }
    
    return sei_messages;
}

} // namespace analyzer
} // namespace videoeye
