#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/analysis/quality/ReferenceQualityAnalyzer.h"

namespace {

using videoeye::ReferenceQualityAnalyzer;
using videoeye::model::FrameSample;
using videoeye::model::ReferenceQualityOptions;

FrameSample MakeSample(double timestamp, int frame_index, uint8_t value, int width = 16, int height = 16) {
    FrameSample sample;
    sample.frame_index = frame_index;
    sample.timestamp_seconds = timestamp;
    sample.width = width;
    sample.height = height;
    sample.source_width = width;
    sample.source_height = height;
    sample.gray.assign(static_cast<size_t>(width) * static_cast<size_t>(height), value);
    return sample;
}

// 非恒定图案: 让 SSIM 有真实的结构项，而不是退化到常数平面的特例
FrameSample MakeGradientSample(double timestamp, int frame_index, int shift = 0, int width = 16, int height = 16) {
    FrameSample sample = MakeSample(timestamp, frame_index, 0, width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            sample.gray[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                static_cast<uint8_t>((x * 7 + y * 3 + shift) & 0xFF);
        }
    }
    return sample;
}

// 常量亮度差 delta 的理论 PSNR: 10*log10(255^2 / delta^2)
double ExpectedPsnr(double delta) {
    return 20.0 * std::log10(255.0 / delta);
}

} // namespace

TEST(ReferenceQualityAnalyzerTest, IdenticalSequencesAreLossless) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    for (int i = 0; i < 3; ++i) {
        const double t = i * 0.04;
        reference.push_back(MakeGradientSample(t, i));
        distorted.push_back(MakeGradientSample(t, i));
    }

    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, ReferenceQualityOptions());

    ASSERT_TRUE(result.valid) << result.error_message;
    ASSERT_EQ(result.frames.size(), 3u);
    EXPECT_EQ(result.summary.compared_frames, 3);
    EXPECT_EQ(result.summary.failed_frames, 0);
    EXPECT_EQ(result.summary.skipped_frames, 0);
    EXPECT_EQ(result.summary.lossless_frames, 3);
    EXPECT_DOUBLE_EQ(result.summary.coverage, 1.0);
    EXPECT_TRUE(std::isinf(result.summary.mean_psnr_db));
    EXPECT_TRUE(std::isinf(result.summary.min_psnr_db));
    EXPECT_NEAR(result.summary.mean_ssim, 1.0, 1e-9);
    EXPECT_FALSE(result.summary.duration_mismatch);
    for (const auto& frame : result.frames) {
        EXPECT_TRUE(frame.valid) << frame.error_message;
        EXPECT_TRUE(std::isinf(frame.psnr_db));
        EXPECT_NEAR(frame.ssim, 1.0, 1e-9);
        EXPECT_DOUBLE_EQ(frame.alignment_delta_seconds, 0.0);
    }
}

TEST(ReferenceQualityAnalyzerTest, ConstantLumaDeltaProducesExpectedPsnr) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    for (int i = 0; i < 3; ++i) {
        reference.push_back(MakeSample(i * 0.04, i, 100));
        distorted.push_back(MakeSample(i * 0.04, 100 + i, 110));
    }

    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, ReferenceQualityOptions());

    ASSERT_TRUE(result.valid) << result.error_message;
    ASSERT_EQ(result.frames.size(), 3u);
    EXPECT_EQ(result.summary.compared_frames, 3);
    EXPECT_EQ(result.summary.lossless_frames, 0);
    EXPECT_NEAR(result.summary.mean_psnr_db, ExpectedPsnr(10.0), 1e-6);
    EXPECT_NEAR(result.summary.min_psnr_db, ExpectedPsnr(10.0), 1e-6);
    EXPECT_LT(result.summary.mean_ssim, 1.0);
    for (const auto& frame : result.frames) {
        EXPECT_NEAR(frame.psnr_db, ExpectedPsnr(10.0), 1e-6);
    }
}

TEST(ReferenceQualityAnalyzerTest, FirstFrameNormalizationAlignsDifferentStartPts) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    for (int i = 0; i < 3; ++i) {
        reference.push_back(MakeGradientSample(5.0 + i * 0.04, i));
        distorted.push_back(MakeGradientSample(100.0 + i * 0.04, i));
    }

    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, ReferenceQualityOptions());

    ASSERT_TRUE(result.valid) << result.error_message;
    EXPECT_EQ(result.summary.compared_frames, 3);
    ASSERT_FALSE(result.frames.empty());
    EXPECT_NEAR(result.frames.front().timestamp_seconds, 0.0, 1e-9);
    EXPECT_NEAR(result.frames.front().distorted_timestamp_seconds, 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(result.frames.front().alignment_delta_seconds, 0.0);
}

TEST(ReferenceQualityAnalyzerTest, ManualOffsetMatchesShiftedDistortedSequence) {
    std::vector<FrameSample> reference;
    for (int i = 0; i < 3; ++i)
        reference.push_back(MakeGradientSample(i * 0.04, i, i * 5));

    // 待测视频前 0.48s 是片头（恒定暗场），对应画面从 0.48s 才开始。
    // 两侧都按首帧归一到 0，所以只有给了 offset 才能把内容对上。
    std::vector<FrameSample> distorted;
    for (int j = 0; j <= 15; ++j) {
        const double t = j * 0.04;
        if (j >= 12)
            distorted.push_back(MakeGradientSample(t, j, (j - 12) * 5));
        else
            distorted.push_back(MakeSample(t, j, 20));
    }

    ReferenceQualityOptions options;
    options.offset_seconds = 0.5;
    const auto aligned = ReferenceQualityAnalyzer::Compare(reference, distorted, options);
    ASSERT_TRUE(aligned.valid) << aligned.error_message;
    EXPECT_EQ(aligned.summary.compared_frames, 3);
    EXPECT_DOUBLE_EQ(aligned.summary.coverage, 1.0);
    EXPECT_EQ(aligned.summary.lossless_frames, 3);

    // 不给偏移: 参考画面会配上 0.5s 前的片头，指标必须明显变差而不是"照样满分"
    const auto unaligned = ReferenceQualityAnalyzer::Compare(reference, distorted, ReferenceQualityOptions());
    ASSERT_TRUE(unaligned.valid) << unaligned.error_message;
    EXPECT_EQ(unaligned.summary.compared_frames, 3);
    EXPECT_LT(unaligned.summary.mean_psnr_db, 20.0);
    EXPECT_LT(unaligned.summary.mean_ssim, 0.9);
}

TEST(ReferenceQualityAnalyzerTest, NeverReusesDistortedFrameForTwoTargets) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    // 参考比待测更密: 0.02s 间隔 vs 0.03s，容差 0.02s
    reference.push_back(MakeGradientSample(0.00, 0));
    reference.push_back(MakeGradientSample(0.02, 1));
    reference.push_back(MakeGradientSample(0.04, 2));
    distorted.push_back(MakeGradientSample(0.00, 10));
    distorted.push_back(MakeGradientSample(0.03, 11));

    ReferenceQualityOptions options;
    options.alignment_tolerance_seconds = 0.02;
    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, options);

    ASSERT_TRUE(result.valid) << result.error_message;
    ASSERT_EQ(result.frames.size(), 2u);
    EXPECT_EQ(result.summary.compared_frames, 2);
    EXPECT_EQ(result.summary.skipped_frames, 1); // 0.04 已无待测帧可配
    EXPECT_NE(result.frames[0].distorted_frame_index, result.frames[1].distorted_frame_index);
    EXPECT_DOUBLE_EQ(result.summary.coverage, 2.0 / 3.0);
}

TEST(ReferenceQualityAnalyzerTest, DimensionMismatchFailsExplicitlyInsteadOfFakingMetrics) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    for (int i = 0; i < 2; ++i) {
        reference.push_back(MakeGradientSample(i * 0.04, i, 0, 16, 16));
        distorted.push_back(MakeGradientSample(i * 0.04, i, 0, 8, 8));
    }

    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, ReferenceQualityOptions());

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.summary.compared_frames, 0);
    EXPECT_EQ(result.summary.failed_frames, 2);
    ASSERT_EQ(result.frames.size(), 2u);
    for (const auto& frame : result.frames) {
        EXPECT_FALSE(frame.valid);
        EXPECT_NE(std::string::npos, frame.error_message.find("different dimensions"));
        EXPECT_TRUE(std::isnan(frame.psnr_db));
        EXPECT_TRUE(std::isnan(frame.ssim));
    }
    EXPECT_NE(std::string::npos, result.error_message.find("所有帧比较失败"));
}

TEST(ReferenceQualityAnalyzerTest, MaxFramesTruncatesComparison) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    for (int i = 0; i < 5; ++i) {
        reference.push_back(MakeGradientSample(i * 0.04, i));
        distorted.push_back(MakeGradientSample(i * 0.04, i));
    }

    ReferenceQualityOptions options;
    options.max_frames = 2;
    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, options);

    ASSERT_TRUE(result.valid) << result.error_message;
    EXPECT_EQ(result.summary.compared_frames, 2);
    EXPECT_EQ(result.frames.size(), 2u);
    EXPECT_EQ(result.summary.target_count, 5);
    EXPECT_TRUE(result.summary.truncated);
}

TEST(ReferenceQualityAnalyzerTest, TimeWindowRestrictsTargets) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    for (int i = 0; i < 4; ++i) {
        reference.push_back(MakeGradientSample(i * 0.04, i));
        distorted.push_back(MakeGradientSample(i * 0.04, i));
    }

    ReferenceQualityOptions options;
    options.start_seconds = 0.04;
    options.end_seconds = 0.08;
    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, options);

    ASSERT_TRUE(result.valid) << result.error_message;
    EXPECT_EQ(result.summary.target_count, 2);
    EXPECT_EQ(result.summary.compared_frames, 2);
    ASSERT_FALSE(result.frames.empty());
    EXPECT_NEAR(result.frames.front().timestamp_seconds, 0.04, 1e-9);
    EXPECT_NEAR(result.frames.back().timestamp_seconds, 0.08, 1e-9);
}

TEST(ReferenceQualityAnalyzerTest, ShorterDistortedSequenceSkipsTailAndFlagsDurationMismatch) {
    std::vector<FrameSample> reference;
    std::vector<FrameSample> distorted;
    for (int i = 0; i < 4; ++i)
        reference.push_back(MakeGradientSample(static_cast<double>(i), i));
    for (int i = 0; i < 2; ++i)
        distorted.push_back(MakeGradientSample(static_cast<double>(i), i));

    const auto result = ReferenceQualityAnalyzer::Compare(reference, distorted, ReferenceQualityOptions());

    ASSERT_TRUE(result.valid) << result.error_message;
    EXPECT_EQ(result.summary.compared_frames, 2);
    EXPECT_EQ(result.summary.skipped_frames, 2);
    EXPECT_DOUBLE_EQ(result.summary.coverage, 0.5);
    EXPECT_TRUE(result.summary.duration_mismatch);
}

TEST(ReferenceQualityAnalyzerTest, EmptyInputsFailWithMessage) {
    const std::vector<FrameSample> empty;
    const std::vector<FrameSample> one = {MakeGradientSample(0.0, 0)};

    const auto no_reference = ReferenceQualityAnalyzer::Compare(empty, one, ReferenceQualityOptions());
    EXPECT_FALSE(no_reference.valid);
    EXPECT_FALSE(no_reference.error_message.empty());

    const auto no_distorted = ReferenceQualityAnalyzer::Compare(one, empty, ReferenceQualityOptions());
    EXPECT_FALSE(no_distorted.valid);
    EXPECT_FALSE(no_distorted.error_message.empty());
}

TEST(ReferenceQualityAnalyzerTest, NegativeToleranceIsRejected) {
    const std::vector<FrameSample> sequence = {MakeGradientSample(0.0, 0)};

    ReferenceQualityOptions options;
    options.alignment_tolerance_seconds = -0.01;
    const auto result = ReferenceQualityAnalyzer::Compare(sequence, sequence, options);

    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error_message.empty());
}
