#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <string>

#include "core/analysis/quality/ReferenceQualityRunner.h"
#include "tests/support/TestMediaFactory.h"

namespace {

std::string MediaDir() {
    const std::string dir = (std::filesystem::temp_directory_path() / "videoeye_reference_quality").string();
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

// 同一 spec 生成的两份样本内容逐帧一致（工厂是确定性的）:
// 走完整的"打开 -> 解码 -> 采样 -> 对齐 -> PSNR/SSIM"链路，结果必须是无损。
TEST(ReferenceQualityRunnerTest, IdenticalMediaIsLossless) {
    const std::string dir = MediaDir();
    const auto reference = videoeye_test::CreateTestMedia(dir, "reference");
    ASSERT_TRUE(reference.ok) << reference.error;
    const auto distorted = videoeye_test::CreateTestMedia(dir, "distorted");
    ASSERT_TRUE(distorted.ok) << distorted.error;

    const auto result = videoeye::ReferenceQualityRunner::Run(reference.path, distorted.path);

    ASSERT_TRUE(result.valid) << result.error_message;
    EXPECT_FALSE(result.cancelled);
    EXPECT_GE(result.summary.compared_frames, 2);
    EXPECT_EQ(result.summary.failed_frames, 0);
    EXPECT_DOUBLE_EQ(result.summary.coverage, 1.0);
    // 完全一致时 PSNR 是 +inf，这里用 >= 40 同时容纳"编码器抖动带来的近无损"
    EXPECT_GE(result.summary.min_psnr_db, 40.0);
    EXPECT_NEAR(result.summary.mean_ssim, 1.0, 1e-4);
    for (const auto& frame : result.frames) {
        EXPECT_TRUE(frame.valid) << frame.error_message;
        EXPECT_EQ(frame.width, frame.height * 16 / 9); // 16:9 源，样本按比例缩放
    }
}

// 宽高比不同的两个文件会得到不同尺寸的样本: 必须逐帧报错，不能悄悄拉伸后给指标。
TEST(ReferenceQualityRunnerTest, AspectRatioMismatchIsReportedNotFaked) {
    const std::string dir = MediaDir();
    const auto reference = videoeye_test::CreateTestMedia(dir, "reference_169");
    ASSERT_TRUE(reference.ok) << reference.error;

    videoeye_test::TestMediaSpec spec;
    spec.width = 128;
    spec.height = 96; // 4:3 -> 320x240，与 16:9 的 320x180 不同
    const auto distorted = videoeye_test::CreateTestMedia(dir, "distorted_43", spec);
    ASSERT_TRUE(distorted.ok) << distorted.error;

    const auto result = videoeye::ReferenceQualityRunner::Run(reference.path, distorted.path);

    EXPECT_FALSE(result.valid);
    EXPECT_GT(result.summary.failed_frames, 0);
    EXPECT_EQ(result.summary.compared_frames, 0);
    EXPECT_NE(std::string::npos, result.error_message.find("所有帧比较失败"));
    ASSERT_FALSE(result.frames.empty());
    EXPECT_NE(std::string::npos, result.frames.front().error_message.find("different dimensions"));
}

// 取消要能及时生效，且已经算出来的帧不能丢。
TEST(ReferenceQualityRunnerTest, CancelFromProgressKeepsPartialResults) {
    const std::string dir = MediaDir();
    const auto reference = videoeye_test::CreateTestMedia(dir, "cancel_reference");
    ASSERT_TRUE(reference.ok) << reference.error;
    const auto distorted = videoeye_test::CreateTestMedia(dir, "cancel_distorted");
    ASSERT_TRUE(distorted.ok) << distorted.error;

    videoeye::ReferenceQualityRunOptions options;
    options.target_fps = 25.0; // 40 帧全采，才有足够帧数触发进度回调
    std::atomic<bool> cancel{false};
    int progress_calls = 0;
    const auto result = videoeye::ReferenceQualityRunner::Run(reference.path, distorted.path, options, &cancel,
                                                              [&](const videoeye::ReferenceQualityRunProgress&) {
                                                                  ++progress_calls;
                                                                  cancel = true;
                                                              });

    EXPECT_TRUE(result.cancelled);
    EXPECT_GE(progress_calls, 1);
    ASSERT_TRUE(result.valid) << result.error_message;
    EXPECT_EQ(result.summary.compared_frames, 25); // 进度回调在第 25 帧触发，随后立即停
    EXPECT_EQ(result.frames.size(), 25u);
}

TEST(ReferenceQualityRunnerTest, CorruptReferenceFailsWithMessage) {
    const std::string dir = MediaDir();
    const std::string bad = videoeye_test::CreateCorruptMediaFile(dir, "bad_reference");
    const auto good = videoeye_test::CreateTestMedia(dir, "good_for_bad");
    ASSERT_TRUE(good.ok) << good.error;

    const auto result = videoeye::ReferenceQualityRunner::Run(bad, good.path);

    EXPECT_FALSE(result.valid);
    EXPECT_NE(std::string::npos, result.error_message.find("参考视频打开失败"));
}

// 双路网络流需要另一套缓冲/对齐策略，离线评估直接拒绝并说明原因。
TEST(ReferenceQualityRunnerTest, NetworkPathsAreRejected) {
    const auto result = videoeye::ReferenceQualityRunner::Run("http://example.com/a.mp4", "http://example.com/b.mp4");

    EXPECT_FALSE(result.valid);
    EXPECT_NE(std::string::npos, result.error_message.find("只支持本地文件"));
}

TEST(ReferenceQualityRunnerTest, EmptyPathIsRejected) {
    const auto result = videoeye::ReferenceQualityRunner::Run(std::string(), std::string());

    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.error_message.empty());
}
