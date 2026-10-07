// OpenController 封面图判定的边界回归（评审 P0-1）
//
// 旧实现 HandleCoverArt 里先取流再判索引：
//     AVStream* vs = fmt->streams[video_index];
//     if (video_index < 0 || !vs || ...) return false;
// 音频-only 文件里 av_find_best_stream(AVMEDIA_TYPE_VIDEO) 返回 -1，于是
// streams[-1] 是**越界读** —— 未定义行为，可能直接崩溃。
//
// 修复把判定抽成 IsCoverArtStream()，边界检查前置。这里用内存合成的
// AVFormatContext（avformat_new_stream 造流）覆盖四种输入，不需要任何真实媒体样本：
//   * 音频-only（video_index == -1）—— 崩溃那条路径
//   * 空上下文 / 越界索引 —— 同样不能越界读
//   * 普通视频轨（无 ATTACHED_PIC）—— 不是封面
//   * 带封面图（AV_DISPOSITION_ATTACHED_PIC）—— 命中

#include <gtest/gtest.h>

#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

#include "core/player/OpenController.h"

namespace {

using videoeye::player::IsCoverArtStream;

struct FormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_free_context(ctx);
    }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

FormatContextPtr MakeContext() {
    return FormatContextPtr(avformat_alloc_context());
}

}  // namespace

// 空上下文：任何索引都不命中，且绝不能解引用空指针。
TEST(OpenControllerCoverArt, NullContextIsNotCoverArt) {
    EXPECT_FALSE(IsCoverArtStream(nullptr, -1));
    EXPECT_FALSE(IsCoverArtStream(nullptr, 0));
}

// 音频-only：1 条音频流、无视频流 —— av_find_best_stream(VIDEO) 会给出 -1。
// 这就是旧实现 streams[-1] 越界的现场；ASan 构建下它会在这里红。
TEST(OpenControllerCoverArt, AudioOnlyNegativeVideoIndexIsNotCoverArt) {
    auto ctx = MakeContext();
    ASSERT_NE(ctx, nullptr);
    AVStream* audio = avformat_new_stream(ctx.get(), nullptr);
    ASSERT_NE(audio, nullptr);
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    ASSERT_EQ(ctx->nb_streams, 1u);

    EXPECT_FALSE(IsCoverArtStream(ctx.get(), -1));
}

// 越界索引：nb_streams 之内没有该流，同样必须先判边界再取流。
TEST(OpenControllerCoverArt, OutOfRangeIndexIsNotCoverArt) {
    auto ctx = MakeContext();
    ASSERT_NE(ctx, nullptr);
    ASSERT_NE(avformat_new_stream(ctx.get(), nullptr), nullptr);  // 只有 index 0
    ASSERT_EQ(ctx->nb_streams, 1u);

    EXPECT_FALSE(IsCoverArtStream(ctx.get(), 1));
    EXPECT_FALSE(IsCoverArtStream(ctx.get(), 99));
}

// 普通视频轨（无 ATTACHED_PIC）不是封面图。
TEST(OpenControllerCoverArt, NormalVideoStreamIsNotCoverArt) {
    auto ctx = MakeContext();
    ASSERT_NE(ctx, nullptr);
    AVStream* video = avformat_new_stream(ctx.get(), nullptr);
    ASSERT_NE(video, nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->disposition = 0;

    EXPECT_FALSE(IsCoverArtStream(ctx.get(), 0));
}

// 带封面图（AV_DISPOSITION_ATTACHED_PIC）的视频轨命中。
TEST(OpenControllerCoverArt, AttachedPicStreamIsCoverArt) {
    auto ctx = MakeContext();
    ASSERT_NE(ctx, nullptr);
    AVStream* video = avformat_new_stream(ctx.get(), nullptr);
    ASSERT_NE(video, nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->disposition = AV_DISPOSITION_ATTACHED_PIC;

    EXPECT_TRUE(IsCoverArtStream(ctx.get(), 0));
}
