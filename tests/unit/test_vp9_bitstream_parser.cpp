#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/analysis/codec/Vp9BitstreamParser.h"

namespace {

using videoeye::Vp9BitstreamParser;
using videoeye::model::Vp9ColorSpace;

// --------------------------------------------------------------------------
// 逐位组装 VP9 uncompressed header（MSB-first）
//
// 测试数据自己按规范 6.2 逐位拼出来，不参考本项目 parser 的既有实现 ——
// 否则就会退化成「parser 和测试互相配合的错误」。
// VP9 的 f(n) 是 MSB-first（VP8 才是 LSB-first），断言里刻意钉住这一点。
// --------------------------------------------------------------------------
class BitWriter {
public:
    void Put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            const uint32_t bit = (value >> i) & 1u;
            const int pos = static_cast<int>(bits_.size());
            if (pos % 8 == 0) bytes_.push_back(0);
            bytes_.back() |= static_cast<uint8_t>(bit << (7 - (pos % 8)));
            bits_.push_back(bit != 0);
        }
    }
    void PutBool(bool v) { Put(v ? 1u : 0u, 1); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<bool> bits_;
};

struct Vp9HeaderSpec {
    int profile = 0;
    bool show_existing_frame = false;
    bool key_frame = true;
    bool show_frame = true;
    bool error_resilient = false;

    int ten_or_twelve_bit = 0;   // 0=10bit 1=12bit（仅 profile 2/3）
    int color_space = 2;         // BT.709
    bool color_range_full = false;
    int subsampling_x = 1;
    int subsampling_y = 1;

    int frame_width = 1920;
    int frame_height = 1080;
    bool render_size_different = false;
    int render_width = 0;
    int render_height = 0;
};

std::vector<uint8_t> BuildVp9Header(const Vp9HeaderSpec& s) {
    BitWriter w;
    w.Put(0x02, 2);                                  // frame_marker（恒为 2）
    w.Put(static_cast<uint32_t>(s.profile & 1), 1);  // profile_low_bit
    w.Put(static_cast<uint32_t>((s.profile >> 1) & 1), 1);  // profile_high_bit
    if (s.profile == 3) w.Put(0, 1);                 // reserved_zero
    w.PutBool(s.show_existing_frame);
    if (s.show_existing_frame) {
        w.Put(0, 3);  // frame_to_show_map_idx
        return w.bytes();
    }
    w.PutBool(!s.key_frame);  // frame_type: 0 = KEY_FRAME
    w.PutBool(s.show_frame);
    w.PutBool(s.error_resilient);
    if (!s.key_frame) return w.bytes();

    w.Put(0x498342u, 24);  // frame_sync_code

    if (s.profile == 2 || s.profile == 3) {
        w.PutBool(s.ten_or_twelve_bit != 0);
    }
    w.Put(static_cast<uint32_t>(s.color_space), 3);
    const bool has_range = (s.profile == 1 || s.profile == 3);
    if (s.color_space != static_cast<int>(Vp9ColorSpace::Srgb)) {
        if (has_range) w.PutBool(s.color_range_full);
        if (has_range) {
            w.PutBool(s.subsampling_x != 0);
            w.PutBool(s.subsampling_y != 0);
            w.Put(0, 1);  // reserved_zero
        }
    } else {
        if (has_range) w.PutBool(s.color_range_full);
        if (has_range) w.Put(0, 1);
    }
    w.Put(static_cast<uint32_t>(s.frame_width - 1), 16);
    w.Put(static_cast<uint32_t>(s.frame_height - 1), 16);
    w.PutBool(s.render_size_different);
    if (s.render_size_different) {
        w.Put(static_cast<uint32_t>(s.render_width - 1), 16);
        w.Put(static_cast<uint32_t>(s.render_height - 1), 16);
    }
    return w.bytes();
}

}  // namespace

TEST(Vp9BitstreamParserTests, Profile0KeyFrame1080p) {
    const std::vector<uint8_t> frame = BuildVp9Header(Vp9HeaderSpec{});
    // 0x82 是 profile 0 关键帧的固定开头：frame_marker=2、profile=0、
    // show_existing=0、keyframe、show=1、非 error resilient
    ASSERT_FALSE(frame.empty());
    EXPECT_EQ(frame[0], 0x82);
    EXPECT_EQ(frame[1], 0x49);
    EXPECT_EQ(frame[2], 0x83);
    EXPECT_EQ(frame[3], 0x42);

    EXPECT_TRUE(Vp9BitstreamParser::LooksLikeKeyFrame(frame.data(), frame.size()));

    const auto sh = Vp9BitstreamParser::ParseFromFrame(frame.data(), frame.size());
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.profile, 0);
    EXPECT_EQ(sh.bit_depth, 8);
    EXPECT_EQ(sh.frame_width, 1920);
    EXPECT_EQ(sh.frame_height, 1080);
    EXPECT_EQ(sh.width(), 1920);
    EXPECT_EQ(sh.height(), 1080);
    EXPECT_EQ(sh.color_space, Vp9ColorSpace::Bt709);
    EXPECT_EQ(sh.ChromaSubsamplingName(), "4:2:0");
    EXPECT_FALSE(sh.render_size_different);
    EXPECT_TRUE(sh.key_frame);
}

TEST(Vp9BitstreamParserTests, Profile2TenBitBt2020) {
    Vp9HeaderSpec spec;
    spec.profile = 2;
    spec.color_space = static_cast<int>(Vp9ColorSpace::Bt2020Ncl);
    spec.frame_width = 3840;
    spec.frame_height = 2160;
    const std::vector<uint8_t> frame = BuildVp9Header(spec);

    const auto sh = Vp9BitstreamParser::ParseFromFrame(frame.data(), frame.size());
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.profile, 2);
    // profile 2 的 ten_or_twelve_bit=0 → 10bit
    EXPECT_EQ(sh.bit_depth, 10);
    EXPECT_EQ(sh.frame_width, 3840);
    EXPECT_EQ(sh.frame_height, 2160);
    EXPECT_EQ(sh.ColorSpaceName(), "BT.2020 NCL");
    EXPECT_EQ(sh.ChromaSubsamplingName(), "4:2:0");
    EXPECT_EQ(sh.ProfileName(), "Profile 2 (10/12-bit 4:2:0)");
}

TEST(Vp9BitstreamParserTests, Profile1FullRange422) {
    // 4:2:2 + full range 只在 profile 1 / 3 里出现，顺带钉住
    // subsampling_x=1 / subsampling_y=0 的换算
    Vp9HeaderSpec spec;
    spec.profile = 1;
    spec.color_range_full = true;
    spec.subsampling_x = 1;
    spec.subsampling_y = 0;
    const std::vector<uint8_t> frame = BuildVp9Header(spec);

    const auto sh = Vp9BitstreamParser::ParseFromFrame(frame.data(), frame.size());
    ASSERT_TRUE(sh.present);
    EXPECT_EQ(sh.profile, 1);
    EXPECT_TRUE(sh.color_range_full);
    EXPECT_EQ(sh.ChromaSubsamplingName(), "4:2:2");
    EXPECT_EQ(sh.ProfileName(), "Profile 1 (8-bit 4:2:2/4:4:4)");
}

TEST(Vp9BitstreamParserTests, RenderSizeIsPreferredForDisplay) {
    Vp9HeaderSpec spec;
    spec.render_size_different = true;
    spec.render_width = 1440;
    spec.render_height = 810;
    const std::vector<uint8_t> frame = BuildVp9Header(spec);

    const auto sh = Vp9BitstreamParser::ParseFromFrame(frame.data(), frame.size());
    ASSERT_TRUE(sh.present);
    // 编码尺寸仍是 1920x1080，显示尺寸按 render_size
    EXPECT_EQ(sh.FrameWidth(), 1920);
    EXPECT_EQ(sh.FrameHeight(), 1080);
    EXPECT_EQ(sh.width(), 1440);
    EXPECT_EQ(sh.height(), 810);
}

TEST(Vp9BitstreamParserTests, NonKeyFrameYieldsNothing) {
    Vp9HeaderSpec spec;
    spec.key_frame = false;
    const std::vector<uint8_t> frame = BuildVp9Header(spec);
    // 非关键帧里没有 color_config / frame_size，一条都不该产出
    EXPECT_FALSE(Vp9BitstreamParser::LooksLikeKeyFrame(frame.data(), frame.size()));
    const auto sh = Vp9BitstreamParser::ParseFromFrame(frame.data(), frame.size());
    EXPECT_FALSE(sh.present);
}

TEST(Vp9BitstreamParserTests, ShowExistingFrameYieldsNothing) {
    Vp9HeaderSpec spec;
    spec.show_existing_frame = true;
    const std::vector<uint8_t> frame = BuildVp9Header(spec);
    const auto sh = Vp9BitstreamParser::ParseFromFrame(frame.data(), frame.size());
    EXPECT_FALSE(sh.present);
}

TEST(Vp9BitstreamParserTests, RejectsGarbage) {
    const std::vector<uint8_t> zeros(32, 0x00);
    EXPECT_FALSE(Vp9BitstreamParser::LooksLikeKeyFrame(zeros.data(), zeros.size()));
    EXPECT_FALSE(Vp9BitstreamParser::ParseFromFrame(zeros.data(), zeros.size()).present);

    // 同步码错一个字节就不该被当成关键帧
    std::vector<uint8_t> broken = BuildVp9Header(Vp9HeaderSpec{});
    ASSERT_GE(broken.size(), 3u);
    broken[2] = 0x00;
    EXPECT_FALSE(Vp9BitstreamParser::LooksLikeKeyFrame(broken.data(), broken.size()));

    // 太短的输入
    const std::vector<uint8_t> tiny{0x82, 0x49};
    EXPECT_FALSE(Vp9BitstreamParser::LooksLikeKeyFrame(tiny.data(), tiny.size()));
    EXPECT_FALSE(Vp9BitstreamParser::ParseFromFrame(nullptr, 0).present);
}

TEST(Vp9BitstreamParserTests, ColorSpaceToCicpMatchesFfmpeg) {
    int pri = 0, trc = 0, matrix = 0;
    // BT.709 -> pri/trc/matrix 全 1
    EXPECT_TRUE(Vp9BitstreamParser::ColorSpaceToCicp(2, pri, trc, matrix));
    EXPECT_EQ(pri, 1);
    EXPECT_EQ(trc, 1);
    EXPECT_EQ(matrix, 1);

    // BT.2020 NCL -> 9 / 16 / 9
    EXPECT_TRUE(Vp9BitstreamParser::ColorSpaceToCicp(5, pri, trc, matrix));
    EXPECT_EQ(pri, 9);
    EXPECT_EQ(trc, 16);
    EXPECT_EQ(matrix, 9);

    // sRGB -> 1 / 13 / 0
    EXPECT_TRUE(Vp9BitstreamParser::ColorSpaceToCicp(7, pri, trc, matrix));
    EXPECT_EQ(pri, 1);
    EXPECT_EQ(trc, 13);
    EXPECT_EQ(matrix, 0);

    // Unknown 不算"已指定"，调用方应跳过这项比对
    EXPECT_FALSE(Vp9BitstreamParser::ColorSpaceToCicp(0, pri, trc, matrix));
}

TEST(Vp9BitstreamParserTests, ComputeLevelByPictureSize) {
    // 1920x1080 = 2073600，落在 Level 4（<= 2228224）之前先撞上 5? 逐级查表：
    // 2.1(245760) < 2073600 <= 3(552960)? 否 → 4(2228224) 满足 → 40
    EXPECT_EQ(Vp9BitstreamParser::ComputeLevel(1920, 1080, 0.0), 40);
    // 3840x2160 = 8294400 → 5.0(8912896) 满足 → 50
    EXPECT_EQ(Vp9BitstreamParser::ComputeLevel(3840, 2160, 0.0), 50);
    // 640x360 = 230400 → 2.1 的 245760 满足 → 21
    EXPECT_EQ(Vp9BitstreamParser::ComputeLevel(640, 360, 0.0), 21);
    // 帧率会推高 level：1080p@60 = 124416000 → 4.1(160432128) 满足 → 41
    EXPECT_EQ(Vp9BitstreamParser::ComputeLevel(1920, 1080, 60.0), 41);
    // 非法尺寸
    EXPECT_EQ(Vp9BitstreamParser::ComputeLevel(0, 0, 30.0), 0);
}

TEST(Vp9BitstreamParserTests, SummaryText) {
    Vp9HeaderSpec spec;
    spec.profile = 2;
    spec.color_space = static_cast<int>(Vp9ColorSpace::Bt2020Ncl);
    spec.frame_width = 3840;
    spec.frame_height = 2160;
    const std::vector<uint8_t> frame = BuildVp9Header(spec);
    const auto sh = Vp9BitstreamParser::ParseFromFrame(frame.data(), frame.size());
    ASSERT_TRUE(sh.present);
    const std::string summary = sh.Summary();
    EXPECT_NE(summary.find("3840x2160"), std::string::npos);
    EXPECT_NE(summary.find("10bit"), std::string::npos);
    EXPECT_NE(summary.find("BT.2020 NCL"), std::string::npos);
}
