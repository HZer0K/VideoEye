#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "core/analyzer/QualityAnalyzer.h"

extern "C" {
#include<libavutil/frame.h>
#include<libavutil/pixfmt.h>
}

namespace {

class FrameGuard {
public:
    FrameGuard(int width, int height, uint8_t value) 
        : frame_(av_frame_alloc()){
        if (!frame_) return;
        frame_->format = AV_PIX_FMT_GRAY8;
        frame_->width = width;
        frame_->height = height;
        frame_->pts = 123;
        if (av_frame_get_buffer(frame_, 32) < 0) {
            av_frame_free(&frame_);
            return;
        }
        // FFmpeg 7 起改名: av_frame_make_writeable -> av_frame_make_writable
        if (av_frame_make_writable(frame_) < 0) {
            av_frame_free(&frame_);
            return;
        }
        for (int y = 0; y < height; y++) {
            uint8_t* dst = frame_->data[0] + y * frame_->linesize[0];
            for (int x = 0; x < width; x++) {
                dst[x] = value;
            }
        }
    }

    ~FrameGuard() {
        av_frame_free(&frame_);
    }

    // 逐像素写入，用来构造非恒定图案（走 linesize，不能假设它等于 width）
    void SetPixel(int x, int y, uint8_t value) {
        if (!frame_ || x < 0 || y < 0 || x >= frame_->width || y >= frame_->height) {
            return;
        }
        frame_->data[0][y * frame_->linesize[0] + x] = value;
    }

    AVFrame* get() const {
        return frame_;
    }

private:
    AVFrame* frame_;
};

TEST(QualityAnalyzerTest, IdenticalFramesHaveInfinitePsnrAndPerfectSsim) {
    FrameGuard reference(8, 8, 100);
    FrameGuard distorted(8, 8, 100);
    ASSERT_NE(reference.get(), nullptr);
    ASSERT_NE(distorted.get(), nullptr);

    const auto metrics = videoeye::analyzer::QualityAnalyzer::CompareFrames(
        reference.get(), distorted.get(), 7, 1.25);

    ASSERT_TRUE(metrics.valid) << metrics.error_message;
    EXPECT_EQ(metrics.frame_index, 7);
    EXPECT_EQ(metrics.width, 8);
    EXPECT_EQ(metrics.height, 8);
    EXPECT_DOUBLE_EQ(metrics.mse, 0.0);
    EXPECT_TRUE(std::isinf(metrics.psnr_nb));
    EXPECT_NEAR(metrics.ssim, 1.0, 1e-9);
}

TEST(QualityAnalyzerTest, ConstantPixelDeltaProducesExpectedPsnr) {
    FrameGuard reference(8, 8, 100);
    FrameGuard distorted(8, 8, 110);
    ASSERT_NE(reference.get(), nullptr);
    ASSERT_NE(distorted.get(), nullptr);

    const auto metrics = videoeye::analyzer::QualityAnalyzer::CompareFrames(
        reference.get(), distorted.get());

    ASSERT_TRUE(metrics.valid) << metrics.error_message;
    // 每个像素差 10 -> MSE = 100 -> PSNR = 10*log10(255^2/100) = 28.1308...
    EXPECT_NEAR(metrics.mse, 100.0, 1e-9);
    EXPECT_NEAR(metrics.psnr_nb, 28.1308036087, 1e-6);
    // 两张都是常数图: 方差/协方差均为 0，结构项退化成 1，只剩亮度项起作用。
    //   (2*100*110 + c1) / (100^2 + 110^2 + c1) = 22006.5025 / 22106.5025
    // 钉死这个值，公式再被改动时会立刻挂。
    EXPECT_NEAR(metrics.ssim, 22006.5025 / 22106.5025, 1e-12);
    EXPECT_LT(metrics.ssim, 1.0);
    EXPECT_GT(metrics.ssim, 0.0);
}

TEST(QualityAnalyzerTest, InvertedFrameYieldsNegativeSsim) {
    FrameGuard reference(8, 8, 0);
    FrameGuard distorted(8, 8, 0);
    ASSERT_NE(reference.get(), nullptr);
    ASSERT_NE(distorted.get(), nullptr);

    // 棋盘图案 vs 它的反相 -> 完全负相关，这是 [-1, 1] 里负半轴的极端情况
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const uint8_t value = ((x + y) % 2 == 0) ? 255 : 0;
            reference.SetPixel(x, y, value);
            distorted.SetPixel(x, y, static_cast<uint8_t>(255 - value));
        }
    }

    const auto metrics = videoeye::analyzer::QualityAnalyzer::CompareFrames(
        reference.get(), distorted.get());

    ASSERT_TRUE(metrics.valid) << metrics.error_message;
    // mu 均为 127.5, var 均为 16256.25, cov = -16256.25:
    //   分子第二项 2*cov + c2 < 0 -> 整体为负
    EXPECT_LT(metrics.ssim, 0.0);
    EXPECT_GE(metrics.ssim, -1.0);
    EXPECT_NEAR(metrics.ssim, -32453.9775L / 32571.0225L, 1e-9);
}

TEST(QualityAnalyzerTest, RejectsSizeMismatch) {
    FrameGuard reference(8, 8, 100);
    FrameGuard distorted(4, 4, 100);
    ASSERT_NE(reference.get(), nullptr);
    ASSERT_NE(distorted.get(), nullptr);

    const auto metrics = videoeye::analyzer::QualityAnalyzer::CompareFrames(
        reference.get(), distorted.get());

    EXPECT_FALSE(metrics.valid);
    EXPECT_EQ(metrics.error_message, "Gray planes have different dimensions");
}

} // namespace