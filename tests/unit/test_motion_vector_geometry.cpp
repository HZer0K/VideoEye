#include <gtest/gtest.h>

#include "ui/main_window/MotionVectorGeometry.h"

namespace {

using videoeye::model::MotionVectorInfo;
using videoeye::ui::ComputeMvArrow;
using videoeye::ui::ComputeMvBlock;
using videoeye::ui::MvOverlayViewport;
using videoeye::ui::MvPixelMagnitude;
using videoeye::ui::MvSampleStride;

MotionVectorInfo MakeMv(int x, int y, int w, int h, int motion_x, int motion_y, int motion_scale = 4) {
    MotionVectorInfo mv;
    mv.block_x = x;
    mv.block_y = y;
    mv.block_w = static_cast<uint8_t>(w);
    mv.block_h = static_cast<uint8_t>(h);
    mv.motion_x = motion_x;
    mv.motion_y = motion_y;
    mv.motion_scale = static_cast<uint16_t>(motion_scale);
    return mv;
}

class MotionVectorGeometryTest : public testing::Test {
protected:
    // 1920x1080 画面铺到 960x540 区域：缩放系数 0.5，左上角偏移 (100, 50)
    static constexpr int kFrameWidth = 1920;
    static constexpr int kFrameHeight = 1080;
    static constexpr double kAmplify = 3.0;
    static constexpr double kMaxLength = 60.0;
    MvOverlayViewport viewport_{100.0, 50.0, 960.0, 540.0};
};

// dst 是块中心：箭头起点必须等于块中心在区域里的位置，不能再额外加半个块。
TEST_F(MotionVectorGeometryTest, ArrowStartsAtBlockCenter) {
    const auto mv = MakeMv(8, 8, 16, 16, 8, 0); // 1/4 像素精度下的 2px 运动
    const auto arrow = ComputeMvArrow(mv, viewport_, kFrameWidth, kFrameHeight, kAmplify, kMaxLength);
    EXPECT_DOUBLE_EQ(arrow.start_x, 104.0); // 100 + 8 * 0.5
    EXPECT_DOUBLE_EQ(arrow.start_y, 54.0);  // 50 + 8 * 0.5
    EXPECT_DOUBLE_EQ(arrow.end_x, 107.0);   // 104 + 2px * 0.5 * 3
    EXPECT_DOUBLE_EQ(arrow.end_y, 54.0);
    EXPECT_DOUBLE_EQ(arrow.length, 3.0);
}

// 块矩形同样以中心展开半个块：16x16 块在 0.5 缩放下是 8x8，左上角落在 (100,50)。
TEST_F(MotionVectorGeometryTest, BlockRectExpandsAroundCenter) {
    const auto mv = MakeMv(8, 8, 16, 16, 8, 0);
    const auto block = ComputeMvBlock(mv, viewport_, kFrameWidth, kFrameHeight);
    EXPECT_DOUBLE_EQ(block.left, 100.0);
    EXPECT_DOUBLE_EQ(block.top, 50.0);
    EXPECT_DOUBLE_EQ(block.width, 8.0);
    EXPECT_DOUBLE_EQ(block.height, 8.0);

    // 块矩形中心与箭头起点必须重合（两块代码用同一套中心语义）
    const auto arrow = ComputeMvArrow(mv, viewport_, kFrameWidth, kFrameHeight, kAmplify, kMaxLength);
    EXPECT_DOUBLE_EQ(block.left + block.width / 2.0, arrow.start_x);
    EXPECT_DOUBLE_EQ(block.top + block.height / 2.0, arrow.start_y);
}

// 小分区（8x8）也按自己的中心展开，不能按 16x16 宏块算。
TEST_F(MotionVectorGeometryTest, SmallPartitionUsesItsOwnSize) {
    const auto mv = MakeMv(12, 12, 8, 8, 0, 8);
    const auto block = ComputeMvBlock(mv, viewport_, kFrameWidth, kFrameHeight);
    EXPECT_DOUBLE_EQ(block.left, 104.0); // 100 + (12 - 4) * 0.5
    EXPECT_DOUBLE_EQ(block.top, 54.0);   // 50 + (12 - 4) * 0.5
    EXPECT_DOUBLE_EQ(block.width, 4.0);
    EXPECT_DOUBLE_EQ(block.height, 4.0);
}

// 过长箭头按 max_length 截断，方向保持不变。
TEST_F(MotionVectorGeometryTest, ArrowIsClampedToMaxLength) {
    const auto mv = MakeMv(100, 100, 16, 16, 4000, 0); // 1000px 运动
    const auto arrow = ComputeMvArrow(mv, viewport_, kFrameWidth, kFrameHeight, kAmplify, kMaxLength);
    EXPECT_DOUBLE_EQ(arrow.length, kMaxLength);
    EXPECT_DOUBLE_EQ(arrow.end_x - arrow.start_x, kMaxLength);
    EXPECT_DOUBLE_EQ(arrow.end_y, arrow.start_y);
}

// motion_scale 缺失（0）时按 1 处理，不能除零/产生无穷大。
TEST_F(MotionVectorGeometryTest, MissingMotionScaleFallsBackToOne) {
    const auto mv = MakeMv(0, 0, 16, 16, 3, 4, 0);
    EXPECT_DOUBLE_EQ(MvPixelMagnitude(mv), 5.0);
    const auto arrow = ComputeMvArrow(mv, viewport_, kFrameWidth, kFrameHeight, kAmplify, kMaxLength);
    EXPECT_DOUBLE_EQ(arrow.length, 5.0 * 0.5 * kAmplify);
}

// 非法帧尺寸时返回零几何，调用方据此跳过绘制（不能画出 NaN 坐标）。
TEST_F(MotionVectorGeometryTest, InvalidFrameSizeReturnsZeroGeometry) {
    const auto mv = MakeMv(8, 8, 16, 16, 8, 0);
    const auto arrow = ComputeMvArrow(mv, viewport_, 0, kFrameHeight, kAmplify, kMaxLength);
    EXPECT_DOUBLE_EQ(arrow.start_x, 0.0);
    EXPECT_DOUBLE_EQ(arrow.end_x, 0.0);
    EXPECT_DOUBLE_EQ(arrow.length, 0.0);

    const auto block = ComputeMvBlock(mv, viewport_, kFrameWidth, 0);
    EXPECT_DOUBLE_EQ(block.left, 0.0);
    EXPECT_DOUBLE_EQ(block.width, 0.0);
}

// 抽样步长：不超过上限时全量（1），超过后按 ceil(count/上限) 等距跳点，
// 保证画出的条数不超过上限且尽量均匀。
TEST(MvSampleStrideTest, StrideKeepsDrawnCountUnderLimit) {
    EXPECT_EQ(MvSampleStride(0, 1500), 1u);
    EXPECT_EQ(MvSampleStride(1500, 1500), 1u);
    EXPECT_EQ(MvSampleStride(1501, 1500), 2u);
    EXPECT_EQ(MvSampleStride(2401, 1500), 2u);
    EXPECT_EQ(MvSampleStride(3001, 1500), 3u);
    EXPECT_EQ(MvSampleStride(6000, 1500), 4u);

    // 上限为 0 视为不限：
    EXPECT_EQ(MvSampleStride(5000, 0), 1u);

    // 画出的条数（向上取整）不会超过上限：
    const std::size_t total = 5000;
    const std::size_t max_count = 1500;
    const std::size_t stride = MvSampleStride(total, max_count);
    EXPECT_LE((total + stride - 1) / stride, max_count);
}

} // namespace
