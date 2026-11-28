#include "core/domain/model/ImageOrientation.h"

#include <cstring>

namespace videoeye::model {
namespace {

constexpr std::uint16_t kOrientationTag = 0x0112;
constexpr std::uint16_t kTiffShortType = 3;

bool StartsWith(const std::uint8_t* data, std::size_t size, const char* signature, std::size_t signature_size) {
    return size >= signature_size && std::memcmp(data, signature, signature_size) == 0;
}

std::uint16_t ReadU16(const std::uint8_t* p, bool little_endian) {
    if (little_endian) {
        return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t ReadU32(const std::uint8_t* p, bool little_endian) {
    if (little_endian) {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

ImageOrientation FromExifValue(std::uint32_t value) {
    if (value < 1 || value > 8) {
        return ImageOrientation::None;
    }
    return static_cast<ImageOrientation>(value);
}

// TIFF 结构（Exif 载荷 / TIFF 文件本体）的 IFD0 解析。
// 任何越界都按"没有方向信息"处理：宁可原样显示，也不要拿错位的数据去旋转。
ImageOrientation ParseTiff(const std::uint8_t* data, std::size_t size) {
    if (size < 8) {
        return ImageOrientation::None;
    }
    bool little_endian = false;
    if (data[0] == 'I' && data[1] == 'I') {
        little_endian = true;
    } else if (data[0] == 'M' && data[1] == 'M') {
        little_endian = false;
    } else {
        return ImageOrientation::None;
    }
    if (ReadU16(data + 2, little_endian) != 42) {
        return ImageOrientation::None;
    }
    const std::uint32_t ifd_offset = ReadU32(data + 4, little_endian);
    if (ifd_offset < 8 || ifd_offset > size - 2) {
        return ImageOrientation::None; // IFD0 不在读到的窗口内（文件极大时可能如此）
    }
    const std::size_t entry_count = ReadU16(data + ifd_offset, little_endian);
    if (entry_count > (size - ifd_offset - 2) / 12) {
        return ImageOrientation::None; // 截断的 IFD
    }
    for (std::size_t i = 0; i < entry_count; ++i) {
        const std::uint8_t* entry = data + ifd_offset + 2 + i * 12;
        if (ReadU16(entry, little_endian) != kOrientationTag) {
            continue;
        }
        // 规范里 Orientation 是 SHORT 单值，直接落在 4 字节值字段里。
        // 非规范写法（类型/个数不对）不猜测，按无方向处理。
        if (ReadU16(entry + 2, little_endian) != kTiffShortType) {
            return ImageOrientation::None;
        }
        if (ReadU32(entry + 4, little_endian) != 1) {
            return ImageOrientation::None;
        }
        return FromExifValue(ReadU16(entry + 8, little_endian));
    }
    return ImageOrientation::None;
}

// Exif 载荷可能带 "Exif\0\0" 前缀（JPEG APP1、部分 WebP/PNG 写入器），也可能不带。
ImageOrientation ParseExifPayload(const std::uint8_t* data, std::size_t size) {
    if (size >= 6 && std::memcmp(data, "Exif\0\0", 6) == 0) {
        data += 6;
        size -= 6;
    }
    return ParseTiff(data, size);
}

ImageOrientation ParseJpeg(const std::uint8_t* data, std::size_t size) {
    std::size_t pos = 2; // 跳过 SOI
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) {
            return ImageOrientation::None; // 段结构异常，不再往下猜
        }
        const std::uint8_t marker = data[pos + 1];
        if (marker == 0xFF) { // 填充字节
            ++pos;
            continue;
        }
        // 无载荷的标记
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            return ImageOrientation::None; // 已进入扫描数据，前面没有 APP1
        }
        const std::size_t segment_size = (static_cast<std::size_t>(data[pos + 2]) << 8) | data[pos + 3];
        if (segment_size < 2u || pos + 2 + segment_size > size) {
            return ImageOrientation::None;
        }
        if (marker == 0xE1 && segment_size >= 8 && std::memcmp(data + pos + 4, "Exif\0\0", 6) == 0) {
            return ParseExifPayload(data + pos + 4, segment_size - 2);
        }
        pos += 2 + segment_size;
    }
    return ImageOrientation::None;
}

ImageOrientation ParsePng(const std::uint8_t* data, std::size_t size) {
    // 8 字节签名之后是 [长度(4, BE) 类型(4) 载荷 CRC(4)] 的块序列。
    std::size_t pos = 8;
    while (pos + 12 <= size) {
        const std::uint32_t chunk_size = ReadU32(data + pos, false);
        if (chunk_size > size - pos - 12) {
            return ImageOrientation::None; // 块被截断
        }
        const bool is_exif = std::memcmp(data + pos + 4, "eXIf", 4) == 0;
        if (is_exif) {
            return ParseExifPayload(data + pos + 8, chunk_size);
        }
        // eXIf 必须在 IDAT 之前才能省下解压；见到 IDAT/IEND 就没必要继续。
        if (std::memcmp(data + pos + 4, "IDAT", 4) == 0 || std::memcmp(data + pos + 4, "IEND", 4) == 0) {
            return ImageOrientation::None;
        }
        pos += 12 + static_cast<std::size_t>(chunk_size);
    }
    return ImageOrientation::None;
}

ImageOrientation ParseWebp(const std::uint8_t* data, std::size_t size) {
    // RIFF(4) + 文件长度(4, LE) + WEBP(4)，之后是 [FourCC(4) 长度(4, LE) 载荷]，
    // 载荷长度为奇数时后面补 1 字节对齐。
    std::size_t pos = 12;
    while (pos + 8 <= size) {
        const std::uint32_t chunk_size = ReadU32(data + pos + 4, true);
        const std::size_t payload = pos + 8;
        if (chunk_size > size - payload) {
            return ImageOrientation::None;
        }
        if (std::memcmp(data + pos, "EXIF", 4) == 0) {
            return ParseExifPayload(data + payload, chunk_size);
        }
        pos = payload + chunk_size + (chunk_size & 1u);
    }
    return ImageOrientation::None;
}

// HEIF / AVIF 的方向写在 meta 里的 irot 属性盒：4 字节 size(=9) + "irot" + 1 字节角度。
// 角度低 2 位是"逆时针 90° 的倍数"，与 EXIF 的顺时针语义相反，需要换算。
// 只认 size 恰好为 9 的规范盒：压缩数据里偶尔出现的 "irot" 字样不会配上一个合法的盒头。
ImageOrientation ParseIsobmff(const std::uint8_t* data, std::size_t size) {
    for (std::size_t pos = 4; pos + 5 <= size; ++pos) {
        if (std::memcmp(data + pos, "irot", 4) != 0) {
            continue;
        }
        if (ReadU32(data + pos - 4, false) != 9) {
            continue;
        }
        switch (data[pos + 4] & 0x03) {
        case 1:
            return ImageOrientation::Rotate270Cw; // 逆时针 90 == 顺时针 270
        case 2:
            return ImageOrientation::Rotate180;
        case 3:
            return ImageOrientation::Rotate90Cw; // 逆时针 270 == 顺时针 90
        default:
            return ImageOrientation::None;
        }
    }
    return ImageOrientation::None;
}

} // namespace

ImageOrientation ReadImageOrientation(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 8) {
        return ImageOrientation::None;
    }
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return ParseJpeg(data, size);
    }
    static constexpr char kPngSignature[] = "\x89PNG\r\n\x1a\n";
    if (StartsWith(data, size, kPngSignature, 8)) {
        return ParsePng(data, size);
    }
    if (StartsWith(data, size, "RIFF", 4) && size >= 12 && std::memcmp(data + 8, "WEBP", 4) == 0) {
        return ParseWebp(data, size);
    }
    // TIFF 魔数按字节序落位：II 是 2A 00，MM 是 00 2A。
    if ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) ||
        (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A)) {
        return ParseTiff(data, size);
    }
    // ISOBMFF：4 字节盒长度之后是 "ftyp"。HEIC / HEIF / AVIF 都落在这里。
    if (size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0) {
        return ParseIsobmff(data, size);
    }
    return ImageOrientation::None;
}

} // namespace videoeye::model
