#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <vector>

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

    // 往当前 frame 挂一组运动矢量记录 (每条对应一个 AVMotionVector)
    void AttachMotionVectors(const std::vector<AVMotionVector>& records) {
        AVFrameSideData* side_data = av_frame_new_side_data(frame_.get(), AV_FRAME_DATA_MOTION_VECTORS,
                                                            static_cast<int>(records.size() * sizeof(AVMotionVector)));
        ASSERT_NE(side_data, nullptr);
        auto* dst = reinterpret_cast<AVMotionVector*>(side_data->data);
        for (std::size_t i = 0; i < records.size(); ++i) {
            dst[i] = records[i];
        }
    }

    videoeye::MacroblockAnalyzer analyzer_;
    std::unique_ptr<AVFrame, decltype(&FreeFrame)> frame_{nullptr, FreeFrame};
};

AVMotionVector MakeRecord(int dst_x, int dst_y, int w, int h, int motion_x, int motion_y, int motion_scale,
                          int source) {
    AVMotionVector mv{};
    mv.dst_x = dst_x;
    mv.dst_y = dst_y;
    mv.w = w;
    mv.h = h;
    mv.motion_x = motion_x;
    mv.motion_y = motion_y;
    mv.motion_scale = motion_scale;
    mv.source = source;
    return mv;
}

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

// P 帧的帧内块数不再"不可用"：H.264 按 16x16 网格，减掉被 inter 块覆盖的单元。
// 这里只有 1 个 16x16 块落在左上角，所以帧内 = 120*68 - 1。
TEST_F(MacroblockAnalyzerTest, H264PredictedFrameEstimatesIntraBlocksFromGridCoverage) {
    AttachMotionVectors({MakeRecord(0, 0, 16, 16, 8, 0, 4, -1)});
    const auto result = Analyze(AV_PICTURE_TYPE_P);
    ASSERT_EQ(result.motion_vectors.size(), 1u);
    EXPECT_TRUE(result.stats.block_counts_estimated);
    EXPECT_TRUE(result.stats.intra_count_available);
    EXPECT_EQ(result.stats.total_blocks, 120 * 68);
    EXPECT_EQ(result.stats.intra_count, 120 * 68 - 1);
    EXPECT_EQ(result.stats.distinct_block_count, 1);
}

// B 帧的同一块会同时给出前向与后向两条记录（dst / 块尺寸相同，只差 source）。
// 块大小分布、幅度分布、平均/最大幅度必须按去重块统计，否则整帧统计翻倍；
// 前向/后向仍按记录数计数，那是两次真实的参考。
TEST_F(MacroblockAnalyzerTest, BidirectionalFrameDeduplicatesForwardAndBackwardRecords) {
    AttachMotionVectors({
        MakeRecord(8, 8, 16, 16, 8, 0, 4, -1),  // 前向: 幅度 2px
        MakeRecord(8, 8, 16, 16, 24, 0, 4, 1),  // 后向: 幅度 6px, 同一块
        MakeRecord(40, 8, 16, 16, 0, 0, 4, -1), // 另一块: 幅度 0
    });
    const auto result = Analyze(AV_PICTURE_TYPE_B);
    const auto& s = result.stats;
    ASSERT_EQ(result.motion_vectors.size(), 3u);
    EXPECT_EQ(s.forward_count, 2);
    EXPECT_EQ(s.backward_count, 1);
    EXPECT_EQ(s.distinct_block_count, 2);

    // 两条记录合并成一块，块大小分布不重复计数
    EXPECT_EQ(s.count_16x16, 2);
    EXPECT_EQ(s.count_16x8 + s.count_8x16 + s.count_8x8 + s.count_8x4 + s.count_4x8 + s.count_4x4 + s.count_other, 0);

    // 同一块的前/后向记录取较大幅度：max(2, 6) = 6
    EXPECT_DOUBLE_EQ(s.max_motion_magnitude, 6.0);
    EXPECT_DOUBLE_EQ(s.avg_motion_magnitude, 3.0); // (6 + 0) / 2
    EXPECT_EQ(s.mag_0_2, 1);
    EXPECT_EQ(s.mag_4_8, 1);
    EXPECT_EQ(s.mag_2_4, 0);
    EXPECT_EQ(s.mag_8_16, 0);
    EXPECT_EQ(s.mag_16_plus, 0);

    // 两个块覆盖两个网格单元
    EXPECT_TRUE(s.intra_count_available);
    EXPECT_EQ(s.intra_count, 120 * 68 - 2);
}

// 一个 16x16 宏块内的多个 8x8 分区不能把网格覆盖扣两次：
// 两个 8x8 块的像素范围都落在 (0,0) 单元，覆盖数仍是 1。
TEST_F(MacroblockAnalyzerTest, SubPartitionsInsideOneGridCellCoverItOnce) {
    AttachMotionVectors({
        MakeRecord(4, 4, 8, 8, 4, 0, 4, -1),
        MakeRecord(12, 4, 8, 8, 4, 0, 4, -1),
    });
    const auto result = Analyze(AV_PICTURE_TYPE_P);
    const auto& s = result.stats;
    EXPECT_EQ(s.distinct_block_count, 2);
    EXPECT_EQ(s.count_8x8, 2);
    EXPECT_EQ(s.total_blocks, 120 * 68);
    EXPECT_TRUE(s.intra_count_available);
    EXPECT_EQ(s.intra_count, 120 * 68 - 1);
}

// FFmpeg 的 export_mvs 只在部分解码器实现。UI 需要据此区分「解码器不支持」
// 与「这一帧恰好没有运动矢量」，否则 HEVC/AV1/VP9 会看起来像分析失效。
TEST(MacroblockCodecTypeTests, ReportsWhichCodecsExportMotionVectors) {
    using namespace videoeye::CodecType;
    EXPECT_TRUE(SupportsMotionVectorExport(AV_CODEC_ID_H264));
    EXPECT_TRUE(SupportsMotionVectorExport(AV_CODEC_ID_MPEG2VIDEO));
    EXPECT_TRUE(SupportsMotionVectorExport(AV_CODEC_ID_MPEG4));
    EXPECT_TRUE(SupportsMotionVectorExport(AV_CODEC_ID_H263));
    // 实测内置 FFmpeg 8.x: 以下编码 P/B 帧的 MV side data 均为 0 条
    EXPECT_FALSE(SupportsMotionVectorExport(AV_CODEC_ID_HEVC));
    EXPECT_FALSE(SupportsMotionVectorExport(AV_CODEC_ID_VP9));
    EXPECT_FALSE(SupportsMotionVectorExport(AV_CODEC_ID_AV1));

    EXPECT_STREQ(DisplayName(AV_CODEC_ID_HEVC), "HEVC");
    EXPECT_STREQ(DisplayName(AV_CODEC_ID_AV1), "AV1");
    EXPECT_STREQ(DisplayName(9999), "");
}

} // namespace
