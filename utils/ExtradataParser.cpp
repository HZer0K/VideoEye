#include "utils/ExtradataParser.h"
#include "utils/BitReader.h"

namespace videoeye {
namespace utils {

// 查找 Annex B 起始码：00 00 00 01 或 00 00 01
const uint8_t* ExtradataParser::FindStartCode(const uint8_t* pos, const uint8_t* end) {
    if (pos + 4 > end) return nullptr;
    
    while (pos + 4 <= end) {
        // 检查 00 00 00 01
        if (pos[0] == 0 && pos[1] == 0 && pos[2] == 0 && pos[3] == 1) {
            return pos;
        }
        
        // 检查 00 00 01
        if (pos + 3 <= end && pos[0] == 0 && pos[1] == 0 && pos[2] == 1) {
            return pos;
        }
        
        pos++;
    }
    
    return nullptr;
}

ExtradataFormat ExtradataParser::DetectFormat(const uint8_t* data, size_t size) {
    if (size < 4) {
        return ExtradataFormat::Unknown;
    }
    
    // 检查 avcC/hvcC/av1C配置记录
    if (size >= 11) {
        // 检查前 4 字节是否为 box type
        uint32_t box_type = BytesToUint32BE(data);
        
        // avcC: "\0\0\0\rcvcC"
        if (box_type == 0x0000000A) { // length = 10
            if (data[4] == 'a' && data[5] == 'v' && data[6] == 'c' && data[7] == 'C') {
                return ExtradataFormat::AvcC;
            }
        }
        
        // hvcC: MP4 HEVC configuration record
        if (box_type == 0x00000014) { // length = 20
            if (data[4] == 'h' && data[5] == 'v' && data[6] == 'c' && data[7] == 'C') {
                return ExtradataFormat::HvcC;
            }
        }
        
        // av1C: AV1 configuration record
        if (box_type == 0x00000019) { // length = 25
            if (data[4] == 'a' && data[5] == 'v' && data[6] == '1' && data[7] == 'C') {
                return ExtradataFormat::Av1C;
            }
        }
    }
    
    // FFmpeg 从 MP4 里取出来的 extradata 是配置记录本体，不含 box header。
    // avcC 的特征是 configurationVersion=1 + 第 5 字节的高 6 位保留位全 1（0xFC|n）。
    // 少了这一条，MP4 里的 H.264 extradata 会被误判成「长度前缀」格式。
    if (size >= 8 && data[0] == 1 && (data[4] & 0xFC) == 0xFC) {
        return ExtradataFormat::AvcC;
    }

    // 检查 Annex B（起始码格式）
    if (size >= 4) {
        const uint8_t* start_code = FindStartCode(data, data + size);
        if (start_code != nullptr) {
            return ExtradataFormat::AnnexB;
        }
    }
    
    // 检查长度前缀格式（通常 extradata 的第一个字节是 lengthSize-1）
    if (size >= 1) {
        int length_size = data[0] & 0x03;
        
        if (length_size == 1 && size >= 5) {
            return ExtradataFormat::LengthPrefix1;
        } else if (length_size == 2 && size >= 6) {
            return ExtradataFormat::LengthPrefix2;
        } else if (length_size == 3 && size >= 7) {
            return ExtradataFormat::LengthPrefix4;
        }
    }
    
    return ExtradataFormat::Unknown;
}

std::vector<uint8_t> ExtradataParser::ConvertAnnexBToLengthPrefix(
    const std::vector<uint8_t>& annex_b) {
    
    std::vector<uint8_t> result;
    const uint8_t* pos = annex_b.data();
    const uint8_t* end = pos + annex_b.size();
    
    while (pos < end) {
        const uint8_t* start_code = FindStartCode(pos, end);
        
        if (start_code == nullptr) {
            // 没有更多起始码，添加剩余数据
            result.insert(result.end(), pos, end);
            break;
        }
        
        // 添加起始码之前的数据（如果有）
        if (start_code > pos) {
            result.insert(result.end(), pos, start_code);
        }
        
        // 跳过起始码
        size_t skip = (start_code[3] == 1 && start_code[2] == 0) ? 4 : 3;
        pos = start_code + skip;
    }
    
    return result;
}

std::vector<uint8_t> ExtradataParser::ConvertLengthPrefixToAnnexB(
    const std::vector<uint8_t>& length_prefix, int prefix_bytes) {
    
    std::vector<uint8_t> result;
    const uint8_t* pos = length_prefix.data();
    const uint8_t* end = pos + length_prefix.size();
    
    while (pos < end) {
        // 读取长度前缀
        uint32_t nal_size = 0;
        
        if (prefix_bytes == 1) {
            if (pos >= end) break;
            nal_size = *pos++;
        } else if (prefix_bytes == 2) {
            if (pos + 2 > end) break;
            nal_size = (static_cast<uint32_t>(pos[0]) << 8) | pos[1];
            pos += 2;
        } else if (prefix_bytes == 4) {
            if (pos + 4 > end) break;
            nal_size = (static_cast<uint32_t>(pos[0]) << 24) |
                      (static_cast<uint32_t>(pos[1]) << 16) |
                      (static_cast<uint32_t>(pos[2]) << 8) |
                      static_cast<uint32_t>(pos[3]);
            pos += 4;
        }
        
        if (pos + nal_size > end) break;
        
        // 添加 Annex B 起始码
        result.push_back(0);
        result.push_back(0);
        result.push_back(0);
        result.push_back(1);
        
        // 添加 NAL 单元数据
        result.insert(result.end(), pos, pos + nal_size);
        
        pos += nal_size;
    }
    
    return result;
}

NalUnit ExtradataParser::ParseH264NalUnit(const uint8_t* data, size_t size) {
    NalUnit nal;
    
    if (size < 1) return nal;
    
    // H.264 NAL header: [forbidden_zero_bit: 1][nal_unit_type: 5][nuh_layer_id: 6][nuh_temporal_id_plus1: 3]
    uint8_t header = data[0];
    nal.type = header & 0x1F; // 低 5 位
    
    // 检查是否为 IDR 帧
    if (nal.type == 5) { // CodedSliceDciIdr
        nal.is_idr = true;
        nal.is_keyframe = true;
    } else if (nal.type >= 1 && nal.type <= 12) {
        nal.is_keyframe = false;
    }
    
    nal.size = static_cast<uint32_t>(size - 1);
    nal.data.assign(data + 1, data + size);
    
    return nal;
}

NalUnit ExtradataParser::ParseHevcNalUnit(const uint8_t* data, size_t size) {
    NalUnit nal;
    
    if (size < 2) return nal;
    
    // HEVC NAL header: [forbidden_zero_bit: 1][nuh_reserved_zero_2bits: 2][nal_unit_type: 6][nuh_layer_id: 6][nuh_temporal_id_plus1: 3]
    uint16_t header = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    nal.type = (header >> 3) & 0x3F; // 位 3-8
    
    // 检查关键帧（CLVS 通常是关键帧）
    if (nal.type >= 32 && nal.type <= 35) {
        nal.is_keyframe = true;
    }
    
    nal.size = static_cast<uint32_t>(size - 2);
    nal.data.assign(data + 2, data + size);
    
    return nal;
}

ObuUnit ExtradataParser::ParseAv1Obu(const uint8_t*& ptr, size_t remaining) {
    ObuUnit obu;
    
    if (remaining < 1) return obu;
    
    uint8_t header = *ptr++;
    remaining--;
    
    obu.type = header >> 1;
    obu.has_extension_header = (header & 0x01) != 0;
    
    // 读取 OBU 大小（变长编码）
    uint32_t size = 0;
    bool more = true;
    
    while (more && remaining >= 1) {
        uint8_t byte = *ptr++;
        remaining--;
        size += (byte & 0x7F);
        more = (byte & 0x80) != 0;
        
        if (more && remaining < 1 && size > 0) {
            // 需要更多字节但没有了
            break;
        }
    }
    
    if (remaining >= size) {
        obu.size = size;
        obu.data.assign(ptr, ptr + size);
        
        // 检查是否为序列头
        if (obu.type == static_cast<uint8_t>(Av1ObuType::SequenceHeader)) {
            obu.is_sequence_header = true;
        }
        
        ptr += size;
    }
    
    return obu;
}

std::vector<NalUnit> ExtradataParser::ExtractAnnBNalUnits(const uint8_t* data, size_t size) {
    std::vector<NalUnit> nal_units;
    
    const uint8_t* pos = data;
    const uint8_t* end = data + size;
    
    while (pos < end) {
        const uint8_t* start_code = FindStartCode(pos, end);
        
        if (start_code == nullptr) {
            break;
        }
        
        // 起始码可能是 4 字节 (00 00 00 01) 或 3 字节 (00 00 01)
        // 00 00 00 01 的第 3 个字节是 0，00 00 01 的第 3 个字节是 1 —— 据此区分
        const size_t sc_len = (start_code[2] == 1) ? 3u : 4u;
        const uint8_t* nal_begin = start_code + sc_len;

        // 下一个起始码；找不到就吃到缓冲区末尾（最后一个 NAL 通常没有后继起始码）。
        // 旧实现在这里写成了 while 循环却从不推进 next_start，一旦出现两个起始码
        // 就会死循环；同时末尾那个 NAL 的长度会被算成 0 而整个丢掉。
        const uint8_t* next_start = end;
        const uint8_t* sc = FindStartCode(nal_begin, end);
        if (sc != nullptr) next_start = sc;

        const size_t nal_size = static_cast<size_t>(next_start - nal_begin);

        if (nal_size > 0) {
            // 尝试判断是 H.264 还是 H.265
            const uint8_t first_byte = nal_begin[0];

            // 简单启发式：如果第一个字节的高 5 位 < 32，可能是 H.264
            if ((first_byte & 0x1F) < 32) {
                nal_units.push_back(ParseH264NalUnit(nal_begin, nal_size));
            } else {
                nal_units.push_back(ParseHevcNalUnit(nal_begin, nal_size));
            }
        }

        // 保证每次迭代都前进：next_start 至少是 start_code + sc_len
        pos = (next_start > start_code) ? next_start : (nal_begin > start_code ? nal_begin : end);
    }
    
    return nal_units;
}

ExtradataResult ExtradataParser::ParseAnnexB(const uint8_t* data, size_t size) {
    ExtradataResult result;
    result.format = ExtradataFormat::AnnexB;
    result.valid = true;
    
    result.nal_units = ExtractAnnBNalUnits(data, size);
    
    return result;
}

ExtradataResult ExtradataParser::ParseAvcC(const uint8_t* data, size_t size) {
    ExtradataResult result;
    result.format = ExtradataFormat::AvcC;
    result.valid = true;
    
    if (size < 13) {
        result.error_message = "avcC too small";
        return result;
    }
    
    // Parse avcC configuration record
    // Structure:
    //   version (1 byte)
    //   profile_index (1 byte)
    //   profile_compatibility (1 byte)
    //   level_index (1 byte)
    //   length_size_minus_one (1 byte, bits 6-7)
    //   reserved (6 bits)
    //   num_sps (1 byte, bits 1-5)
    //   sps[]
    
    result.config.profile_idc = data[1];
    result.config.profile_compatibility = data[2];
    result.config.level_idc = data[3];
    // lengthSizeMinusOne 在第 5 个字节的低 2 位（高 6 位保留，恒为 1 → 0xFC|n）
    result.config.length_size_minus_one = data[4] & 0x03;

    // numOfSequenceParameterSets 在第 6 个字节的低 5 位（高 3 位保留 → 0xE0|n），
    // SPS 数组紧随其后，即从第 7 个字节开始。旧实现整体往后错了一个字节，
    // 读出来的 num_sps 和 SPS 偏移都是错的。
    const uint8_t num_sps = data[5] & 0x1F;

    const uint8_t* pos = data + 6;
    
    for (int i = 0; i < num_sps; ++i) {
        if (pos + 4 > data + size) break;
        
        uint16_t sps_length = (static_cast<uint16_t>(pos[0]) << 8) | pos[1];
        pos += 2;
        
        if (pos + sps_length > data + size) break;
        
        // 统一不变量：NalUnit::data 只放 RBSP payload，**不含 NAL header**
        // （与 ExtractAnnBNalUnits / ParseH264NalUnit 的 AnnexB 路径保持一致）。
        // 上层 parser 因此无需再判断"header 在不在"。
        if (sps_length < 1) break;
        NalUnit nal;
        nal.type = 7; // SPS
        nal.size = sps_length - 1;
        nal.data.assign(pos + 1, pos + sps_length);
        nal.is_keyframe = true;
        
        result.nal_units.push_back(nal);
        
        pos += sps_length;
    }
    
    // Parse PPS
    if (pos + 1 > data + size) return result;
    
    uint8_t num_pps = *pos++;
    
    for (int i = 0; i < num_pps; ++i) {
        if (pos + 4 > data + size) break;
        
        uint16_t pps_length = (static_cast<uint16_t>(pos[0]) << 8) | pos[1];
        pos += 2;
        
        if (pos + pps_length > data + size) break;
        
        if (pps_length < 1) break;
        NalUnit nal;
        nal.type = 8; // PPS
        nal.size = pps_length - 1;
        nal.data.assign(pos + 1, pos + pps_length);
        
        result.nal_units.push_back(nal);
        
        pos += pps_length;
    }
    
    return result;
}

ExtradataResult ExtradataParser::ParseHvcC(const uint8_t* data, size_t size) {
    ExtradataResult result;
    result.format = ExtradataFormat::HvcC;
    result.valid = true;
    
    if (size < 12) {
        result.error_message = "hvcC too small";
        return result;
    }
    
    // Parse hvcC configuration record
    // Simplified parsing
    
    int offset = 0;
    
    // version
    result.config.general_profile_space = data[offset + 1] >> 6;
    result.config.general_tier_flag = (data[offset + 1] >> 5) & 0x01;
    result.config.general_profile_idc = data[offset + 1] & 0x1F;
    
    offset += 20; // Skip to config_NAL_bit_depth_luma_minus8
    
    return result;
}

ExtradataResult ExtradataParser::ParseAv1C(const uint8_t* data, size_t size) {
    ExtradataResult result;
    result.format = ExtradataFormat::Av1C;
    result.valid = true;
    
    if (size < 9) {
        result.error_message = "av1C too small";
        return result;
    }
    
    // Parse av1C configuration record
    // https://aomediacodec.github.io/av1-isobmff/#av1c-record-structure
    
    int offset = 0;
    
    // marker (2 bits) | profile (3 bits) | format (1 bit)
    result.config.profile = (data[offset] >> 5) & 0x07;
    
    offset++;
    
    // frame_width_minus_1 (2 bytes)
    result.width = BytesToUint16BE(data + offset) + 1;
    offset += 2;
    
    // frame_height_minus_1 (2 bytes)
    result.height = BytesToUint16BE(data + offset) + 1;
    offset += 2;
    
    // bit_depth_minus_8 (1 byte)
    result.config.bit_depth_minus_8 = data[offset];
    offset++;
    
    // color_config (1 byte)
    uint8_t color_config_byte = data[offset];
    result.config.high_bitdepth = (color_config_byte >> 7) & 0x01;
    result.config.twelve_bit = (color_config_byte >> 6) & 0x01;
    result.config.chroma_subsampling_x = (color_config_byte >> 5) & 0x01;
    result.config.chroma_subsampling_y = (color_config_byte >> 4) & 0x01;
    result.config.color_range = (color_config_byte >> 3) & 0x01;
    result.config.color_primaries = (color_config_byte >> 0) & 0x07;
    
    offset++;
    
    // More fields...
    
    return result;
}

ExtradataResult ExtradataParser::ParseWithFormat(ExtradataFormat format,
                                                 const uint8_t* data, size_t size) {
    switch (format) {
        case ExtradataFormat::AnnexB:
            return ParseAnnexB(data, size);
            
        case ExtradataFormat::AvcC:
            return ParseAvcC(data, size);
            
        case ExtradataFormat::HvcC:
            return ParseHvcC(data, size);
            
        case ExtradataFormat::Av1C:
            return ParseAv1C(data, size);
            
        case ExtradataFormat::LengthPrefix1:
        case ExtradataFormat::LengthPrefix2:
        case ExtradataFormat::LengthPrefix4:
            // Convert to Annex B and parse
            return ParseWithFormat(ExtradataFormat::AnnexB, data, size);
            
        default:
            ExtradataResult result;
            result.format = ExtradataFormat::Unknown;
            result.valid = false;
            result.error_message = "Unsupported format";
            return result;
    }
}

ExtradataResult ExtradataParser::Parse(const uint8_t* extradata, size_t size) {
    if (extradata == nullptr || size == 0) {
        ExtradataResult result;
        result.valid = false;
        result.error_message = "Empty extradata";
        return result;
    }
    
    ExtradataFormat format = DetectFormat(extradata, size);
    
    if (format == ExtradataFormat::Unknown) {
        ExtradataResult result;
        result.valid = false;
        result.error_message = "Unknown format";
        return result;
    }
    
    return ParseWithFormat(format, extradata, size);
}

} // namespace utils
} // namespace videoeye
