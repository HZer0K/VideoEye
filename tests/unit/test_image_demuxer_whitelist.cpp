// 图片白名单的"可解码性"守护 + 静图/动图判定 + 渲染预算边界。
//
// 为什么要这组用例：core/domain/model/ImageDemuxer.h 的白名单靠一条人工约定
// ——"只登记既有解复用器又有解码器的格式"。约定没有守护时，换一版 FFmpeg
// （比如裁掉 jxl / exr / qoi 解码器）的后果是：图片仍被判成"图片输入"，
// QC 豁免照旧生效，但画面永远出不来（黑屏），而且没有任何测试会红。
// 这里用 av_find_input_format + avcodec_find_decoder 把每个格式逐个钉住。
#include "core/domain/model/ImageBudget.h"
#include "core/domain/model/ImageDemuxer.h"

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

// 白名单 -> 该格式的代表性解码器。新增白名单条目时必须在这里补一行，
// 否则 WhitelistAndExpectationTableStayInSync 会红（TestImage… 的守护前提）。
struct DemuxerCodec {
    std::string_view demuxer;
    AVCodecID codec;
};

constexpr std::array<DemuxerCodec, 16> kExpectedCodecs = {{
    {"gif", AV_CODEC_ID_GIF},
    {"apng", AV_CODEC_ID_APNG},
    // image2 是"按扩展名选解码器"的解复用器，本身不绑定 codec；.jpg 在 FFmpeg 8.x
    // 首选 image2 + mjpeg，所以拿 mjpeg 当代表。
    {"image2", AV_CODEC_ID_MJPEG},
    {"webp_anim", AV_CODEC_ID_WEBP},
    {"png_pipe", AV_CODEC_ID_PNG},
    {"jpeg_pipe", AV_CODEC_ID_MJPEG},
    {"bmp_pipe", AV_CODEC_ID_BMP},
    {"webp_pipe", AV_CODEC_ID_WEBP},
    {"tiff_pipe", AV_CODEC_ID_TIFF},
    {"j2k_pipe", AV_CODEC_ID_JPEG2000},
    {"jpegxl_pipe", AV_CODEC_ID_JPEGXL},
    {"qoi_pipe", AV_CODEC_ID_QOI},
    {"exr_pipe", AV_CODEC_ID_EXR},
    {"ppm_pipe", AV_CODEC_ID_PPM},
    {"pgm_pipe", AV_CODEC_ID_PGM},
    {"pbm_pipe", AV_CODEC_ID_PBM},
}};

// 白名单里"本构建没有这个解复用器"的前瞻登记。只允许这一种例外：
// 多登记一个不存在的名字没有副作用（判定永不命中），而少登记一个真实存在的
// 名字会让画面播完被清掉。把例外显式记账，双向漂移都会判红。
constexpr std::array<std::string_view, 1> kAbsentByDesign = {"webp_anim"};

bool Contains(const std::array<DemuxerCodec, 16>& table, std::string_view name) {
    for (const auto& entry : table) {
        if (entry.demuxer == name) {
            return true;
        }
    }
    return false;
}

bool IsAbsentByDesign(std::string_view name) {
    for (const std::string_view candidate : kAbsentByDesign) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

} // namespace

// 本构建的白名单格式必须"解复用器 + 解码器"齐备；本构建缺失的只允许是
// kAbsentByDesign 里显式记账的那几个。两个方向都要管：
//   * 白名单新增了一个本构建不存在的名字 → 红（不许悄悄加）；
//   * 本构建突然提供了之前缺失的解复用器 → 红（提示把台账销掉、把格式加进动图名单）。
TEST(ImageDemuxerWhitelistTests, EveryWhitelistedFormatIsDemuxableAndDecodable) {
    for (const auto& entry : kExpectedCodecs) {
        const std::string name(entry.demuxer);
        const AVInputFormat* demuxer = av_find_input_format(name.c_str());
        if (demuxer == nullptr) {
            EXPECT_TRUE(IsAbsentByDesign(entry.demuxer))
                << "白名单登记了本构建不存在的解复用器（若非有意前瞻登记，请从白名单移除）: " << name;
            continue;
        }
        EXPECT_FALSE(IsAbsentByDesign(entry.demuxer))
            << name << " 在本构建里已经可用，请从 kAbsentByDesign 移除（并确认它进了动图名单）";
        EXPECT_NE(nullptr, avcodec_find_decoder(entry.codec)) << "缺少解码器: " << name;
    }
}

// 动图 WebP 一旦可解复用，白名单与动图名单必须同时认它 —— 否则这类流会被当成
// 普通视频，播到 EOF 走"播完清屏"的收尾路径，画面一闪就没（JPEG 曾经就是这样）。
TEST(ImageDemuxerWhitelistTests, AnimatedWebpDemuxerStaysClassifiedAsImage) {
    EXPECT_TRUE(videoeye::model::IsImageDemuxer("webp_anim"));
    EXPECT_TRUE(videoeye::model::IsAnimatedImageDemuxer("webp_anim"));
}

// 白名单与上面的映射表必须一一对应：只加白名单不加映射 = 该格式失去守护。
TEST(ImageDemuxerWhitelistTests, WhitelistAndExpectationTableStayInSync) {
    ASSERT_EQ(videoeye::model::kImageDemuxers.size(), kExpectedCodecs.size());
    for (const std::string_view name : videoeye::model::kImageDemuxers) {
        EXPECT_TRUE(Contains(kExpectedCodecs, name)) << "白名单新增格式未在测试里登记解码器: " << name;
    }
}

// 白名单内不允许重复：重复条目会让"新增格式只改一处"的约定失效。
TEST(ImageDemuxerWhitelistTests, WhitelistHasNoDuplicateEntries) {
    for (std::size_t i = 0; i < videoeye::model::kImageDemuxers.size(); ++i) {
        for (std::size_t j = i + 1; j < videoeye::model::kImageDemuxers.size(); ++j) {
            EXPECT_NE(videoeye::model::kImageDemuxers[i], videoeye::model::kImageDemuxers[j])
                << "白名单重复: " << videoeye::model::kImageDemuxers[i];
        }
    }
}

TEST(ImageDemuxerWhitelistTests, IsImageDemuxerMatchesWhitelistExactly) {
    for (const std::string_view name : videoeye::model::kImageDemuxers) {
        EXPECT_TRUE(videoeye::model::IsImageDemuxer(name)) << name;
    }
    // 反例：普通容器不能被当成图片（否则 QC 会豁免掉时长/音轨检查）。
    EXPECT_FALSE(videoeye::model::IsImageDemuxer("mov,mp4,m4a,3gp,3g2,mj2"));
    EXPECT_FALSE(videoeye::model::IsImageDemuxer("matroska,webm"));
    EXPECT_FALSE(videoeye::model::IsImageDemuxer("wav"));
    EXPECT_FALSE(videoeye::model::IsImageDemuxer(""));
}

// 动图必须是图片的子集：动图同样需要 QC 的图片豁免，但帧率要保留。
TEST(ImageDemuxerWhitelistTests, AnimatedDemuxersAreASubsetOfImages) {
    for (const std::string_view name : videoeye::model::kAnimatedImageDemuxers) {
        EXPECT_TRUE(videoeye::model::IsImageDemuxer(name)) << "动图未登记为图片: " << name;
    }
    EXPECT_TRUE(videoeye::model::IsAnimatedImageDemuxer("gif"));
    EXPECT_TRUE(videoeye::model::IsAnimatedImageDemuxer("apng"));
    // 静态图不能被动图判定吃掉：image2 / png_pipe 的帧率是占位值（实测 25/1），
    // 若被当成动图就会在首页写成"静图 @ 25 fps"。
    EXPECT_FALSE(videoeye::model::IsAnimatedImageDemuxer("image2"));
    EXPECT_FALSE(videoeye::model::IsAnimatedImageDemuxer("png_pipe"));
    EXPECT_FALSE(videoeye::model::IsAnimatedImageDemuxer("jpeg_pipe"));
}

// 静图不报帧率（avg_frame_rate 是解复用器填的 25 占位值），动图保留真帧率，
// 普通视频行为不变。这一层是 StreamInfoExtractor 唯一可测的形态：
// 那里的输入是真实文件，纯函数提出来后才有判别力。
TEST(ImageDemuxerWhitelistTests, StillImageDoesNotReportPlaceholderFrameRate) {
    EXPECT_FALSE(videoeye::model::ShouldReportFrameRate(true, "image2"));
    EXPECT_FALSE(videoeye::model::ShouldReportFrameRate(true, "png_pipe"));
    EXPECT_FALSE(videoeye::model::ShouldReportFrameRate(true, "jpeg_pipe"));
    // AVIF 走 mov 复用器：靠扩展名判成图片，同样不能报占位帧率。
    EXPECT_FALSE(videoeye::model::ShouldReportFrameRate(true, "mov,mp4,m4a,3gp,3g2,mj2"));
    // 动图帧率是按帧延时算出来的真值，必须保留。
    EXPECT_TRUE(videoeye::model::ShouldReportFrameRate(true, "gif"));
    EXPECT_TRUE(videoeye::model::ShouldReportFrameRate(true, "apng"));
    // 非图片输入一律保留原行为（不能因为这次改动影响普通视频）。
    EXPECT_TRUE(videoeye::model::ShouldReportFrameRate(false, "mov,mp4,m4a,3gp,3g2,mj2"));
    EXPECT_TRUE(videoeye::model::ShouldReportFrameRate(false, "matroska,webm"));
    EXPECT_TRUE(videoeye::model::ShouldReportFrameRate(false, ""));
}
TEST(ImageDemuxerWhitelistTests, StillImageExtensionIsCaseInsensitiveAndDotTolerant) {
    EXPECT_TRUE(videoeye::model::IsStillImageExtension("avif"));
    EXPECT_TRUE(videoeye::model::IsStillImageExtension(".avif"));
    EXPECT_TRUE(videoeye::model::IsStillImageExtension(".AVIF"));
    EXPECT_TRUE(videoeye::model::IsStillImageExtension("AVIF"));
    EXPECT_FALSE(videoeye::model::IsStillImageExtension("mp4"));
    EXPECT_FALSE(videoeye::model::IsStillImageExtension("avifs"));
    EXPECT_FALSE(videoeye::model::IsStillImageExtension(""));
}

// AVIF 落在 mov 家族：解复用器名看不出"这是张图"，只能靠扩展名 ——
// 这条钉住"两条判定任一命中即为图片输入"的语义，避免调用方只传扩展名时漏判。
TEST(ImageDemuxerWhitelistTests, ImageInputAcceptsEitherSignal) {
    EXPECT_TRUE(videoeye::model::IsImageInput("mov,mp4,m4a,3gp,3g2,mj2", "avif"));
    EXPECT_TRUE(videoeye::model::IsImageInput("png_pipe", ""));
    EXPECT_FALSE(videoeye::model::IsImageInput("mov,mp4,m4a,3gp,3g2,mj2", "mp4"));
    EXPECT_FALSE(videoeye::model::IsImageInput("wav", "wav"));
}

// ---- 渲染预算 ----

TEST(ImageBudgetTests, EightKVideoFitsButOversizedStillDoesNot) {
    // 8K 视频 7680x4320 = 33.2 Mi 像素，必须放行（否则会误伤正常视频播放）。
    EXPECT_FALSE(videoeye::model::ExceedsImagePixelBudget(7680, 4320));
    // 64 Mi 像素是上限本身，取"大于"语义，等值放行。
    EXPECT_FALSE(videoeye::model::ExceedsImagePixelBudget(8192, 8192));
    EXPECT_TRUE(videoeye::model::ExceedsImagePixelBudget(8193, 8192));
    // 典型超大静图：30000x30000 = 9 亿像素，BGRA 要 3.6 GiB。
    EXPECT_TRUE(videoeye::model::ExceedsImagePixelBudget(30000, 30000));
}

TEST(ImageBudgetTests, UnknownSizeIsNotTreatedAsOversized) {
    // 尺寸未知（动图 WebP 的空壳流会给出 0x0）要走"让解码器报错"的路径，
    // 不能被说成"分辨率过大"。
    EXPECT_FALSE(videoeye::model::ExceedsImagePixelBudget(0, 0));
    EXPECT_FALSE(videoeye::model::ExceedsImagePixelBudget(-1, 1080));
    EXPECT_FALSE(videoeye::model::ExceedsImagePixelBudget(1920, -1));
}

TEST(ImageBudgetTests, ByteCountAndDescriptionAreAccurate) {
    EXPECT_EQ(videoeye::model::RenderFrameBytes(1920, 1080), 1920ll * 1080 * 4);
    EXPECT_EQ(videoeye::model::RenderFrameBytes(0, 1080), 0);
    const std::string text = videoeye::model::FormatPixelBudget(1920, 1080);
    EXPECT_NE(std::string::npos, text.find("1920x1080"));
    EXPECT_NE(std::string::npos, text.find("MiB"));
    EXPECT_TRUE(videoeye::model::FormatPixelBudget(0, 0).empty());
}
