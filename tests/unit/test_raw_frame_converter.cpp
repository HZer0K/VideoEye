// RawFrameConverter（裸帧 → RGB888）的口径测试。
//
// 为什么值得单独测：这段逻辑以前是 RawImageSequence 里逐像素手写的 BT.601/709 系数，
// 四种 4:2:0 排布（I420/NV12/NV21/YV12）各自一套下标算术 —— 平面顺序写反不会崩，
// 只会把 U 当 V：人脸变青、天空变红，而"能显示"这件事完全看不出来。换 sws_scale 之后
// 风险从"算术写错"变成了"平面指针接错 + 范围/矩阵设错"，同样是静默偏色。
//
// 具体盯四件事：
//   1. 格式表与帧尺寸：表的标签能反解析回枚举、单帧字节数对得上真实排布，
//      4:2:0 的偶数约束不能放松（宽高为奇数时按帧读取会错位一整个色度平面）。
//   2. 同一份 YUV 换四种排布必须得到**同一幅** RGB —— 这是平面顺序的唯一落点。
//   3. 范围与矩阵：limited range 下 Y=16/235 必须映射到 0/255（旧实现直接当成 16/235，
//      黑电平发灰），BT.709 与 BT.601 在饱和色上必须真的不同。
//   4. 10bit：两种排布的**位对齐方式不同**（P010 在高位、平面 10bit 在低位），
//      等价内容必须与 8bit 结果一致 —— 这一条最容易写错，见 TenBitCode 上方的注释。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "core/player/RawFrameConverter.h"

using videoeye::player::ParseRawPixelFormatLabel;
using videoeye::player::RawFrameConverter;
using videoeye::player::RawFrameSizeBytes;
using videoeye::player::RawPixelFormat;
using videoeye::player::RawPixelFormatOption;
using videoeye::player::RawPixelFormatOptions;

namespace {

constexpr int kW = 16;
constexpr int kH = 16;

struct YuvPlanes {
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
};

YuvPlanes MakeYuv(std::uint8_t y_value, std::uint8_t u_value, std::uint8_t v_value) {
    YuvPlanes planes;
    planes.y.assign(static_cast<std::size_t>(kW) * kH, y_value);
    planes.u.assign(static_cast<std::size_t>(kW / 2) * (kH / 2), u_value);
    planes.v.assign(static_cast<std::size_t>(kW / 2) * (kH / 2), v_value);
    return planes;
}

// 每 2x2 块的色度都不同：平面顺序一旦接反，块与块之间的色偏立刻对不上。
//
// 步长取小（每块 ±2）是刻意的：平面与半平面在 sws 里走两条转换路径，块边界上的
// 色度上采样方式不同（一个线性插值、一个近邻），步长越大差得越多 —— 但那是**上采样
// 策略差异**，不是我们关心的平面顺序。U/V 接反带来的差异是几十，容差 4 两边都留得开。
YuvPlanes MakeGradientYuv() {
    YuvPlanes planes;
    planes.y.resize(static_cast<std::size_t>(kW) * kH);
    for (int j = 0; j < kH; ++j) {
        for (int i = 0; i < kW; ++i) {
            planes.y[static_cast<std::size_t>(j) * kW + i] = static_cast<std::uint8_t>(40 + i * 8 + j * 3);
        }
    }
    const int chroma_w = kW / 2;
    planes.u.resize(static_cast<std::size_t>(chroma_w) * (kH / 2));
    planes.v.resize(planes.u.size());
    for (std::size_t idx = 0; idx < planes.u.size(); ++idx) {
        const int col = static_cast<int>(idx) % chroma_w;
        const int row = static_cast<int>(idx) / chroma_w;
        planes.u[idx] = static_cast<std::uint8_t>(90 + col * 2);
        planes.v[idx] = static_cast<std::uint8_t>(150 - row * 2);
    }
    return planes;
}

std::vector<std::uint8_t> LayoutI420(const YuvPlanes& p) {
    std::vector<std::uint8_t> out(p.y);
    out.insert(out.end(), p.u.begin(), p.u.end());
    out.insert(out.end(), p.v.begin(), p.v.end());
    return out;
}

// YV12 = Y,V,U（色度平面先后与 I420 相反）
std::vector<std::uint8_t> LayoutYv12(const YuvPlanes& p) {
    std::vector<std::uint8_t> out(p.y);
    out.insert(out.end(), p.v.begin(), p.v.end());
    out.insert(out.end(), p.u.begin(), p.u.end());
    return out;
}

std::vector<std::uint8_t> LayoutNv12(const YuvPlanes& p) {
    std::vector<std::uint8_t> out(p.y);
    for (std::size_t idx = 0; idx < p.u.size(); ++idx) {
        out.push_back(p.u[idx]);
        out.push_back(p.v[idx]);
    }
    return out;
}

std::vector<std::uint8_t> LayoutNv21(const YuvPlanes& p) {
    std::vector<std::uint8_t> out(p.y);
    for (std::size_t idx = 0; idx < p.u.size(); ++idx) {
        out.push_back(p.v[idx]);
        out.push_back(p.u[idx]);
    }
    return out;
}

// 8bit 限制范围码值 → 10bit 限制范围码值：16→64、235→940，恰是 ×4。
int TenBitCode(int eight_bit) {
    return eight_bit << 2;
}

// ⚠️ 两种 10bit 排布的**位对齐不同**，这是本文件最容易写错的一处：
//   * P010（半平面，硬件常用）：10 位数据在 16bit 容器的**高位**，低 6 位补零（MSB 对齐）；
//   * yuv420p10le（平面）：10 位数据在 16bit 容器的**低位**（LSB 对齐，值就是 0..1023）。
// 拿同一套样本编码去喂两种格式，色度会被截成垃圾 —— 表现为"10bit 文件颜色完全不对"，
// 但不会报错（2026-11 实测：把 P010 的编码喂给 yuv420p10le，红色变成青色）。
// 参考口径：`ffmpeg -f rawvideo -pix_fmt gray -i g.raw -pix_fmt yuv420p10le`，
// 8bit 的 126 落盘为低位对齐的 497。
std::uint16_t P010Sample(int eight_bit) {
    return static_cast<std::uint16_t>(TenBitCode(eight_bit) << 6);
}

std::uint16_t PlanarTenBitSample(int eight_bit) {
    return static_cast<std::uint16_t>(TenBitCode(eight_bit));
}

void PushSample(std::vector<std::uint8_t>& out, std::uint16_t sample) {
    out.push_back(static_cast<std::uint8_t>(sample & 0xFF));
    out.push_back(static_cast<std::uint8_t>((sample >> 8) & 0xFF));
}

// P010：Y 平面 + UV 交织（半高行，每行 w 个样本）。
std::vector<std::uint8_t> LayoutP010(int y8, int u8, int v8) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < kW * kH; ++i) {
        PushSample(out, P010Sample(y8));
    }
    for (int i = 0; i < (kW / 2) * (kH / 2); ++i) {
        PushSample(out, P010Sample(u8));
        PushSample(out, P010Sample(v8));
    }
    return out;
}

// YUV420P10LE：Y,U,V 三个平面，每个样本 2 字节（低位对齐）。
std::vector<std::uint8_t> LayoutYuv420P10(int y8, int u8, int v8) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < kW * kH; ++i) {
        PushSample(out, PlanarTenBitSample(y8));
    }
    for (int i = 0; i < (kW / 2) * (kH / 2); ++i) {
        PushSample(out, PlanarTenBitSample(u8));
    }
    for (int i = 0; i < (kW / 2) * (kH / 2); ++i) {
        PushSample(out, PlanarTenBitSample(v8));
    }
    return out;
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb PixelAt(const std::vector<std::uint8_t>& rgb, std::size_t stride, int x, int y) {
    const std::size_t offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 3;
    return Rgb{rgb[offset], rgb[offset + 1], rgb[offset + 2]};
}

// 跑一次转换并把结果拷出来（内部缓冲下次转换就会被覆盖，用例里需要一份固定快照）。
std::vector<std::uint8_t> ConvertOrFail(RawFrameConverter& converter, const std::vector<std::uint8_t>& src) {
    EXPECT_TRUE(converter.Convert(src.data(), src.size()));
    const std::uint8_t* rgb = converter.RgbData();
    return std::vector<std::uint8_t>(rgb, rgb + converter.OutputBytes());
}

int AbsDiff(int lhs, int rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 格式表与帧尺寸
// ---------------------------------------------------------------------------

TEST(RawFrameConverterTests, FormatCatalogCoversEveryFormatAndRoundTrips) {
    // 显式列全枚举：将来加了格式却忘了进表，这条会红（表是"下拉框 / 解析 / 转换"的唯一事实源）。
    const RawPixelFormat kAllFormats[] = {
        RawPixelFormat::I420,  RawPixelFormat::NV12,        RawPixelFormat::NV21,  RawPixelFormat::YV12,
        RawPixelFormat::P010,  RawPixelFormat::YUV420P10LE, RawPixelFormat::YUY2,  RawPixelFormat::UYVY,
        RawPixelFormat::GRAY8, RawPixelFormat::RGB24,       RawPixelFormat::BGR24,
    };

    const std::vector<RawPixelFormatOption>& options = RawPixelFormatOptions();
    ASSERT_EQ(options.size(), std::size(kAllFormats));

    for (const RawPixelFormat format : kAllFormats) {
        int hits = 0;
        for (const RawPixelFormatOption& option : options) {
            if (option.format == format) {
                ++hits;
                RawPixelFormat parsed = RawPixelFormat::GRAY8;
                ASSERT_TRUE(ParseRawPixelFormatLabel(option.label, parsed)) << option.label;
                EXPECT_EQ(parsed, format) << option.label;
            }
        }
        EXPECT_EQ(hits, 1) << "格式在表里出现次数不是 1（缺失或重复）: " << static_cast<int>(format);
    }

    // 标签唯一：重复会让下拉框选到的永远是第一个，而默认值的 indexOf 也是第一个。
    for (std::size_t i = 0; i < options.size(); ++i) {
        for (std::size_t j = i + 1; j < options.size(); ++j) {
            EXPECT_STRNE(options[i].label, options[j].label);
        }
    }

    RawPixelFormat unused = RawPixelFormat::I420;
    EXPECT_FALSE(ParseRawPixelFormatLabel("YUV420P", unused)); // 不能模糊匹配 "YUV420P(I420)"
    EXPECT_FALSE(ParseRawPixelFormatLabel("", unused));
}

TEST(RawFrameConverterTests, FrameSizeRejectsDimensionsTheLayoutCannotHold) {
    std::string reason;

    // 4:2:0 要偶数宽高：奇数会让两个色度平面各差半个样本，整帧错位。
    EXPECT_EQ(RawFrameSizeBytes(17, 16, RawPixelFormat::I420, &reason), 0u);
    EXPECT_FALSE(reason.empty());
    reason.clear();
    EXPECT_EQ(RawFrameSizeBytes(16, 15, RawPixelFormat::NV12, &reason), 0u);
    EXPECT_FALSE(reason.empty());
    reason.clear();
    EXPECT_EQ(RawFrameSizeBytes(16, 15, RawPixelFormat::P010, &reason), 0u);
    EXPECT_FALSE(reason.empty());

    // 打包 4:2:2 只约束宽度，高度任意。
    reason.clear();
    EXPECT_EQ(RawFrameSizeBytes(17, 16, RawPixelFormat::YUY2, &reason), 0u);
    EXPECT_FALSE(reason.empty());
    EXPECT_GT(RawFrameSizeBytes(16, 17, RawPixelFormat::YUY2, nullptr), 0u);

    // RGB / 灰度任意尺寸。
    EXPECT_GT(RawFrameSizeBytes(17, 15, RawPixelFormat::RGB24, nullptr), 0u);
    EXPECT_GT(RawFrameSizeBytes(17, 15, RawPixelFormat::GRAY8, nullptr), 0u);

    // 非正尺寸一律拒绝（对话框允许最小值 1，1x1 对 4:2:0 就是非法的）。
    EXPECT_EQ(RawFrameSizeBytes(0, 16, RawPixelFormat::GRAY8, nullptr), 0u);
    EXPECT_EQ(RawFrameSizeBytes(16, -2, RawPixelFormat::GRAY8, nullptr), 0u);
}

TEST(RawFrameConverterTests, FrameSizeMatchesRealLayouts) {
    const std::size_t pixels = 16u * 16u;

    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::I420, nullptr), pixels * 3 / 2);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::NV12, nullptr), pixels * 3 / 2);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::YV12, nullptr), pixels * 3 / 2);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::P010, nullptr), pixels * 3);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::YUV420P10LE, nullptr), pixels * 3);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::YUY2, nullptr), pixels * 2);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::GRAY8, nullptr), pixels);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::RGB24, nullptr), pixels * 3);
    EXPECT_EQ(RawFrameSizeBytes(16, 16, RawPixelFormat::BGR24, nullptr), pixels * 3);

    // 转换器自己报的输入字节数必须与上面同一口径，否则"读完一帧"与"转一帧"会错位。
    RawFrameConverter converter(kW, kH, RawPixelFormat::NV12, false);
    ASSERT_TRUE(converter.Valid());
    EXPECT_EQ(converter.FrameBytes(), RawFrameSizeBytes(kW, kH, RawPixelFormat::NV12, nullptr));
    EXPECT_EQ(converter.OutputBytes(), static_cast<std::size_t>(kW) * kH * 3);
}

// ---------------------------------------------------------------------------
// 2. 四种 4:2:0 排布必须得到同一幅 RGB
// ---------------------------------------------------------------------------

TEST(RawFrameConverterTests, AllFourTwoZeroLayoutsOfSameContentAreIdentical) {
    const YuvPlanes planes = MakeGradientYuv();

    struct Case {
        RawPixelFormat format;
        std::vector<std::uint8_t> bytes;
    };
    const Case cases[] = {
        {RawPixelFormat::I420, LayoutI420(planes)},
        {RawPixelFormat::NV12, LayoutNv12(planes)},
        {RawPixelFormat::NV21, LayoutNv21(planes)},
        {RawPixelFormat::YV12, LayoutYv12(planes)},
    };

    std::vector<std::uint8_t> reference;
    for (const Case& item : cases) {
        RawFrameConverter converter(kW, kH, item.format, false);
        ASSERT_TRUE(converter.Valid());
        ASSERT_EQ(item.bytes.size(), converter.FrameBytes()) << "排布构造与帧字节数不一致";
        const std::vector<std::uint8_t> rgb = ConvertOrFail(converter, item.bytes);
        if (reference.empty()) {
            reference = rgb;
            continue;
        }
        // 容差 4：见 MakeGradientYuv 的说明 —— 半平面与平面的色度上采样策略不同，
        // 块边界会差到 2 个刻度；而平面顺序接反是几十的差。
        ASSERT_EQ(rgb.size(), reference.size());
        int worst = 0;
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            worst = std::max(worst, AbsDiff(static_cast<int>(rgb[i]), static_cast<int>(reference[i])));
        }
        EXPECT_LE(worst, 4) << "排布 " << static_cast<int>(item.format) << " 与 I420 偏差过大（平面顺序接反？）";
    }
}

// ---------------------------------------------------------------------------
// 3. 范围与矩阵
// ---------------------------------------------------------------------------

TEST(RawFrameConverterTests, LimitedRangeMapsBlackAndWhiteToFullRange) {
    RawFrameConverter converter(kW, kH, RawPixelFormat::I420, false);
    ASSERT_TRUE(converter.Valid());

    const std::vector<std::uint8_t> black = ConvertOrFail(converter, LayoutI420(MakeYuv(16, 128, 128)));
    const Rgb black_pixel = PixelAt(black, converter.Stride(), 0, 0);
    EXPECT_LE(AbsDiff(black_pixel.r, 0), 1);
    EXPECT_LE(AbsDiff(black_pixel.g, 0), 1);
    EXPECT_LE(AbsDiff(black_pixel.b, 0), 1);

    const std::vector<std::uint8_t> white = ConvertOrFail(converter, LayoutI420(MakeYuv(235, 128, 128)));
    const Rgb white_pixel = PixelAt(white, converter.Stride(), 8, 8);
    EXPECT_LE(AbsDiff(white_pixel.r, 255), 1);
    EXPECT_LE(AbsDiff(white_pixel.g, 255), 1);
    EXPECT_LE(AbsDiff(white_pixel.b, 255), 1);
}

TEST(RawFrameConverterTests, Bt709AndBt601DifferOnSaturatedChroma) {
    // 红色（BT.601 的 Y=81 U=90 V=240）：601 下 G 几乎为 0，709 下 G 明显抬起来。
    // 矩阵选错不会崩，只会偏色 —— 只能靠数值钉。
    const std::vector<std::uint8_t> frame = LayoutI420(MakeYuv(81, 90, 240));

    RawFrameConverter bt601(kW, kH, RawPixelFormat::I420, false);
    RawFrameConverter bt709(kW, kH, RawPixelFormat::I420, true);
    ASSERT_TRUE(bt601.Valid());
    ASSERT_TRUE(bt709.Valid());

    const Rgb p601 = PixelAt(ConvertOrFail(bt601, frame), bt601.Stride(), 4, 4);
    const Rgb p709 = PixelAt(ConvertOrFail(bt709, frame), bt709.Stride(), 4, 4);

    EXPECT_GE(p601.r, 250) << "601 下红色分量应接近饱和";
    EXPECT_LE(p601.g, 4);
    EXPECT_LE(p601.b, 4);

    EXPECT_GE(p709.r, 250);
    EXPECT_GE(p709.g, p601.g + 12) << "709 的绿色分量必须比 601 高（系数 0.213 vs 0.391）";
}

TEST(RawFrameConverterTests, GrayIsPassedThroughWithoutColorMatrix) {
    // 灰度是"全范围 Y，没有矩阵"：0 与 255 必须原样到 RGB 三通道。
    RawFrameConverter converter(8, 8, RawPixelFormat::GRAY8, false);
    ASSERT_TRUE(converter.Valid());

    std::vector<std::uint8_t> src(64, 0);
    const Rgb black = PixelAt(ConvertOrFail(converter, src), converter.Stride(), 0, 0);
    EXPECT_EQ(black.r, 0);
    EXPECT_EQ(black.g, 0);
    EXPECT_EQ(black.b, 0);

    std::fill(src.begin(), src.end(), static_cast<std::uint8_t>(255));
    const Rgb white = PixelAt(ConvertOrFail(converter, src), converter.Stride(), 7, 7);
    EXPECT_EQ(white.r, 255);
    EXPECT_EQ(white.g, 255);
    EXPECT_EQ(white.b, 255);
}

TEST(RawFrameConverterTests, Rgb24AndBgr24HonorChannelOrder) {
    // 同一串字节，(12,34,56) 在 RGB24 里是 RGB、在 BGR24 里必须读成 BGR。
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(4) * 4 * 3);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        bytes[i + 0] = 12;
        bytes[i + 1] = 34;
        bytes[i + 2] = 56;
    }

    RawFrameConverter rgb24(4, 4, RawPixelFormat::RGB24, false);
    RawFrameConverter bgr24(4, 4, RawPixelFormat::BGR24, false);
    ASSERT_TRUE(rgb24.Valid());
    ASSERT_TRUE(bgr24.Valid());

    const Rgb as_rgb = PixelAt(ConvertOrFail(rgb24, bytes), rgb24.Stride(), 1, 1);
    EXPECT_EQ(as_rgb.r, 12);
    EXPECT_EQ(as_rgb.g, 34);
    EXPECT_EQ(as_rgb.b, 56);

    const Rgb as_bgr = PixelAt(ConvertOrFail(bgr24, bytes), bgr24.Stride(), 1, 1);
    EXPECT_EQ(as_bgr.r, 56);
    EXPECT_EQ(as_bgr.g, 34);
    EXPECT_EQ(as_bgr.b, 12);
}

// ---------------------------------------------------------------------------
// 4. 10bit（两种位对齐）
// ---------------------------------------------------------------------------

TEST(RawFrameConverterTests, TenBitLayoutsMatchEightBitEquivalents) {
    // 10bit 的限制范围是 64..940，与 8bit 的 16..235 一一对应（×4）。等价内容的 RGB
    // 必须与 8bit 一致，否则就是位对齐取错（P010 在高位、平面 10bit 在低位）。
    RawFrameConverter ref(kW, kH, RawPixelFormat::I420, false);
    ASSERT_TRUE(ref.Valid());
    const std::vector<std::uint8_t> ref_rgb = ConvertOrFail(ref, LayoutI420(MakeYuv(81, 90, 240)));
    const Rgb expected = PixelAt(ref_rgb, ref.Stride(), 2, 2);

    struct Case {
        RawPixelFormat format;
        std::vector<std::uint8_t> bytes;
    };
    const Case cases[] = {
        {RawPixelFormat::P010, LayoutP010(81, 90, 240)},
        {RawPixelFormat::YUV420P10LE, LayoutYuv420P10(81, 90, 240)},
    };

    for (const Case& item : cases) {
        RawFrameConverter converter(kW, kH, item.format, false);
        ASSERT_TRUE(converter.Valid());
        ASSERT_EQ(item.bytes.size(), converter.FrameBytes()) << "10bit 排布与帧字节数不一致";
        const Rgb actual = PixelAt(ConvertOrFail(converter, item.bytes), converter.Stride(), 2, 2);
        EXPECT_LE(AbsDiff(actual.r, expected.r), 2) << static_cast<int>(item.format);
        EXPECT_LE(AbsDiff(actual.g, expected.g), 2) << static_cast<int>(item.format);
        EXPECT_LE(AbsDiff(actual.b, expected.b), 2) << static_cast<int>(item.format);
    }
}

// ---------------------------------------------------------------------------
// 5. 输入/输出的边界
// ---------------------------------------------------------------------------

TEST(RawFrameConverterTests, ConvertRejectsShortSourceAndNullSource) {
    RawFrameConverter converter(kW, kH, RawPixelFormat::I420, false);
    ASSERT_TRUE(converter.Valid());

    const std::vector<std::uint8_t> src = LayoutI420(MakeYuv(81, 90, 240));

    // 少一个字节就不该被当成完整一帧（否则色度平面整体前移，画面会斜着花）。
    EXPECT_FALSE(converter.Convert(src.data(), src.size() - 1));
    // 空指针一律拒绝（调用点都在界面线程，宁红不崩）。
    EXPECT_FALSE(converter.Convert(nullptr, src.size()));
    // 完整的输入必须成功：上面两条拒绝不能顺手把正常路径也挡住。
    EXPECT_TRUE(converter.Convert(src.data(), src.size()));
}

TEST(RawFrameConverterTests, StrideIsFourByteAlignedAndRowsDoNotBleed) {
    // 宽度 ×3 不是 4 的倍数时（17 像素 → 51 字节），步长必须补到 52：
    // QImage 用外部缓冲时要求行首 4 字节对齐，不补就整幅图斜一行。
    RawFrameConverter converter(17, 15, RawPixelFormat::RGB24, false);
    ASSERT_TRUE(converter.Valid());
    EXPECT_EQ(converter.Stride() % 4, 0u);
    EXPECT_GE(converter.Stride(), 17u * 3u);

    // 每行用不同的纯色填充，逐行核对最右一个像素 —— 行间串色（步长算错）会立刻显形。
    std::vector<std::uint8_t> src(17u * 15u * 3u, 0);
    for (int row = 0; row < 15; ++row) {
        for (int col = 0; col < 17; ++col) {
            const std::size_t offset = (static_cast<std::size_t>(row) * 17 + col) * 3;
            src[offset + 0] = static_cast<std::uint8_t>(row * 16);
            src[offset + 1] = static_cast<std::uint8_t>(col * 12);
            src[offset + 2] = 7;
        }
    }

    ASSERT_TRUE(converter.Convert(src.data(), src.size()));
    const std::vector<std::uint8_t> rgb(converter.RgbData(), converter.RgbData() + converter.OutputBytes());
    for (int row = 0; row < 15; ++row) {
        const Rgb right = PixelAt(rgb, converter.Stride(), 16, row);
        EXPECT_EQ(right.r, row * 16) << "row " << row;
        EXPECT_EQ(right.g, 16 * 12) << "row " << row;
        const Rgb left = PixelAt(rgb, converter.Stride(), 0, row);
        EXPECT_EQ(left.r, row * 16) << "row " << row;
        EXPECT_EQ(left.g, 0) << "row " << row;
    }
}

TEST(RawFrameConverterTests, SmallFramesDoNotCorruptMemory) {
    // 回归：swscale 在窄帧上会多写一整行（8x8 多 24 字节、2x2 多 42 字节），
    // 以前把内部像素缓冲直接交给调用方的 QImage，尾部没有余量 → 堆越界，
    // 症状是"转换返回成功，销毁时随机崩"。缓冲归转换器并留足尾部余量之后，
    // 这些尺寸必须能安全走完（用例本身就是判据：崩了整进程全红）。
    const int sizes[][2] = {{2, 2}, {4, 4}, {8, 8}, {8, 16}, {16, 8}};
    for (const auto& wh : sizes) {
        const int w = wh[0];
        const int h = wh[1];
        RawFrameConverter converter(w, h, RawPixelFormat::I420, false);
        ASSERT_TRUE(converter.Valid()) << w << "x" << h;

        YuvPlanes planes;
        planes.y.assign(static_cast<std::size_t>(w) * h, 126);
        planes.u.assign(static_cast<std::size_t>(w / 2) * (h / 2), 128);
        planes.v.assign(planes.u.size(), 128);
        const std::vector<std::uint8_t> src = LayoutI420(planes);

        ASSERT_EQ(src.size(), converter.FrameBytes()) << w << "x" << h;
        ASSERT_TRUE(converter.Convert(src.data(), src.size())) << w << "x" << h;

        // 中灰：既不能是 0（没写）也不能是 255（写花了）。
        const std::vector<std::uint8_t> rgb(converter.RgbData(), converter.RgbData() + converter.OutputBytes());
        const Rgb pixel = PixelAt(rgb, converter.Stride(), w / 2, h / 2);
        EXPECT_GE(pixel.r, 118) << w << "x" << h;
        EXPECT_LE(pixel.r, 138) << w << "x" << h;
    }
}
