// ImageOrientation（EXIF / irot 方向解析）单元测试
//
// 为什么单独一份：
//   方向解析以前借 Qt 的 QImageReader::transformation()，只在部署了对应图片插件时
//   才有值 —— 本仓库只部署 qgif/qico/qjpeg，所以 TIFF 的 EXIF 分支实际是死代码，
//   PNG/WebP 的 EXIF 也从来没有被覆盖。改成自己解析字节之后，这个判断必须能脱离
//   文件系统、脱离 Qt 被逐条钉住：方向值错一位就是"竖拍照片横着显示"，而且很难
//   在界面测试里发现（画面本身是"对的像素、错的朝向"）。
//
// 样本全部在内存里手工构造：只造到 IFD0 那一层，不含像素数据 —— 解析器本来就只读元数据。

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/domain/model/ImageOrientation.h"

namespace {

using videoeye::model::ImageOrientation;
using videoeye::model::ReadImageOrientation;

using Bytes = std::vector<std::uint8_t>;

Bytes TiffBlock(std::uint16_t orientation, bool little_endian) {
    Bytes out;
    const auto push16 = [&out, little_endian](std::uint16_t v) {
        if (little_endian) {
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        } else {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
    };
    const auto push32 = [&out, little_endian](std::uint32_t v) {
        if (little_endian) {
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
            out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
        } else {
            out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
    };

    out.push_back(little_endian ? 'I' : 'M');
    out.push_back(little_endian ? 'I' : 'M');
    push16(42);
    push32(8);      // IFD0 紧跟 8 字节头
    push16(1);      // 一个条目
    push16(0x0112); // Orientation
    push16(3);      // SHORT
    push32(1);      // count
    push16(orientation);
    push16(0); // 值字段补齐 4 字节
    push32(0); // 下一个 IFD
    return out;
}

void AppendBigEndian32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void AppendLittleEndian32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void AppendAscii(Bytes& out, const char* text, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<std::uint8_t>(text[i]));
    }
}

Bytes JpegWithOrientation(std::uint16_t orientation) {
    const Bytes tiff = TiffBlock(orientation, true);
    Bytes out = {0xFF, 0xD8}; // SOI
    // APP1：长度字段包含它自己那 2 字节
    out.push_back(0xFF);
    out.push_back(0xE1);
    const std::size_t segment_size = 2 + 6 + tiff.size();
    out.push_back(static_cast<std::uint8_t>(segment_size >> 8));
    out.push_back(static_cast<std::uint8_t>(segment_size & 0xFF));
    AppendAscii(out, "Exif\0\0", 6);
    out.insert(out.end(), tiff.begin(), tiff.end());
    out.push_back(0xFF);
    out.push_back(0xD9); // EOI
    return out;
}

Bytes JpegWithoutExif() {
    Bytes out = {0xFF, 0xD8};
    // APP0 (JFIF)：与方向无关，解析器必须跳过它继续找
    out.push_back(0xFF);
    out.push_back(0xE0);
    out.push_back(0x00);
    out.push_back(0x04);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0xFF);
    out.push_back(0xD9);
    return out;
}

Bytes PngWithOrientation(std::uint16_t orientation, bool prefix_exif_marker) {
    static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes out(kSignature, kSignature + 8);
    Bytes payload;
    if (prefix_exif_marker) {
        AppendAscii(payload, "Exif\0\0", 6);
    }
    const Bytes tiff = TiffBlock(orientation, true);
    payload.insert(payload.end(), tiff.begin(), tiff.end());

    AppendBigEndian32(out, static_cast<std::uint32_t>(payload.size()));
    AppendAscii(out, "eXIf", 4);
    out.insert(out.end(), payload.begin(), payload.end());
    AppendBigEndian32(out, 0); // CRC 不参与解析

    AppendBigEndian32(out, 13);
    AppendAscii(out, "IEND", 4);
    AppendBigEndian32(out, 0);
    return out;
}

Bytes WebpWithOrientation(std::uint16_t orientation, bool prefix_exif_marker, bool odd_padding_chunk) {
    Bytes body;
    if (odd_padding_chunk) {
        // 长度 3 的 ICCP 块：载荷后必须补 1 字节对齐，解析器若漏掉 padding 会读错块头
        AppendAscii(body, "ICCP", 4);
        AppendLittleEndian32(body, 3);
        body.push_back('a');
        body.push_back('b');
        body.push_back('c');
        body.push_back(0);
    }
    Bytes payload;
    if (prefix_exif_marker) {
        AppendAscii(payload, "Exif\0\0", 6);
    }
    const Bytes tiff = TiffBlock(orientation, false); // 顺带覆盖大端 IFD
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    AppendAscii(body, "EXIF", 4);
    AppendLittleEndian32(body, static_cast<std::uint32_t>(payload.size()));
    body.insert(body.end(), payload.begin(), payload.end());
    if (payload.size() % 2 == 1) {
        body.push_back(0);
    }

    Bytes out;
    AppendAscii(out, "RIFF", 4);
    AppendLittleEndian32(out, static_cast<std::uint32_t>(body.size() + 4));
    AppendAscii(out, "WEBP", 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// 最小 ISOBMFF：ftyp 盒 + 一个 irot 属性盒（size 固定 9 + 1 字节角度）。
Bytes IsobmffWithIrot(std::uint8_t raw_angle, std::uint32_t box_size) {
    Bytes out;
    AppendBigEndian32(out, 16);
    AppendAscii(out, "ftyp", 4);
    AppendAscii(out, "heic", 4);
    AppendBigEndian32(out, 0);
    AppendBigEndian32(out, box_size);
    AppendAscii(out, "irot", 4);
    out.push_back(raw_angle);
    return out;
}

ImageOrientation Read(const Bytes& bytes) {
    return ReadImageOrientation(bytes.data(), bytes.size());
}

} // namespace

// EXIF 1..8 的完整映射。逐条钉住，避免把 5/7（转置/反转置）写成同一个值 ——
// 这两个值本来就低频，肉眼又看不出区别。
TEST(ImageOrientationTest, JpegCoversAllEightExifValues) {
    const std::pair<std::uint16_t, ImageOrientation> cases[] = {
        {1, ImageOrientation::None},       {2, ImageOrientation::MirrorHorizontal},
        {3, ImageOrientation::Rotate180},  {4, ImageOrientation::MirrorVertical},
        {5, ImageOrientation::Transpose},  {6, ImageOrientation::Rotate90Cw},
        {7, ImageOrientation::Transverse}, {8, ImageOrientation::Rotate270Cw},
    };
    for (const auto& [value, expected] : cases) {
        EXPECT_EQ(expected, Read(JpegWithOrientation(value))) << "EXIF orientation = " << value;
    }
}

TEST(ImageOrientationTest, JpegWithoutApp1IsIdentity) {
    EXPECT_EQ(ImageOrientation::None, Read(JpegWithoutExif()));
}

// 越界是这里最容易出事的地方：读到的头部窗口可能刚好切在 IFD 中间。
TEST(ImageOrientationTest, TruncatedInputNeverInventsOrientation) {
    const Bytes full = JpegWithOrientation(6);
    // APP1 头已经完整，但 IFD0 被切掉一部分
    for (std::size_t cut :
         {static_cast<std::size_t>(2), static_cast<std::size_t>(12), static_cast<std::size_t>(16), full.size() - 4}) {
        EXPECT_EQ(ImageOrientation::None, ReadImageOrientation(full.data(), cut)) << "cut = " << cut;
    }
    EXPECT_EQ(ImageOrientation::None, ReadImageOrientation(static_cast<const std::uint8_t*>(nullptr), 0));
    const Bytes garbage(32, 0x5A);
    EXPECT_EQ(ImageOrientation::None, Read(garbage));
}

TEST(ImageOrientationTest, TiffHandlesBothByteOrders) {
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(TiffBlock(6, true)));
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(TiffBlock(6, false)));
    // 非 42 的魔数不是 TIFF
    Bytes broken = TiffBlock(6, true);
    broken[3] = 0x2B;
    EXPECT_EQ(ImageOrientation::None, Read(broken));
}

// IFD0 的偏移指向缓冲区之外（超大 TIFF 只读了头部窗口）时必须放弃，不能读到别处。
TEST(ImageOrientationTest, TiffWithIfdOutsideWindowIsIgnored) {
    Bytes bytes = TiffBlock(6, true);
    bytes[4] = 0xFF; // IFD0 偏移 = 0xFF
    bytes[5] = 0x00;
    bytes[6] = 0x00;
    bytes[7] = 0x00;
    EXPECT_EQ(ImageOrientation::None, Read(bytes));
}

TEST(ImageOrientationTest, PngReadsEXifChunkWithOrWithoutMarkerPrefix) {
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(PngWithOrientation(6, false)));
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(PngWithOrientation(6, true)));
    EXPECT_EQ(ImageOrientation::Rotate180, Read(PngWithOrientation(3, false)));
}

TEST(ImageOrientationTest, WebpReadsEXifChunkAndHonoursRiffPadding) {
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(WebpWithOrientation(6, true, false)));
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(WebpWithOrientation(6, false, false)));
    // 前面放一个奇数长度的块：漏掉 RIFF 的 1 字节对齐就会读错块头
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(WebpWithOrientation(6, true, true)));
}

// HEIF 的 irot 是"逆时针角度"，与 EXIF 的顺时针语义相反：1 → 顺时针 270，3 → 顺时针 90。
TEST(ImageOrientationTest, IsobmffIrotUsesCounterClockwiseSemantics) {
    EXPECT_EQ(ImageOrientation::None, Read(IsobmffWithIrot(0, 9)));
    EXPECT_EQ(ImageOrientation::Rotate270Cw, Read(IsobmffWithIrot(1, 9)));
    EXPECT_EQ(ImageOrientation::Rotate180, Read(IsobmffWithIrot(2, 9)));
    EXPECT_EQ(ImageOrientation::Rotate90Cw, Read(IsobmffWithIrot(3, 9)));
}

// 压缩数据里可能出现 "irot" 字样；只有盒长度合法（9）才认，避免把像素数据当属性。
TEST(ImageOrientationTest, IsobmffIgnoresIrotTextWithoutValidBoxHeader) {
    EXPECT_EQ(ImageOrientation::None, Read(IsobmffWithIrot(3, 13)));
}

// 回归守卫：.xyz / 无签名数据不能因为"看起来像"就被判成某种容器。
TEST(ImageOrientationTest, UnknownContainerIsIdentity) {
    Bytes bytes = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(ImageOrientation::None, Read(bytes));
}
