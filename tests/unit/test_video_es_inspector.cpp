#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "core/analysis/codec/VideoEsInspector.h"

// codec_id 直接就是 AVCodecID（与生产代码同一口径），所以测试也要看得见枚举。
// FFmpeg 头必须包在 extern "C" 里：本仓库的预编译包没有自带 extern "C"，
// 不包的话编译过、链接爆 LNK2019（见 BitstreamAnalyzer.cpp 里的同款注释）。
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using videoeye::VideoEsInspector;
using videoeye::model::H264PpsInfo;
using videoeye::model::H264SpsInfo;

// ---- 逐位组装（MSB-first），测试数据不参考本项目 parser 的实现 ----
class BitWriter {
public:
    void Put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            const uint32_t bit = (value >> i) & 1u;
            if (bits_.size() % 8 == 0) bytes_.push_back(0);
            bytes_.back() |= static_cast<uint8_t>(bit << (7 - (bits_.size() % 8)));
            bits_.push_back(bit != 0);
        }
    }
    void PutBool(bool v) { Put(v ? 1u : 0u, 1); }
    void PutUE(uint32_t n) {
        const uint32_t x = n + 1;
        int bits = 0;
        for (uint32_t t = x; t > 0; t >>= 1) ++bits;
        Put(0, bits - 1);
        Put(x, bits);
    }
    void PutSE(int v) {
        if (v <= 0) {
            PutUE(static_cast<uint32_t>(-2 * v));
        } else {
            PutUE(static_cast<uint32_t>(2 * v - 1));
        }
    }
    void AlignByte() {
        while (bits_.size() % 8 != 0) Put(0, 1);
    }
    std::vector<uint8_t> bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<bool> bits_;
};

// RBSP 的防竞争转义：00 00 之后插一个 03（与真实码流一致；解析侧的
// UnescapeRbsp 会把它去掉，所以这里必须真的插）
std::vector<uint8_t> EscapeRbsp(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    int zeros = 0;
    for (uint8_t b : in) {
        if (zeros >= 2 && b <= 0x03) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }
    return out;
}

// 包成一个 AnnexB NAL：起始码(4B) + NAL header(1B) + 转义后的 RBSP
std::vector<uint8_t> MakeAnnexB(uint8_t nal_header, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out{0x00, 0x00, 0x00, 0x01, nal_header};
    const std::vector<uint8_t> escaped = EscapeRbsp(rbsp);
    out.insert(out.end(), escaped.begin(), escaped.end());
    return out;
}

// SEI message：payloadType / payloadSize 都是 0xFF 累加段
void AppendSeiMessage(std::vector<uint8_t>& rbsp, int payload_type,
                      const std::vector<uint8_t>& payload) {
    rbsp.push_back(static_cast<uint8_t>(payload_type));
    rbsp.push_back(static_cast<uint8_t>(payload.size()));
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
}

// 一个"什么都不带的"最小 slice header（按下面 MakeSps/MakePps 的参数集布局）
std::vector<uint8_t> MakeSliceHeader(int slice_type, int slice_qp_delta, bool idr) {
    BitWriter w;
    w.PutUE(0);                  // first_mb_in_slice
    w.PutUE(static_cast<uint32_t>(slice_type));
    w.PutUE(0);                  // pic_parameter_set_id
    w.Put(0, 4);                 // frame_num（log2_max_frame_num_minus4 = 0）
    if (idr) w.PutUE(0);         // idr_pic_id
    w.Put(0, 4);                 // pic_order_cnt_lsb（log2_max_poc_lsb_minus4 = 0）
    w.PutSE(slice_qp_delta);
    w.Put(1, 1);                 // rbsp_trailing_bits 的 stop bit
    w.AlignByte();
    return w.bytes();
}

// 与 MakeSliceHeader 的布局一一对应的参数集（直接填结构，不走字节解析）
H264SpsInfo MakeSps() {
    H264SpsInfo sps;
    sps.present = true;
    sps.profile_idc = 100;
    sps.chroma_format_idc = 1;
    sps.separate_colour_plane_flag = false;
    sps.log2_max_frame_num_minus4 = 0;
    sps.pic_order_cnt_type = 0;
    sps.log2_max_pic_order_cnt_lsb_minus4 = 0;
    sps.frame_mbs_only_flag = true;
    return sps;
}

H264PpsInfo MakePps(int init_qp_minus26) {
    H264PpsInfo pps;
    pps.present = true;
    pps.pic_parameter_set_id = 0;
    pps.bottom_field_pic_order_in_frame_present_flag = false;
    pps.redundant_pic_cnt_present_flag = 0;
    pps.pic_init_qp_minus26 = init_qp_minus26;
    return pps;
}

// MDCV（payloadType 137）：primaries / white point 各 u(16)，luminance u(32)
std::vector<uint8_t> MakeMdcvPayload() {
    std::vector<uint8_t> p;
    auto u16 = [&](uint16_t v) {
        p.push_back(static_cast<uint8_t>(v >> 8));
        p.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto u32 = [&](uint32_t v) {
        p.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        p.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        p.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        p.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    // G(0.68, 0.32) B(0.265, 0.69) R(0.15, 0.06)，单位 0.00002
    u16(34000); u16(16000);
    u16(13250); u16(34500);
    u16(7500);  u16(3000);
    u16(15635); u16(16450);   // white point D65
    u32(10000000);            // max = 1000 nits（单位 0.0001）
    u32(50);                  // min = 0.005 nits
    return p;
}

}  // namespace

// ==========================================================================
// H.264：SEI（MDCV / CLL / film grain）+ slice QP
// ==========================================================================

TEST(VideoEsInspectorTests, H264SeiMasteringDisplayAndContentLight) {
    // NAL type 6 = SEI, nal_ref_idc = 0
    std::vector<uint8_t> sei_rbsp;
    AppendSeiMessage(sei_rbsp, 137, MakeMdcvPayload());
    AppendSeiMessage(sei_rbsp, 144, {0x03, 0xE8, 0x01, 0x90});  // MaxCLL=1000 MaxFALL=400
    const std::vector<uint8_t> sei_nal = MakeAnnexB(0x06, sei_rbsp);

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    ASSERT_TRUE(inspector.enabled());
    ASSERT_TRUE(inspector.Feed(sei_nal.data(), sei_nal.size()));
    inspector.Finish();

    const auto& r = inspector.result();
    ASSERT_TRUE(r.analyzed);
    ASSERT_TRUE(r.sei.has_mastering_display);
    // 10000000 * 0.0001 = 1000 nits
    EXPECT_NEAR(r.sei.max_display_mastering_luminance, 1000.0, 0.01);
    EXPECT_NEAR(r.sei.min_display_mastering_luminance, 0.005, 0.0001);
    EXPECT_NEAR(r.sei.display_primaries_x[0], 0.68, 0.0001);   // G
    EXPECT_NEAR(r.sei.white_point_x, 0.3127, 0.0001);

    ASSERT_TRUE(r.sei.has_content_light_level);
    EXPECT_EQ(r.sei.max_content_light_level, 1000u);
    EXPECT_EQ(r.sei.max_pic_average_light_level, 400u);
    EXPECT_EQ(r.sei.total_messages, 2);
}

TEST(VideoEsInspectorTests, H264SeiHdr10PlusViaT35) {
    // payloadType 4 = user_data_registered_itu_t_t35：
    // country_code=0xB5 (US) + terminal_provider_code=0x003B (SMPTE)
    // + provider_oriented_code=0x8000 (ST 2094-40) + 应用标识
    std::vector<uint8_t> t35{0xB5, 0x00, 0x3B, 0x80, 0x00, 0x04, 0x01, 0x00};
    std::vector<uint8_t> sei_rbsp;
    AppendSeiMessage(sei_rbsp, 4, t35);
    const std::vector<uint8_t> sei_nal = MakeAnnexB(0x06, sei_rbsp);

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    ASSERT_TRUE(inspector.Feed(sei_nal.data(), sei_nal.size()));
    inspector.Finish();

    EXPECT_TRUE(inspector.result().sei.has_hdr10plus);
    EXPECT_EQ(inspector.result().sei.hdr10plus_count, 1);
}

TEST(VideoEsInspectorTests, H264SeiFilmGrain) {
    // payloadType 19 的 film_grain_characteristics：首 bit 是 cancel flag
    std::vector<uint8_t> sei_rbsp;
    AppendSeiMessage(sei_rbsp, 19, {0x80});
    const std::vector<uint8_t> sei_nal = MakeAnnexB(0x06, sei_rbsp);

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    ASSERT_TRUE(inspector.Feed(sei_nal.data(), sei_nal.size()));
    inspector.Finish();

    const auto& sei = inspector.result().sei;
    EXPECT_TRUE(sei.has_film_grain);
    EXPECT_TRUE(sei.film_grain_cancel_flag);
    EXPECT_EQ(sei.film_grain_count, 1);
}

TEST(VideoEsInspectorTests, H264SliceQpNeedsPpsBaseline) {
    // 三条 I slice，delta 分别 0 / +5 / +10 → QP 26 / 31 / 36（PPS init_qp = 0）
    std::vector<uint8_t> data;
    for (int delta : {0, 5, 10}) {
        const std::vector<uint8_t> nal = MakeAnnexB(0x65, MakeSliceHeader(2, delta, true));
        data.insert(data.end(), nal.begin(), nal.end());
    }

    // 先不给参数集：slice 位宽无解，QP 一律判无效（不给假数）
    {
        VideoEsInspector inspector;
        inspector.Reset(AV_CODEC_ID_H264);
        inspector.Feed(data.data(), data.size());
        inspector.Finish();
        EXPECT_FALSE(inspector.result().slices.analyzed);
        EXPECT_FALSE(inspector.result().slices.qp_valid);
    }

    // 给了参数集：QP 才是真的
    {
        VideoEsInspector inspector;
        inspector.Reset(AV_CODEC_ID_H264);
        inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
        inspector.Feed(data.data(), data.size());
        inspector.Finish();

        const auto& s = inspector.result().slices;
        ASSERT_TRUE(s.analyzed);
        EXPECT_TRUE(s.supported);
        EXPECT_EQ(s.slice_count, 3);
        EXPECT_EQ(s.type_i, 3);
        EXPECT_EQ(s.type_p, 0);
        EXPECT_EQ(s.type_b, 0);
        ASSERT_TRUE(s.qp_valid);
        EXPECT_EQ(s.qp_min, 26);
        EXPECT_EQ(s.qp_max, 36);
        EXPECT_NEAR(s.qp_avg, 31.0, 0.001);
        EXPECT_EQ(s.qp_curve.size(), 3u);
    }
}

TEST(VideoEsInspectorTests, H264SliceQpUsesPpsInitQp) {
    // PPS 的 pic_init_qp_minus26 = -4 → 基准从 26 变成 22
    const std::vector<uint8_t> nal = MakeAnnexB(0x65, MakeSliceHeader(2, 0, true));

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(-4));
    inspector.Feed(nal.data(), nal.size());
    inspector.Finish();

    ASSERT_TRUE(inspector.result().slices.qp_valid);
    EXPECT_EQ(inspector.result().slices.qp_min, 22);
}

TEST(VideoEsInspectorTests, H264OutOfRangeQpIsRejected) {
    // delta 大到把 QP 顶出 [0,51]：错位不会报错，只能靠取值范围拦
    const std::vector<uint8_t> nal = MakeAnnexB(0x65, MakeSliceHeader(2, 100, true));

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(nal.data(), nal.size());
    inspector.Finish();

    EXPECT_FALSE(inspector.result().slices.analyzed);
}

// ==========================================================================
// HEVC：DV RPU 识别；slice QP 明确不支持
// ==========================================================================

TEST(VideoEsInspectorTests, HevcDolbyVisionRpuNal62) {
    // HEVC NAL type 62（unspecified）里的 RPU payload 首字节恒为 0x19。
    //
    // ❗ HEVC 的 NAL header 是 **2 字节**，且 nal_unit_type 取 byte0 的 bit6..1
    // （(byte0 >> 1) & 0x3F）—— 不是 H.264 那种「低 5 位就是 type」。
    // 把 type 写进第二个字节（或写成 62 << 3）在这里都解不出 62。
    std::vector<uint8_t> hevc_nal{0x00, 0x00, 0x00, 0x01,
                                  static_cast<uint8_t>(62 << 1),  // nal_unit_type = 62
                                  0x01,                           // nuh_temporal_id_plus1 = 1
                                  0x19, 0x01, 0x02, 0x03};

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_HEVC);
    ASSERT_TRUE(inspector.enabled());
    inspector.Feed(hevc_nal.data(), hevc_nal.size());
    inspector.Finish();

    EXPECT_TRUE(inspector.result().sei.has_dolby_vision_rpu);
    EXPECT_EQ(inspector.result().sei.dolby_vision_rpu_count, 1);
}

TEST(VideoEsInspectorTests, HevcSliceQpIsNotClaimed) {
    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_HEVC);
    inspector.Finish();
    // 不支持要明说，而不是给一张全是 0 的表
    EXPECT_FALSE(inspector.result().slices.supported);
    EXPECT_FALSE(inspector.result().slices.unsupported_reason.empty());
}

TEST(VideoEsInspectorTests, UnsupportedCodecIsDisabled) {
    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_MPEG2VIDEO);
    EXPECT_FALSE(inspector.enabled());
    const std::vector<uint8_t> some{0x00, 0x01, 0x02};
    EXPECT_FALSE(inspector.Feed(some.data(), some.size()));
}

TEST(VideoEsInspectorTests, PacketBudgetStopsTheScan) {
    VideoEsInspector::Options options;
    options.max_packets = 2;
    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264, options);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));

    const std::vector<uint8_t> nal = MakeAnnexB(0x65, MakeSliceHeader(2, 0, true));
    EXPECT_TRUE(inspector.Feed(nal.data(), nal.size()));
    EXPECT_TRUE(inspector.Feed(nal.data(), nal.size()));
    // 第三次已经到上限：返回 false 且标记截断
    EXPECT_FALSE(inspector.Feed(nal.data(), nal.size()));
    inspector.Finish();
    EXPECT_TRUE(inspector.result().truncated);
    EXPECT_EQ(inspector.result().packets_scanned, 2);
}
