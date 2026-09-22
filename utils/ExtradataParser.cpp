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

    // av1C（AV1 ISOBMFF 规范 2.3）：marker(1)=1 + version(7)=1 → 首字节 0x81，
    // 第 2 字节是 seq_profile(3) | seq_level_idx_0(5)，profile 必须 <= 2。
    // FFmpeg 从 MP4 / WebM 取出的 AV1 extradata 就是这个记录本体（通常 4 字节）。
    // 少了这一条，0x81 会被下面的 `data[0] & 0x03` 误判成 1 字节长度前缀。
    if (size >= 4 && (data[0] & 0x80) != 0 && (data[0] & 0x7F) <= 1 &&
        ((data[1] >> 5) & 0x07) <= 2) {
        return ExtradataFormat::Av1C;
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

NalUnit ExtradataParser::ParseVvcNalUnit(const uint8_t* data, size_t size) {
    NalUnit nal;

    if (size < 2) return nal;

    // VVC NAL header（H.266 7.3.1.2，共 2 字节）：
    //   forbidden_zero_bit      f(1)   byte0 bit7
    //   nuh_reserved_zero_bit   f(1)   byte0 bit6
    //   nuh_layer_id            u(6)   byte0 bit5..0
    //   nal_unit_type           u(5)   byte1 bit7..3
    //   nuh_temporal_id_plus1   u(3)   byte1 bit2..0
    //
    // ⚠️ 与 HEVC 不同：HEVC 的 type 在 byte0（(byte0 >> 1) & 0x3F），
    //    VVC 挪到了 byte1 的高 5 位。
    nal.type = static_cast<uint8_t>((data[1] >> 3) & 0x1F);

    // IDR_W_RADL(7) / IDR_N_LP(8) / CRA_NUT(9) / GDR_NUT(10) 都可作随机访问点
    if (nal.type >= 7 && nal.type <= 10) {
        nal.is_keyframe = true;
        nal.is_idr = (nal.type == 7 || nal.type == 8);
    }

    nal.size = static_cast<uint32_t>(size - 2);
    nal.data.assign(data + 2, data + size);

    return nal;
}

ObuUnit ExtradataParser::ParseAv1Obu(const uint8_t*& ptr, size_t remaining) {
    ObuUnit obu;
    if (ptr == nullptr || remaining < 1) return obu;

    // obu_header（AV1 规范 6.2.1）：
    //   obu_forbidden_bit   f(1)  bit7，必须为 0
    //   obu_type            f(4)  bit6..3
    //   obu_extension_flag  f(1)  bit2
    //   obu_has_size_field  f(1)  bit1
    //   obu_reserved_1bit   f(1)  bit0
    size_t pos = 0;
    const uint8_t header = ptr[pos++];
    if ((header >> 7) & 0x01) return obu;   // forbidden bit 为 1，不是合法 OBU 起始

    obu.type = (header >> 3) & 0x0F;
    const bool extension_flag = ((header >> 2) & 0x01) != 0;
    const bool has_size_field = ((header >> 1) & 0x01) != 0;
    obu.has_extension_header = extension_flag;

    // obu_extension_header：temporal_id f(3) / spatial_id f(2) / reserved f(3)
    if (extension_flag) {
        if (pos >= remaining) return obu;
        const uint8_t ext = ptr[pos++];
        obu.temporal_id = (ext >> 5) & 0x07;
        obu.spatial_id  = (ext >> 3) & 0x03;
    }

    // obu_size：leb128，低 7 位有效、最高位表示还有后续字节。
    // 注意每一位组要左移拼接，不是累加。
    size_t obu_size = 0;
    if (has_size_field) {
        int shift = 0;
        uint8_t byte = 0;
        do {
            if (pos >= remaining || shift > 28) return obu;
            byte = ptr[pos++];
            obu_size |= static_cast<size_t>(byte & 0x7F) << shift;
            shift += 7;
        } while ((byte & 0x80) != 0);
    } else {
        // 没有长度字段时，OBU 一直延伸到所在单元的末尾
        obu_size = remaining - pos;
    }

    // 数据被截断时能拿多少拿多少，别把整个 buffer 当成一个 OBU
    if (obu_size > remaining - pos) {
        obu_size = remaining - pos;
    }

    obu.size = static_cast<uint32_t>(obu_size);
    obu.data.assign(ptr + pos, ptr + pos + obu_size);
    obu.is_sequence_header =
        (obu.type == static_cast<uint8_t>(Av1ObuType::SequenceHeader));

    ptr += (pos + obu_size);
    return obu;
}

std::vector<ObuUnit> ExtradataParser::ExtractObuUnits(const uint8_t* data, size_t size) {
    std::vector<ObuUnit> obu_units;
    if (data == nullptr || size == 0) return obu_units;

    const uint8_t* ptr = data;
    size_t remaining = size;

    while (remaining > 0) {
        const uint8_t* before = ptr;
        ObuUnit obu = ParseAv1Obu(ptr, remaining);

        // 只在指针没有推进时停止，避免死循环。
        // 注意不能拿 `data.empty()` 当失败判据：OBU_TEMPORAL_DELIMITER
        // 是合法的 0 长度 OBU，会被误当成解析失败而提前结束。
        if (ptr <= before) break;

        obu_units.push_back(std::move(obu));
        remaining = static_cast<size_t>(data + size - ptr);
    }

    return obu_units;
}

std::vector<NalUnit> ExtradataParser::ExtractAnnBNalUnits(const uint8_t* data, size_t size,
                                                           NalSyntax syntax) {
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
            switch (syntax) {
                case NalSyntax::H264:
                    nal_units.push_back(ParseH264NalUnit(nal_begin, nal_size));
                    break;
                case NalSyntax::Hevc:
                    nal_units.push_back(ParseHevcNalUnit(nal_begin, nal_size));
                    break;
                case NalSyntax::Vvc:
                    nal_units.push_back(ParseVvcNalUnit(nal_begin, nal_size));
                    break;
                default: {
                    // 自动判定：H.264 的 NAL header 只有 1 字节且低 5 位就是 type，
                    // 而 HEVC/VVC 的 byte0 高位还带着 forbidden(0) 与 type 位。
                    // 这条启发式认不出 VVC（它的 byte0 是 nuh_layer_id）——
                    // VVC 的 extradata 走 vvcC，或由调用方显式传 NalSyntax::Vvc。
                    const uint8_t first_byte = nal_begin[0];
                    if ((first_byte & 0x1F) < 32) {
                        nal_units.push_back(ParseH264NalUnit(nal_begin, nal_size));
                    } else {
                        nal_units.push_back(ParseHevcNalUnit(nal_begin, nal_size));
                    }
                    break;
                }
            }
        }

        // 保证每次迭代都前进：next_start 至少是 start_code + sc_len
        pos = (next_start > start_code) ? next_start : (nal_begin > start_code ? nal_begin : end);
    }
    
    return nal_units;
}

ExtradataResult ExtradataParser::ParseAnnexB(const uint8_t* data, size_t size, NalSyntax syntax) {
    ExtradataResult result;
    result.format = ExtradataFormat::AnnexB;
    result.valid = true;
    
    result.nal_units = ExtractAnnBNalUnits(data, size, syntax);
    
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

ExtradataResult ExtradataParser::ParseVvcC(const uint8_t* data, size_t size) {
    ExtradataResult result;
    result.format = ExtradataFormat::VvcC;
    result.valid = true;

    // vvcC（ISO/IEC 14496-15 的 VvcDecoderConfigurationRecord）：
    //   byte0: reserved(5)=11111 | lengthSizeMinusOne(2) | ptl_present_flag(1)
    //   if ptl_present_flag:
    //     u16 : ols_idx(9) | num_sublayers(3) | constant_frame_rate(2) | chroma_format_idc(2)
    //     byte: bit_depth_minus8(3) | reserved(5)
    //     byte: reserved(2) | num_bytes_constraint_info(6)
    //     byte: general_profile_idc(7) | general_tier_flag(1)
    //     byte: general_level_idc(8)
    //     bits: frame_only(1) | multilayer(1) | constraint info(8*n-2)
    //     [num_sublayers>1] byte sublayer_level_present_flags
    //     [每个 present 的子层] byte sublayer_level_idc
    //     byte num_sub_profiles + 4 字节 / 个
    //     u16 max_picture_width / u16 max_picture_height / u16 avg_frame_rate
    //   byte num_of_arrays
    //   每组: byte(completeness|reserved(2)|NAL_unit_type(5)) [u16 numNalus]
    //         + numNalus × (u16 length + payload)
    if (size < 1) {
        result.error_message = "vvcC too small";
        return result;
    }

    const uint8_t* pos = data;
    const uint8_t* end = data + size;

    result.config.length_size_minus_one = (pos[0] >> 1) & 0x03;
    const bool ptl_present = (pos[0] & 0x01) != 0;
    ++pos;

    if (ptl_present) {
        if (pos + 2 > end) {
            result.error_message = "vvcC truncated (ols/sublayers/chroma)";
            return result;
        }
        const uint16_t ols_field = BytesToUint16BE(pos);
        pos += 2;
        result.config.num_sublayers = (ols_field >> 4) & 0x07;
        result.config.chroma_format_idc = ols_field & 0x03;

        if (pos >= end) {
            result.error_message = "vvcC truncated (bit depth)";
            return result;
        }
        result.config.bit_depth_minus_8 = (*pos >> 5) & 0x07;
        ++pos;

        if (pos >= end) {
            result.error_message = "vvcC truncated (constraint info size)";
            return result;
        }
        const int num_bytes_constraint_info = *pos & 0x3F;
        ++pos;

        if (pos + 2 > end) {
            result.error_message = "vvcC truncated (profile/level)";
            return result;
        }
        result.config.general_profile_idc = (*pos >> 1) & 0x7F;
        result.config.general_tier_flag = *pos & 0x01;
        ++pos;
        result.config.general_level_idc = *pos;
        ++pos;

        // frame_only(1) + multilayer(1) + constraint info(8*n-2) bits
        int constraint_bits = num_bytes_constraint_info * 8 - 2;
        if (constraint_bits < 0) constraint_bits = 0;
        const int ptl_bits = 2 + constraint_bits;
        if (pos + (ptl_bits + 7) / 8 > end) {
            result.error_message = "vvcC truncated (constraint bits)";
            return result;
        }
        pos += (ptl_bits + 7) / 8;

        if (result.config.num_sublayers > 1) {
            if (pos >= end) {
                result.error_message = "vvcC truncated (sublayer flags)";
                return result;
            }
            uint8_t flags = *pos++;
            for (int i = result.config.num_sublayers - 2; i >= 0; --i) {
                const bool present = ((flags >> i) & 0x01) != 0;
                if (present) {
                    if (pos >= end) {
                        result.error_message = "vvcC truncated (sublayer level)";
                        return result;
                    }
                    ++pos;  // sublayer_level_idc[i]
                }
            }
        }

        if (pos >= end) {
            result.error_message = "vvcC truncated (num_sub_profiles)";
            return result;
        }
        const int num_sub_profiles = *pos++;
        if (pos + static_cast<size_t>(num_sub_profiles) * 4 > end) {
            result.error_message = "vvcC truncated (sub profiles)";
            return result;
        }
        pos += static_cast<size_t>(num_sub_profiles) * 4;

        if (pos + 6 > end) {
            result.error_message = "vvcC truncated (max picture size)";
            return result;
        }
        result.config.max_picture_width = static_cast<int>(BytesToUint16BE(pos));
        result.config.max_picture_height = static_cast<int>(BytesToUint16BE(pos + 2));
        pos += 6;  // width + height + avg_frame_rate
    }

    if (pos >= end) {
        result.error_message = "vvcC truncated (num_of_arrays)";
        return result;
    }
    const int num_of_arrays = *pos++;
    result.width = result.config.max_picture_width;
    result.height = result.config.max_picture_height;

    for (int i = 0; i < num_of_arrays; ++i) {
        if (pos >= end) break;
        const uint8_t array_header = *pos++;
        const uint8_t nal_type = array_header & 0x1F;

        // DCI(13) / OPI(12) 组不带 numNalus 字段，固定 1 个
        int num_nalus = 1;
        if (nal_type != 13 && nal_type != 12) {
            if (pos + 2 > end) break;
            num_nalus = static_cast<int>(BytesToUint16BE(pos));
            pos += 2;
        }

        for (int j = 0; j < num_nalus; ++j) {
            if (pos + 2 > end) break;
            const uint16_t nal_length = BytesToUint16BE(pos);
            pos += 2;
            if (pos + nal_length > end) break;

            // 与 AnnexB 路径一致：只保留 RBSP payload（剥掉 2 字节 NAL header）
            NalUnit nal = ParseVvcNalUnit(pos, nal_length);
            nal.type = nal_type;  // 以数组头声明的类型为准
            result.nal_units.push_back(std::move(nal));
            pos += nal_length;
        }
    }

    return result;
}

ExtradataResult ExtradataParser::ParseAv1C(const uint8_t* data, size_t size) {
    ExtradataResult result;
    result.format = ExtradataFormat::Av1C;
    result.valid = true;
    
    // av1C 记录最小 4 字节：
    //   https://aomediacodec.github.io/av1-isobmff/#av1c-record-structure
    //
    //   byte0: marker(1)=1 | version(7)=1
    //   byte1: seq_profile(3) | seq_level_idx_0(5)
    //   byte2: seq_tier_0(1) | high_bitdepth(1) | twelve_bit(1) | monochrome(1)
    //          | chroma_subsampling_x(1) | chroma_subsampling_y(1) | chroma_sample_position(2)
    //   byte3: reserved(3) | initial_presentation_delay_present(1)
    //          | [initial_presentation_delay_minus_one(4)]
    //   之后: configOBUs[]（可选，可能是空的）
    //
    // ⚠️ 旧实现在这里读 frame_width/height_minus_1 —— av1C **不含宽高**，
    // 那两个字节实际是 seq_profile/level/tier/bitdepth/subsampling，
    // 读出来的分辨率是纯垃圾。宽高只能从序列头 OBU 拿。
    if (size < 4) {
        result.valid = false;
        result.error_message = "av1C too small";
        return result;
    }

    result.config.profile = (data[1] >> 5) & 0x07;
    result.config.level_idc = (data[1] & 0x1F);   // seq_level_idx_0
    result.config.general_tier_flag = (data[2] >> 7) & 0x01;
    result.config.high_bitdepth = (data[2] >> 6) & 0x01;
    result.config.twelve_bit = (data[2] >> 5) & 0x01;
    result.config.monochrome = (data[2] >> 4) & 0x01;
    result.config.chroma_subsampling_x = (data[2] >> 3) & 0x01;
    result.config.chroma_subsampling_y = (data[2] >> 2) & 0x01;
    result.config.chroma_sample_position = (data[2] & 0x03);

    if ((data[3] >> 4) & 0x01) {
        result.config.initial_presentation_delay_bits = (data[3] & 0x0F) + 1;
    }

    // 位深：profile 2 且 high_bitdepth 时 twelve_bit 决定 10/12，否则 8/10
    if (result.config.profile == 2 && result.config.high_bitdepth) {
        result.config.bit_depth_minus_8 = result.config.twelve_bit ? 4 : 2;
    } else {
        result.config.bit_depth_minus_8 = result.config.high_bitdepth ? 2 : 0;
    }

    // width / height 不在 av1C 里，保持 0（调用方应改用序列头 OBU 的值）。
    result.width = 0;
    result.height = 0;

    // configOBUs：有些封装会把序列头 OBU 放在 av1C 尾部
    if (size > 4) {
        result.obu_units = ExtractObuUnits(data + 4, size - 4);
    }

    return result;
}

ExtradataResult ExtradataParser::ParseWithFormat(ExtradataFormat format,
                                                 const uint8_t* data, size_t size,
                                                 NalSyntax syntax) {
    switch (format) {
        case ExtradataFormat::AnnexB:
            return ParseAnnexB(data, size, syntax);
            
        case ExtradataFormat::AvcC:
            return ParseAvcC(data, size);
            
        case ExtradataFormat::HvcC:
            return ParseHvcC(data, size);
            
        case ExtradataFormat::VvcC:
            return ParseVvcC(data, size);
            
        case ExtradataFormat::Av1C:
            return ParseAv1C(data, size);
            
        case ExtradataFormat::LengthPrefix1:
        case ExtradataFormat::LengthPrefix2:
        case ExtradataFormat::LengthPrefix4:
            // Convert to Annex B and parse
            return ParseAnnexB(data, size, syntax);
            
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
