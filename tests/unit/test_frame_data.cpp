#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/model/FrameData.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace {

TEST(FrameDataTest, CopyFromCreatesIndependentPlaneStorage) {
    videoeye::model::FrameData source;
    source.width = 4;
    source.height = 4;
    source.format = AV_PIX_FMT_YUV420P;
    source.pts = 42;
    source.timestamp = 1.5;

    std::vector<uint8_t> y(16, 1);
    std::vector<uint8_t> u(4, 2);
    std::vector<uint8_t> v(4, 3);
    
    source.data[0] = y.data();
    source.linesize[0] = 4;
    source.data[1] = u.data();
    source.linesize[1] = 2;
    source.data[2] = v.data();
    source.linesize[2] = 2;

    videoeye::model::FrameData copy;
    copy.CopyFrom(source);

    ASSERT_NE(copy.data[0], nullptr);
    ASSERT_NE(copy.data[1], nullptr);
    ASSERT_NE(copy.data[2], nullptr);
    ASSERT_NE(copy.data[0], source.data[0]);
    ASSERT_NE(copy.data[1], source.data[1]);
    ASSERT_NE(copy.data[2], source.data[2]);
    EXPECT_EQ(copy.owned[0].size(), 16U);
    EXPECT_EQ(copy.owned[1].size(), 4U);
    EXPECT_EQ(copy.owned[2].size(), 4U);
    ASSERT_EQ(copy.width, source.width);
    ASSERT_EQ(copy.height, source.height);
    ASSERT_EQ(copy.format, source.format);
    ASSERT_EQ(copy.pts, source.pts);
    EXPECT_DOUBLE_EQ(copy.timestamp, source.timestamp);

    y[0] = 99;
    EXPECT_EQ(copy.data[0][0], 1);
}

} // namespace