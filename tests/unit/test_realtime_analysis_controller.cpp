#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include "core/player/RealtimeAnalysisController.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

class videoeye::player::RealtimeAnalysisControllerTestAccess {
public:
    static void Feed(RealtimeAnalysisController& controller, const AVFrame* frame, double timestamp) {
        controller.FeedVisualDefectFrame(frame, timestamp, false);
    }

    static void Seek(RealtimeAnalysisController& controller, double target_ms) {
        controller.OnSeekDone(target_ms, model::SeekMode::ExactFrame);
    }

    static int SampleCount(const RealtimeAnalysisController& controller) {
        return controller.visual_defect_analyzer_.analyzed_samples();
    }

    static model::VisualDefectReport Snapshot(const RealtimeAnalysisController& controller) {
        return controller.visual_defect_analyzer_.Snapshot();
    }
};

namespace {

using videoeye::player::RealtimeAnalysisControllerTestAccess;

void FreeFrame(AVFrame* frame) {
    av_frame_free(&frame);
}

class RealtimeAnalysisControllerTest : public testing::Test {
protected:
    void SetUp() override {
        frame_.reset(av_frame_alloc());
        ASSERT_NE(frame_, nullptr);
        frame_->format = AV_PIX_FMT_GRAY8;
        frame_->width = 64;
        frame_->height = 36;
        ASSERT_GE(av_frame_get_buffer(frame_.get(), 32), 0);
        for (int row = 0; row < frame_->height; ++row) {
            std::fill_n(frame_->data[0] + row * frame_->linesize[0], frame_->width, 128);
        }
        videoeye::VisualDefectOptions options;
        options.sample_fps = 2.0;
        options.analysis_width = 64;
        options.capture_rgb = false;
        controller_.SetVisualDefectOptions(options);
        controller_.SetVisualDefectAnalysisEnabled(true);
    }

    void Feed(double timestamp) {
        RealtimeAnalysisControllerTestAccess::Feed(controller_, frame_.get(), timestamp);
    }

    bool WaitForSamples(int expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (RealtimeAnalysisControllerTestAccess::SampleCount(controller_) < expected) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    // 给工作线程留出"如果真收到帧就该处理完"的时间，用来断言"这一帧根本没被投递"。
    // 直接断言计数会与异步处理赛跑，测不出问题。
    void Settle() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    videoeye::player::AnalysisSession analysis_;
    videoeye::player::PlaybackSession playback_;
    videoeye::player::RealtimeAnalysisController controller_{analysis_, playback_};
    std::unique_ptr<AVFrame, decltype(&FreeFrame)> frame_{nullptr, FreeFrame};
};

TEST_F(RealtimeAnalysisControllerTest, BackwardTimestampResumesSamplingImmediately) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    Feed(10.0);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_DOUBLE_EQ(report.samples.back().timestamp_seconds, 10.0);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

TEST_F(RealtimeAnalysisControllerTest, BackwardSeekResetsSamplingClockAndFrameHistory) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    RealtimeAnalysisControllerTestAccess::Seek(controller_, 10000.0);
    Feed(10.0);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

TEST_F(RealtimeAnalysisControllerTest, ShortForwardSeekDoesNotCompareUnrelatedFrames) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    RealtimeAnalysisControllerTestAccess::Seek(controller_, 61000.0);
    Feed(61.0);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

TEST_F(RealtimeAnalysisControllerTest, SamplingIntervalStillAppliesAfterSeek) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    RealtimeAnalysisControllerTestAccess::Seek(controller_, 10000.0);
    Feed(10.0);
    ASSERT_TRUE(WaitForSamples(2));
    Feed(10.1);
    EXPECT_EQ(RealtimeAnalysisControllerTestAccess::SampleCount(controller_), 2);
    Feed(10.5);
    ASSERT_TRUE(WaitForSamples(3));
}

// 时间戳小幅倒退（小于一个采样间隔）不该被当成 seek: 这一帧本来就会被节流丢掉，
// 而重建工作线程 + 清空亮度历史会让闪烁 / 冻结这类靠历史累积的判定失效。
TEST_F(RealtimeAnalysisControllerTest, SmallBackwardJitterKeepsFrameHistory) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    Feed(59.99); // 倒退 10ms，远小于 2fps 的 0.5s 采样间隔
    Settle();
    EXPECT_EQ(RealtimeAnalysisControllerTestAccess::SampleCount(controller_), 1);
    Feed(60.5);
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    // 帧历史没被清掉: 这一帧仍与 60.0s 的那帧比较（两张纯色图 → 差值≈0，而不是 NaN）
    EXPECT_FALSE(std::isnan(report.samples.back().frame_diff));
    EXPECT_LT(std::fabs(report.samples.back().frame_diff), 1e-6);
}

// 向前大跨度跳变（没有 seek 回调，比如时间戳直接跳进新分段）同样要清掉帧历史。
TEST_F(RealtimeAnalysisControllerTest, ForwardJumpResetsTimeline) {
    Feed(60.0);
    ASSERT_TRUE(WaitForSamples(1));
    Feed(65.0); // 跨度 5s > 2s 的跳变阈值
    ASSERT_TRUE(WaitForSamples(2));
    const auto report = RealtimeAnalysisControllerTestAccess::Snapshot(controller_);
    ASSERT_EQ(report.samples.size(), 2u);
    EXPECT_DOUBLE_EQ(report.samples.back().timestamp_seconds, 65.0);
    EXPECT_TRUE(std::isnan(report.samples.back().frame_diff));
}

} // namespace
