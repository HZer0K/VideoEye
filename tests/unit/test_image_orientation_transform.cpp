// OrientationToTransform（EXIF 方向 → QTransform）单元测试
//
// 为什么单独一份：
//   方向换算错一位，画面就变成"对的像素、错的朝向" —— 竖拍照片横躺、镜像翻错边，
//   界面上看起来像"文件本身就是这样"。用法也最容易抄错：EXIF 的 5/7 是转置/反转置，
//   写成"旋转 + 镜像"时很容易把两者写反。
//   这里用 3x2 的唯一色像素图，按 EXIF 定义逐像素核对每个源点落到哪个目标点，
//   同时钉住"5/7/6/8 会交换宽高、2/3/4 不会"。

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>

#include "core/domain/model/ImageOrientation.h"
#include "ui/player/ImageOrientationTransform.h"

namespace {

using videoeye::model::ImageOrientation;
using videoeye::ui::OrientationToTransform;

constexpr int kWidth = 3;
constexpr int kHeight = 2;

// 每个像素给一个唯一且可逆的颜色（RGB 三通道各存一份坐标）。
QImage MakeProbeImage() {
    QImage image(kWidth, kHeight, QImage::Format_RGB32);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            image.setPixel(x, y, qRgb(16 + x, 32 + y, 64 + x * 4 + y));
        }
    }
    return image;
}

QRgb PixelAt(const QImage& image, int x, int y) {
    return image.pixel(x, y);
}

struct Case {
    ImageOrientation orientation;
    bool swaps_dimensions;
    // 源像素 (x,y) 旋转后在显示图上的坐标
    int (*map_x)(int x, int y);
    int (*map_y)(int x, int y);
};

int X(int x, int) {
    return x;
}
int Y(int, int y) {
    return y;
}
int FlippedX(int x, int) {
    return kWidth - 1 - x;
}
int FlippedY(int, int y) {
    return kHeight - 1 - y;
}
int SwapX(int, int y) {
    return y;
}
int SwapY(int x, int) {
    return x;
}
// 顺时针 90：源 (x,y) 落在 (h-1-y, x)
int Rot90X(int, int y) {
    return kHeight - 1 - y;
}
int Rot90Y(int x, int) {
    return x;
}
// 顺时针 270：源 (x,y) 落在 (y, w-1-x)
int Rot270X(int, int y) {
    return y;
}
int Rot270Y(int x, int) {
    return kWidth - 1 - x;
}
// 转置（沿主对角线）：源 (x,y) 落在 (y, x)
int TransposeX(int, int y) {
    return y;
}
int TransposeY(int x, int) {
    return x;
}
// 反转置（沿副对角线）：源 (x,y) 落在 (h-1-y, w-1-x)
int TransverseX(int, int y) {
    return kHeight - 1 - y;
}
int TransverseY(int x, int) {
    return kWidth - 1 - x;
}

const Case& CaseFor(ImageOrientation orientation) {
    static const Case kCases[] = {
        {ImageOrientation::None, false, X, Y},
        {ImageOrientation::MirrorHorizontal, false, FlippedX, Y},
        {ImageOrientation::Rotate180, false, FlippedX, FlippedY},
        {ImageOrientation::MirrorVertical, false, X, FlippedY},
        {ImageOrientation::Transpose, true, TransposeX, TransposeY},
        {ImageOrientation::Rotate90Cw, true, Rot90X, Rot90Y},
        {ImageOrientation::Transverse, true, TransverseX, TransverseY},
        {ImageOrientation::Rotate270Cw, true, Rot270X, Rot270Y},
    };
    return kCases[static_cast<int>(orientation) - 1];
}

const char* NameOf(ImageOrientation orientation) {
    switch (orientation) {
    case ImageOrientation::None:
        return "None(1)";
    case ImageOrientation::MirrorHorizontal:
        return "MirrorHorizontal(2)";
    case ImageOrientation::Rotate180:
        return "Rotate180(3)";
    case ImageOrientation::MirrorVertical:
        return "MirrorVertical(4)";
    case ImageOrientation::Transpose:
        return "Transpose(5)";
    case ImageOrientation::Rotate90Cw:
        return "Rotate90Cw(6)";
    case ImageOrientation::Transverse:
        return "Transverse(7)";
    case ImageOrientation::Rotate270Cw:
        return "Rotate270Cw(8)";
    }
    return "?";
}

} // namespace

// 逐像素核对：源图的每个像素必须出现在映射公式算出的位置上。
TEST(ImageOrientationTransformTest, EveryPixelLandsWhereExifSaysItShould) {
    const QImage source = MakeProbeImage();
    for (int value = 1; value <= 8; ++value) {
        const auto orientation = static_cast<ImageOrientation>(value);
        const Case& expected = CaseFor(orientation);
        const QImage out = source.transformed(OrientationToTransform(orientation), Qt::FastTransformation);

        const int expected_width = expected.swaps_dimensions ? kHeight : kWidth;
        const int expected_height = expected.swaps_dimensions ? kWidth : kHeight;
        ASSERT_EQ(expected_width, out.width()) << NameOf(orientation);
        ASSERT_EQ(expected_height, out.height()) << NameOf(orientation);

        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int dx = expected.map_x(x, y);
                const int dy = expected.map_y(x, y);
                EXPECT_EQ(PixelAt(source, x, y), PixelAt(out, dx, dy))
                    << NameOf(orientation) << ": 源 (" << x << "," << y << ") 应在 (" << dx << "," << dy << ")";
            }
        }
    }
}

// 5/7 必须与 6/8 区分开：这四者的宽高都交换，只看尺寸区分不出来。
// 单独钉一条"转置与旋转不同"的断言，避免把 Transpose 写成 Rotate90Cw 也能通过。
TEST(ImageOrientationTransformTest, TransposeAndTransverseDifferFromRotations) {
    const QImage source = MakeProbeImage();
    const QImage transpose =
        source.transformed(OrientationToTransform(ImageOrientation::Transpose), Qt::FastTransformation);
    const QImage rotate90 =
        source.transformed(OrientationToTransform(ImageOrientation::Rotate90Cw), Qt::FastTransformation);
    const QImage transverse =
        source.transformed(OrientationToTransform(ImageOrientation::Transverse), Qt::FastTransformation);

    EXPECT_NE(PixelAt(transpose, 0, 0), PixelAt(rotate90, 0, 0));
    EXPECT_NE(PixelAt(transverse, 0, 0), PixelAt(rotate90, 0, 0));
    // 转置与反转置互为"再沿对角线翻一次"：左上角像素来源不同
    EXPECT_NE(PixelAt(transpose, 0, 0), PixelAt(transverse, 0, 0));
}

// 没有方向信息时必须是单位变换（不能悄悄加一个 90° 旋转）。
TEST(ImageOrientationTransformTest, NoneIsIdentity) {
    const QImage source = MakeProbeImage();
    const QImage out = source.transformed(OrientationToTransform(ImageOrientation::None), Qt::FastTransformation);
    EXPECT_EQ(source, out);
}
