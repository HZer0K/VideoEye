#include "core/analyzer/Av1BitstreamParser.h"
#include "utils/ExtradataParser.h"

namespace videoeye {
namespace analyzer {

std::string Av1BitstreamParser::GetProfileName(int profile) {
    switch (profile) {
        case 0: return "Profile 0";
        case 1: return "Profile 1";
        case 2: return "Profile 2";
        default: return "Unknown";
    }
}

bool Av1BitstreamParser::IsSequenceHeaderObu(const utils::ObuUnit& obu_unit) {
    return obu_unit.type == static_cast<uint8_t>(utils::Av1ObuType::SequenceHeader);
}

model::Av1SequenceHeaderInfo Av1BitstreamParser::ParseFromObuUnit(const utils::ObuUnit& obu_unit) {
    if (!IsSequenceHeaderObu(obu_unit)) {
        return model::Av1SequenceHeaderInfo();
    }
    
    model::Av1SequenceHeaderInfo seq_header;
    
    if (obu_unit.data.empty()) {
        return seq_header;
    }
    
    const uint8_t* ptr = obu_unit.data.data();
    const uint8_t* end = ptr + obu_unit.size;
    
    // AV1 Sequence Header OBU structure:
    // https://aomediacodec.github.io/av1-isobmff/#sequence-header-obu-structure
    
    // profile (3 bits) | frame_width_minus_1 (12 bits) | ...
    if (ptr + 3 > end) return seq_header;
    
    uint32_t first_word = (static_cast<uint32_t>(ptr[0]) << 16) |
                         (static_cast<uint32_t>(ptr[1]) << 8) |
                         static_cast<uint32_t>(ptr[2]);
    
    seq_header.profile = (first_word >> 13) & 0x07;
    seq_header.frame_width_minus_1 = first_word & 0x0FFF;
    
    ptr += 3;
    
    // frame_height_minus_1 (12 bits) | bit_depth_minus_8 (4 bits)
    if (ptr + 2 > end) return seq_header;
    
    uint32_t second_word = (static_cast<uint32_t>(ptr[0]) << 8) |
                          static_cast<uint32_t>(ptr[1]);
    
    seq_header.frame_height_minus_1 = (second_word >> 12) & 0x0FFF;
    seq_header.bit_depth_minus_8 = (second_word >> 8) & 0x0F;
    
    ptr += 2;
    
    // color_config (1 byte)
    if (ptr >= end) return seq_header;
    
    uint8_t color_config_byte = *ptr++;
    seq_header.color_config.high_bitdepth = (color_config_byte >> 7) & 0x01;
    seq_header.color_config.twelve_bit = (color_config_byte >> 6) & 0x01;
    seq_header.color_config.subsampling_x = (color_config_byte >> 5) & 0x01;
    seq_header.color_config.subsampling_y = (color_config_byte >> 4) & 0x01;
    seq_header.color_config.full_range_flag = (color_config_byte >> 3) & 0x01;
    seq_header.color_config.color_primaries = color_config_byte & 0x07;
    
    seq_header.present = true;
    
    return seq_header;
}

model::Av1SequenceHeaderInfo Av1BitstreamParser::ParseSequenceHeader(const uint8_t* extradata, size_t size) {
    auto result = utils::ExtradataParser::Parse(extradata, size);
    
    for (const auto& obu : result.obu_units) {
        if (IsSequenceHeaderObu(obu)) {
            return ParseFromObuUnit(obu);
        }
    }
    
    return model::Av1SequenceHeaderInfo();
}

} // namespace analyzer
} // namespace videoeye
