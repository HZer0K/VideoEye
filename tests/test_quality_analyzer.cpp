#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/analysis/quality/QualityAnalyzer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

class FrameGuard {
public:
    FrameGuard(int width, int height, uint8_t value) : frame_(av_frame_alloc()) {
        if (!frame_)
            return;
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

    const auto metrics = videoeye::QualityAnalyzer::CompareFrames(reference.get(), distorted.get(), 7, 1.25);

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

    const auto metrics = videoeye::QualityAnalyzer::CompareFrames(reference.get(), distorted.get());

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

    const auto metrics = videoeye::QualityAnalyzer::CompareFrames(reference.get(), distorted.get());

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

    const auto metrics = videoeye::QualityAnalyzer::CompareFrames(reference.get(), distorted.get());

    EXPECT_FALSE(metrics.valid);
    EXPECT_EQ(metrics.error_message, "Gray planes have different dimensions");
}

// VMAF 是可选依赖（libvmaf），默认构建不接入：
// CompareSamples 的结果里 vmaf 必须是 nullopt 而非 NaN —— NaN 会静默流进
// 曲线与 JSON 导出（JSON 不允许 NaN 字面量），"未启用"必须与"算失败"可区分。
TEST(QualityAnalyzerTest, CompareSamplesLeavesVmafEmptyWhenUnsupported) {
    videoeye::model::FrameSample reference;
    reference.frame_index = 0;
    reference.timestamp_seconds = 0.0;
    reference.width = 8;
    reference.height = 8;
    reference.gray.assign(64, 100);

    videoeye::model::FrameSample distorted;
    distorted.frame_index = 1;
    distorted.timestamp_seconds = 0.04;
    distorted.width = 8;
    distorted.height = 8;
    distorted.gray.assign(64, 110);

    const auto metrics = videoeye::QualityAnalyzer::CompareSamples(reference, distorted);

    ASSERT_TRUE(metrics.valid) << metrics.error_message;
    EXPECT_NEAR(metrics.mse, 100.0, 1e-9);
    EXPECT_NEAR(metrics.psnr_nb, 28.1308036087, 1e-6);
    EXPECT_FALSE(metrics.vmaf.has_value());
    EXPECT_FALSE(videoeye::QualityAnalyzer::VmafSupported());
}

// yuv420p 帧: Y 平面恒定 luma，色度填 128（中性），可声明亮度范围与传输特性。
// 直接喂 QualityAnalyzer::BuildSample，验证真实 AVFrame 路径上的范围保持 / HDR 标记。
class YuvFrameGuard {
public:
    YuvFrameGuard(int width, int height, uint8_t luma, AVColorRange range,
                  AVColorTransferCharacteristic trc = AVCOL_TRC_BT709)
        : frame_(av_frame_alloc()) {
        if (!frame_)
            return;
        frame_->format = AV_PIX_FMT_YUV420P;
        frame_->width = width;
        frame_->height = height;
        frame_->color_range = range;
        frame_->color_trc = trc;
        if (av_frame_get_buffer(frame_, 32) < 0) {
            av_frame_free(&frame_);
            return;
        }
        if (av_frame_make_writable(frame_) < 0) {
            av_frame_free(&frame_);
            return;
        }
        const int cw = (width + 1) / 2;
        const int ch = (height + 1) / 2;
        for (int y = 0; y < height; ++y)
            std::memset(frame_->data[0] + y * frame_->linesize[0], luma, static_cast<size_t>(width));
        for (int y = 0; y < ch; ++y) {
            std::memset(frame_->data[1] + y * frame_->linesize[1], 128, static_cast<size_t>(cw));
            std::memset(frame_->data[2] + y * frame_->linesize[2], 128, static_cast<size_t>(cw));
        }
    }

    ~YuvFrameGuard() {
        av_frame_free(&frame_);
    }

    AVFrame* get() const {
        return frame_;
    }

private:
    AVFrame* frame_ = nullptr;
};

// limited range（MPEG）的合法峰值白 235 必须在采样后仍是 235。
// 若 sws 把 limited 展开成 full，235 会变成 255，曝光判定就会把白底/标题卡
// 误报成削波 —— 这条用例就是那个口径错位的回归。
TEST(QualityAnalyzerTest, BuildSampleKeepsLimitedRangeLuma) {
    YuvFrameGuard frame(320, 180, 235, AVCOL_RANGE_MPEG);
    ASSERT_NE(frame.get(), nullptr);

    videoeye::model::FrameSample sample;
    std::string error;
    ASSERT_TRUE(videoeye::QualityAnalyzer::BuildSample(frame.get(), 64, 64, false, sample, error)) << error;

    EXPECT_TRUE(sample.limited_range);
    EXPECT_EQ(sample.source_width, 320);
    EXPECT_EQ(sample.source_height, 180);
    ASSERT_TRUE(sample.valid());
    for (uint8_t v : sample.gray)
        EXPECT_NEAR(v, 235, 1);
}

// full range（JPEG）的 238 不能按 limited 口径压到 235 附近
TEST(QualityAnalyzerTest, BuildSampleKeepsFullRangeLuma) {
    YuvFrameGuard frame(320, 180, 238, AVCOL_RANGE_JPEG);
    ASSERT_NE(frame.get(), nullptr);

    videoeye::model::FrameSample sample;
    std::string error;
    ASSERT_TRUE(videoeye::QualityAnalyzer::BuildSample(frame.get(), 64, 64, false, sample, error)) << error;

    EXPECT_FALSE(sample.limited_range);
    for (uint8_t v : sample.gray)
        EXPECT_NEAR(v, 238, 1);
}

// 块效应探针: 4K 帧要生成中心 + 四角共 5 块、每块 <= 256x256 的 1:1 裁剪，
// 既保住 8px 块周期，又不让单帧快照无限增长。
TEST(QualityAnalyzerTest, BuildSampleBuildsMultiRegionBlockProbes) {
    YuvFrameGuard frame(3840, 2160, 128, AVCOL_RANGE_MPEG);
    ASSERT_NE(frame.get(), nullptr);

    videoeye::model::FrameSample sample;
    std::string error;
    ASSERT_TRUE(videoeye::QualityAnalyzer::BuildSample(frame.get(), 64, 64, false, sample, error, true)) << error;

    ASSERT_TRUE(sample.has_block_probes());
    EXPECT_EQ(sample.block_probes.size(), 5u);
    for (const auto& probe : sample.block_probes) {
        EXPECT_TRUE(probe.valid());
        EXPECT_LE(probe.width, 256);
        EXPECT_LE(probe.height, 256);
        // 块边界相位必须对齐到 8px 编码块
        EXPECT_EQ(probe.x % 8, 0);
        EXPECT_EQ(probe.y % 8, 0);
    }
}

// PQ 帧要被标记为 hdr 并做近似色调映射（tone_mapped），否则曝光/色偏会被永久关闭
TEST(QualityAnalyzerTest, BuildSampleToneMapsPqFrame) {
    YuvFrameGuard frame(320, 180, 180, AVCOL_RANGE_MPEG, AVCOL_TRC_SMPTE2084);
    ASSERT_NE(frame.get(), nullptr);

    videoeye::model::FrameSample sample;
    std::string error;
    ASSERT_TRUE(videoeye::QualityAnalyzer::BuildSample(frame.get(), 64, 64, false, sample, error)) << error;

    EXPECT_TRUE(sample.hdr);
    EXPECT_TRUE(sample.tone_mapped);
    // 色调映射输出是 full range（0..255），limited_range 必须同步改掉，
    // 否则曝光阈值会继续按 236/252 卡一条 0..255 的信号
    EXPECT_FALSE(sample.limited_range);
}

} // namespace