#include <gtest/gtest.h>

#include <memory>

#include "core/analysis/quality/MacroblockAnalyzer.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
}

namespace {

void FreeFrame(AVFrame* frame) {
    av_frame_free(&frame);
}

class MacroblockAnalyzerTest : public testing::Test {
protected:
    void SetUp() override {
        frame_.reset(av_frame_alloc());
        ASSERT_NE(frame_, nullptr);
        frame_->width = 1920;
        frame_->height = 1080;
    }

    videoeye::model::MacroblockFrameAnalysis Analyze(int frame_type, int codec_id = AV_CODEC_ID_H264) {
        return analyzer_.AnalyzeFrame(frame_.get(), 0, 0, 0.0, frame_type, codec_id);
    }

    videoeye::MacroblockAnalyzer analyzer_;
    std::unique_ptr<AVFrame, decltype(&FreeFrame)> frame_{nullptr, FreeFrame};
};

TEST_F(MacroblockAnalyzerTest, PredictedFrameWithoutMotionVectorsHasUnknownIntraCount) {
    const auto result = Analyze(AV_PICTURE_TYPE_P);
    EXPECT_EQ(result.frame_type, AV_PICTURE_TYPE_P);
    EXPECT_FALSE(result.has_motion_vectors);
    EXPECT_FALSE(result.stats.intra_count_available);
    EXPECT_EQ(result.stats.intra_count, 0);
}

TEST_F(MacroblockAnalyzerTest, BidirectionalFrameWithoutMotionVectorsHasUnknownIntraCount) {
    const auto result = Analyze(AV_PICTURE_TYPE_B);
    EXPECT_EQ(result.frame_type, AV_PICTURE_TYPE_B);
    EXPECT_FALSE(result.stats.intra_count_available);
    EXPECT_EQ(result.stats.intra_count, 0);
}

TEST_F(MacroblockAnalyzerTest, HevcIntraFrameCountsAreExplicitlyEstimated) {
    const auto result = Analyze(AV_PICTURE_TYPE_I, AV_CODEC_ID_HEVC);
    EXPECT_TRUE(result.stats.intra_count_available);
    EXPECT_TRUE(result.stats.block_counts_estimated);
    EXPECT_EQ(result.stats.intra_count, 510);
    EXPECT_EQ(result.stats.total_blocks, 510);
}

// AV1 / VP9 等编码的运动矢量语义与 H.264 / HEVC 不同，照 16x16 网格硬套会给出错误数字，
// 所以这类 I 帧不估帧内块数。注意此时 total_blocks 也停在 0：ComputeStats 对空 MV 列表
// 是 early-return，网格根本没铺过 —— 所以"总块数"同样不可用，这个 0 不是"0 个块"。
TEST_F(MacroblockAnalyzerTest, NonH264HevcIntraFrameDoesNotInventIntraCount) {
    const auto result = Analyze(AV_PICTURE_TYPE_I, AV_CODEC_ID_AV1);
    EXPECT_FALSE(result.stats.intra_count_available);
    EXPECT_EQ(result.stats.intra_count, 0);
    EXPECT_FALSE(result.stats.block_counts_estimated);
    EXPECT_EQ(result.stats.total_blocks, 0);
}

TEST_F(MacroblockAnalyzerTest, MotionVectorCountDoesNotInferIntraBlocks) {
    AVFrameSideData* side_data =
        av_frame_new_side_data(frame_.get(), AV_FRAME_DATA_MOTION_VECTORS, sizeof(AVMotionVector));
    ASSERT_NE(side_data, nullptr);
    auto* motion = reinterpret_cast<AVMotionVector*>(side_data->data);
    *motion = AVMotionVector{};
    motion->w = 16;
    motion->h = 16;
    motion->source = -1;
    motion->motion_scale = 4;
    const auto result = Analyze(AV_PICTURE_TYPE_P);
    ASSERT_EQ(result.motion_vectors.size(), 1u);
    EXPECT_TRUE(result.stats.block_counts_estimated);
    EXPECT_FALSE(result.stats.intra_count_available);
    EXPECT_EQ(result.stats.intra_count, 0);
}

}
