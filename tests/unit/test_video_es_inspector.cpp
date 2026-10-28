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
            if (bits_.size() % 8 == 0)
                bytes_.push_back(0);
            bytes_.back() |= static_cast<uint8_t>(bit << (7 - (bits_.size() % 8)));
            bits_.push_back(bit != 0);
        }
    }
    void PutBool(bool v) {
        Put(v ? 1u : 0u, 1);
    }
    void PutUE(uint32_t n) {
        const uint32_t x = n + 1;
        int bits = 0;
        for (uint32_t t = x; t > 0; t >>= 1)
            ++bits;
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
        while (bits_.size() % 8 != 0)
            Put(0, 1);
    }
    std::vector<uint8_t> bytes() const {
        return bytes_;
    }

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
void AppendSeiMessage(std::vector<uint8_t>& rbsp, int payload_type, const std::vector<uint8_t>& payload) {
    rbsp.push_back(static_cast<uint8_t>(payload_type));
    rbsp.push_back(static_cast<uint8_t>(payload.size()));
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
}

// 一个"什么都不带的"最小 slice header（按下面 MakeSps/MakePps 的参数集布局）
//
// ❗ 这里必须**完整**写出 slice_header()（H.264 7.3.3）在 slice_qp_delta 之前的
// 每个语法元素，包括 dec_ref_pic_marking()。旧版本省掉了那两位 —— 恰好就是
// 实现漏读的那两位，于是测试永远绿、真实码流的 QP 永远错。
//
// 参数集固定为：frame_mbs_only_flag=1、pic_order_cnt_type=0、
// weighted_pred_flag=0、weighted_bipred_idc=0、entropy_coding_mode_flag=0。
std::vector<uint8_t> MakeSliceHeader(int slice_type, int slice_qp_delta, bool idr, int nal_ref_idc = 3,
                                     int first_mb_in_slice = 0, int l0_mod_entries = 0, int l1_mod_entries = 0) {
    const int t = slice_type % 5; // 0=P 1=B 2=I 3=SP 4=SI（5..9 是同一套的变体）
    BitWriter w;
    w.PutUE(static_cast<uint32_t>(first_mb_in_slice)); // first_mb_in_slice
    w.PutUE(static_cast<uint32_t>(slice_type));        // slice_type
    w.PutUE(0);                                        // pic_parameter_set_id
    w.Put(0, 4);                                       // frame_num（log2_max_frame_num_minus4 = 0）
    if (idr)
        w.PutUE(0); // idr_pic_id
    w.Put(0, 4);    // pic_order_cnt_lsb（minus4 = 0）
    if (t == 1)
        w.PutBool(false); // direct_spatial_mv_pred_flag
    if (t == 0 || t == 1 || t == 3) {
        w.PutBool(false); // num_ref_idx_active_override_flag（用 PPS 默认值）
    }
    // ref_pic_list_modification()：每个 idc 后面**必须**再跟一个 ue(v) 操作数
    // （idc 0/1 = abs_diff_pic_num_minus1、idc 2 = long_term_pic_num、3 = 结束）。
    // 测试按规范逐位写全，才能钉住实现有没有漏读。
    //
    // ❗ 操作数刻意取 5..7（都 > 3）：只读 idc 的实现在这里会读到"idc = 5"，
    // 直接越界失败，而不是靠数值巧合把位对齐 —— 否则一两条修改项时新旧实现
    // 消耗的位数可能刚好相同，测试就成了永远绿的摆设。
    const auto put_modification = [&w](int entries) {
        for (int i = 0; i < entries; ++i) {
            w.PutUE(static_cast<uint32_t>(i % 3)); // 三种 idc 都覆盖一遍
            w.PutUE(static_cast<uint32_t>(5 + i)); // 对应的操作数
        }
        w.PutUE(3); // modification_of_pic_nums_idc = 3 收尾
    };
    if (t != 2 && t != 4) {
        if (l0_mod_entries > 0) {
            w.PutBool(true); // ref_pic_list_modification_flag_l0
            put_modification(l0_mod_entries);
        } else {
            w.PutBool(false); // ref_pic_list_modification_flag_l0
        }
    }
    if (t == 1) {
        if (l1_mod_entries > 0) {
            w.PutBool(true); // ref_pic_list_modification_flag_l1
            put_modification(l1_mod_entries);
        } else {
            w.PutBool(false); // ref_pic_list_modification_flag_l1
        }
    }
    // pred_weight_table()：weighted_pred_flag=0 且 weighted_bipred_idc=0 → 不存在
    if (nal_ref_idc != 0) { // dec_ref_pic_marking()
        if (idr) {
            w.PutBool(false); // no_output_of_prior_pics_flag
            w.PutBool(false); // long_term_reference_flag
        } else {
            w.PutBool(false); // adaptive_ref_pic_marking_mode_flag
        }
    }
    // cabac_init_idc：entropy_coding_mode_flag=0 → 不存在
    w.PutSE(slice_qp_delta);
    w.Put(1, 1); // rbsp_trailing_bits 的 stop bit
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
    u16(34000);
    u16(16000);
    u16(13250);
    u16(34500);
    u16(7500);
    u16(3000);
    u16(15635);
    u16(16450);    // white point D65
    u32(10000000); // max = 1000 nits（单位 0.0001）
    u32(50);       // min = 0.005 nits
    return p;
}

} // namespace

// ==========================================================================
// H.264：SEI（MDCV / CLL / film grain）+ slice QP
// ==========================================================================

TEST(VideoEsInspectorTests, H264SeiMasteringDisplayAndContentLight) {
    // NAL type 6 = SEI, nal_ref_idc = 0
    std::vector<uint8_t> sei_rbsp;
    AppendSeiMessage(sei_rbsp, 137, MakeMdcvPayload());
    AppendSeiMessage(sei_rbsp, 144, {0x03, 0xE8, 0x01, 0x90}); // MaxCLL=1000 MaxFALL=400
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
    EXPECT_NEAR(r.sei.display_primaries_x[0], 0.68, 0.0001); // G
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

TEST(VideoEsInspectorTests, H264IdrSliceQpMatchesSpec) {
    // 规范写法（含 dec_ref_pic_marking 两位）+ delta=+1 → QP = 26 + 0 + 1 = 27。
    // 旧实现漏读那两位，读出来的 QP 是 31 且 qp_valid=true —— 一个"看着合理"的错值。
    const std::vector<uint8_t> nal = MakeAnnexB(0x65, MakeSliceHeader(2, 1, /*idr=*/true));

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(nal.data(), nal.size());
    inspector.Finish();

    const auto& s = inspector.result().slices;
    ASSERT_TRUE(s.qp_valid);
    EXPECT_EQ(s.qp_min, 27);
    EXPECT_EQ(s.qp_max, 27);
}

TEST(VideoEsInspectorTests, H264SliceTypeVariantIsNormalized) {
    // x264 的全 I 帧写 slice_type=7（5..9 是 0..4 的"整帧同类型"变体）。
    // 不归一化的话既判不出 I slice，后面的语法分支也全走错 → 整条 slice 被丢弃。
    const std::vector<uint8_t> data = MakeAnnexB(0x65, MakeSliceHeader(7, 0, /*idr=*/true));

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(data.data(), data.size());
    inspector.Finish();

    const auto& s = inspector.result().slices;
    ASSERT_TRUE(s.analyzed);
    EXPECT_EQ(s.slice_count, 1);
    EXPECT_EQ(s.type_i, 1);
    EXPECT_EQ(s.type_other, 0);
    ASSERT_TRUE(s.qp_valid);
    EXPECT_EQ(s.qp_min, 26);
}

TEST(VideoEsInspectorTests, H264PSliceHasItsOwnSyntax) {
    // P slice 比 I 多 num_ref_idx_active_override_flag 与 ref_pic_list_modification，
    // 少 idr_pic_id。漏掉任何一项都会从那里开始错位。
    const std::vector<uint8_t> nal = MakeAnnexB(0x41, MakeSliceHeader(0, 2, /*idr=*/false));

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(nal.data(), nal.size());
    inspector.Finish();

    const auto& s = inspector.result().slices;
    ASSERT_TRUE(s.analyzed);
    EXPECT_EQ(s.type_p, 1);
    EXPECT_EQ(s.type_i, 0);
    ASSERT_TRUE(s.qp_valid);
    EXPECT_EQ(s.qp_min, 28);
}

TEST(VideoEsInspectorTests, H264RefPicListModificationOperandsAreConsumed) {
    // ⚠️ 回归：ref_pic_list_modification() 里每个 modification_of_pic_nums_idc 后面
    // 都跟着一个 ue(v) 操作数（0/1 → abs_diff_pic_num_minus1、2 → long_term_pic_num）。
    // 旧实现只读 idc、不读操作数，于是 pred_weight_table / dec_ref_pic_marking /
    // slice_qp_delta 全部按错误偏移读 —— 实测同一条 QP=30 的 slice 给出 QP=28，
    // 而且 qp_valid 仍然是 true（错得很"有效"）。
    //
    // 下面三条 slice 的 QP 目标值完全一致（26 + delta 4 = 30），差别只在
    // 修改列表是否打开、有几个条目。只要实现漏读一位，QP 就会对不上。
    struct Case {
        int l0_entries;
        int l1_entries;
        int slice_type; // 0 = P, 1 = B
    };
    const Case cases[] = {{0, 0, 0}, {1, 0, 0}, {3, 0, 0}, {2, 2, 1}, {0, 0, 1}};
    for (const Case& c : cases) {
        const std::vector<uint8_t> nal =
            MakeAnnexB(0x41, MakeSliceHeader(c.slice_type, 4, /*idr=*/false, /*nal_ref_idc=*/3,
                                             /*first_mb_in_slice=*/0, c.l0_entries, c.l1_entries));

        VideoEsInspector inspector;
        inspector.Reset(AV_CODEC_ID_H264);
        inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
        inspector.Feed(nal.data(), nal.size());
        inspector.Finish();

        const auto& s = inspector.result().slices;
        ASSERT_TRUE(s.analyzed) << "slice_type=" << c.slice_type << " l0=" << c.l0_entries << " l1=" << c.l1_entries;
        ASSERT_TRUE(s.qp_valid) << "slice_type=" << c.slice_type << " l0=" << c.l0_entries << " l1=" << c.l1_entries;
        EXPECT_EQ(s.qp_min, 30) << "slice_type=" << c.slice_type << " l0=" << c.l0_entries << " l1=" << c.l1_entries;
    }
}

TEST(VideoEsInspectorTests, H264NonReferenceSliceSkipsDecRefPicMarking) {
    // nal_ref_idc == 0 → dec_ref_pic_marking() 整段不存在。
    // 无条件按"有"去读，非参考帧就会少读 1 位（IDR 分支则是多读 2 位）。
    const std::vector<uint8_t> nal = MakeAnnexB(0x01, MakeSliceHeader(0, 3, /*idr=*/false,
                                                                      /*nal_ref_idc=*/0));

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(nal.data(), nal.size());
    inspector.Finish();

    const auto& s = inspector.result().slices;
    ASSERT_TRUE(s.analyzed);
    ASSERT_TRUE(s.qp_valid);
    EXPECT_EQ(s.qp_min, 29);
}

TEST(VideoEsInspectorTests, H264FramesCountsPicturesNotSlices) {
    // 同一帧的两个 slice（first_mb_in_slice = 0 与 5）：slice_count=2，frames=1。
    // 旧实现 frames = slice_count，一帧多 slice 的流帧数会翻倍。
    std::vector<uint8_t> data;
    for (int first_mb : {0, 5}) {
        const std::vector<uint8_t> nal = MakeAnnexB(0x65, MakeSliceHeader(2, 0, /*idr=*/true, 3, first_mb));
        data.insert(data.end(), nal.begin(), nal.end());
    }

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(data.data(), data.size());
    inspector.Finish();

    const auto& s = inspector.result().slices;
    EXPECT_EQ(s.slice_count, 2);
    EXPECT_EQ(s.frames, 1);
    EXPECT_EQ(s.qp_parsed_slices, 2);
}

TEST(VideoEsInspectorTests, H264LengthPrefixedPacketIsParsed) {
    // MP4/MOV 的媒体包是 `[4 字节长度][NAL]`，一个起始码都没有。
    // 只按 Annex B 抽的话结果是零个 NAL —— 外观上和"没有 slice"一模一样。
    const std::vector<uint8_t> rbsp = MakeSliceHeader(2, 0, /*idr=*/true);
    std::vector<uint8_t> nal{0x65};
    nal.insert(nal.end(), rbsp.begin(), rbsp.end());

    std::vector<uint8_t> packet;
    const uint32_t len = static_cast<uint32_t>(nal.size());
    packet.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    packet.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    packet.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    packet.push_back(static_cast<uint8_t>(len & 0xFF));
    packet.insert(packet.end(), nal.begin(), nal.end());

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetNalFraming(VideoEsInspector::NalFraming::LengthPrefixed, 4);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(packet.data(), packet.size());
    inspector.Finish();

    const auto& s = inspector.result().slices;
    ASSERT_TRUE(s.analyzed);
    EXPECT_EQ(s.slice_count, 1);
    ASSERT_TRUE(s.qp_valid);
    EXPECT_EQ(s.qp_min, 26);
}

TEST(VideoEsInspectorTests, H264AutoFramingDetectsLengthPrefix) {
    // 没显式告诉封装方式时（Auto）也必须能认出长度前缀包
    const std::vector<uint8_t> rbsp = MakeSliceHeader(2, 1, /*idr=*/true);
    std::vector<uint8_t> nal{0x65};
    nal.insert(nal.end(), rbsp.begin(), rbsp.end());

    std::vector<uint8_t> packet{0x00, 0x00, 0x00, static_cast<uint8_t>(nal.size())};
    packet.insert(packet.end(), nal.begin(), nal.end());

    VideoEsInspector inspector;
    inspector.Reset(AV_CODEC_ID_H264);
    inspector.SetH264ParameterSets(MakeSps(), MakePps(0));
    inspector.Feed(packet.data(), packet.size());
    inspector.Finish();

    ASSERT_TRUE(inspector.result().slices.analyzed);
    EXPECT_EQ(inspector.result().slices.qp_min, 27);
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
    std::vector<uint8_t> hevc_nal{0x00,
                                  0x00,
                                  0x00,
                                  0x01,
                                  static_cast<uint8_t>(62 << 1), // nal_unit_type = 62
                                  0x01,                          // nuh_temporal_id_plus1 = 1
                                  0x19,
                                  0x01,
                                  0x02,
                                  0x03};

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
