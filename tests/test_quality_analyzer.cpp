#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "core/analyzer/Quality_analyzer.h"

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
        frame->format = AV_PIX_FMT_GRAY8;
        frame_->width = width;
        frame_->height = height;
        frame_->pts = 123;
        if (av_frame_get_buffer(frame_, 32) < 0) {
            av_frame_free(&frame_);
            return;
        }
        if (av_frame_make_writeable(frame_) < 0) {
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
    FrameGuard distorted(8, 8, 100);
    ASSERT_NE(reference.get(), nullptr);
    ASSERT_NE(distorted.get(), nullptr);

    const auto metrics = videoeye::analyzer::QualityAnalyzer::CompareFrames(
        reference.get(), distorted.get());

    ASSERT_TRUE(metrics.valid) << metrics.error_message;
    EXPECT_NEAR(metrics.mse, 100.0, 1e-9);
    EXPECT_NEAR(metrics.psnr_nb, 28.1308036087, 1e-6);
    EXPECT_LT(metrics.ssim, 0.0);
}

TEST(QualityAnalyzerTest, RejectsSizeMismatch) {
    FrameGuard reference(8, 8, 100);
    FrameGuard distorted(8, 8, 100);
    ASSERT_NE(reference.get(), nullptr);
    ASSERT_NE(distorted.get(), nullptr);

    const auto metrics = videoeye::analyzer::QualityAnalyzer::CompareFrames(
        reference.get(), distorted.get());

    EXPECT_FALSE(metrics.valid);
    EXPECT_EQ(metrics.error_message, "Size mismatch");
}

} // namespace