// LATM / LOAS 的帧计数：一个 AudioMuxElement 里有 numSubFrames+1 个 subFrame。
//
// 背景（2026-10）：StreamMuxConfig 里的 numSubFrames 一直解析出来了，但没有任何
// 消费者 —— 每个 LOAS 包固定只加 1 帧。多 subFrame 的流（长帧聚合，广播与低码率
// 场景常见）帧数会按倍数少算，而 frame_count 是"时长 / 平均码率 / 每秒帧数"的分子，
// 算错会直接进报告，且完全看不出来。
//
// 期望值来源：ECMA-391（LOAS/LATM）§1.7.3 StreamMuxConfig 语法。
// 包字节用下面的 BitWriter 按规范逐位拼出，不取自被测实现。
//
// 已知限制（本测试**不**覆盖，不要误以为已支持）：
//   一个 subFrame 跨多个 LOAS 包（分包重组）未实现。实现按"每个包自带完整
//   AudioMuxElement"处理，遇到分包的流会走到无法定界那条路上。

#include <gtest/gtest.h>

#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/analysis/quality/AudioBitstreamAnalyzer.h"

namespace {

using videoeye::AudioBitstreamAnalyzer;

// 按位拼规范字段用；与生产代码无关，纯粹是测试侧的编码器
class BitWriter {
public:
    void U(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i)
            bits_.push_back(((value >> i) & 1u) != 0);
    }
    std::vector<uint8_t> Bytes() const {
        std::vector<uint8_t> out;
        uint8_t cur = 0;
        int n = 0;
        for (bool b : bits_) {
            cur = static_cast<uint8_t>((cur << 1) | (b ? 1u : 0u));
            if (++n == 8) {
                out.push_back(cur);
                cur = 0;
                n = 0;
            }
        }
        if (n > 0)
            out.push_back(static_cast<uint8_t>(cur << (8 - n)));
        return out;
    }

private:
    std::vector<bool> bits_;
};

// AudioMuxElement()（ECMA-391 §1.7.3）：
//   useSameStreamMux 1
//   if (!useSameStreamMux) StreamMuxConfig():
//       audioMuxVersion 1 = 0
//       allStreamsSameTimeFraming 1 = 1
//       numSubFrames 6                      <- 本测试的主角
//       numProgram 4 = 0
//       numLayer 3 = 0
//       AudioSpecificConfig(): AOT=2(LC) sf=3(48kHz) cc=1(单声道)
//       frameLengthType 3 = 0
//       latmBufferFullness 8 = 0xFF
//       otherDataPresent 1 = 0
//       crcCheckPresent 1 = 0
//   之后是 numSubFrames+1 个 subFrame 的载荷（本测试用 0 填充，解析器不看）
//
// use_same_stream_mux=true 时只有一个字节（后续包沿用上一条 StreamMuxConfig）。
std::vector<uint8_t> MakeLoasPacket(int num_sub_frames, bool use_same_stream_mux) {
    BitWriter w;
    w.U(use_same_stream_mux ? 1u : 0u, 1);
    if (!use_same_stream_mux) {
        w.U(0, 1);                                     // audioMuxVersion
        w.U(1, 1);                                     // allStreamsSameTimeFraming
        w.U(static_cast<uint32_t>(num_sub_frames), 6); // numSubFrames
        w.U(0, 4);                                     // numProgram
        w.U(0, 3);                                     // numLayer
        w.U(2, 5);                                     // AOT = AAC LC
        w.U(3, 4);                                     // samplingFrequencyIndex = 48000
        w.U(1, 4);                                     // channelConfiguration = 1（单声道）
        w.U(0, 3);                                     // frameLengthType = 0
        w.U(0xFF, 8);                                  // latmBufferFullness
        w.U(0, 1);                                     // otherDataPresent
        w.U(0, 1);                                     // crcCheckPresent
    }
    std::vector<uint8_t> body = w.Bytes();
    // subFrame 载荷：解析器只读配置，不看载荷内容，但包里要有东西才算真实
    for (int i = 0; i < 8; ++i)
        body.push_back(0x00);

    // LOAS 头：syncword 11 bit（0x2B7）+ audioMuxLengthBytes 13 bit
    const size_t len = body.size();
    std::vector<uint8_t> pkt;
    pkt.push_back(0x56);
    pkt.push_back(static_cast<uint8_t>(0xE0 | ((len >> 8) & 0x1F)));
    pkt.push_back(static_cast<uint8_t>(len & 0xFF));
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

// 造一个只含一条 AAC-LATM 音轨的容器上下文，用来驱动 RegisterStreams。
//
// 这里刻意**不调用** avformat_alloc_output_context2 / avformat_new_stream /
// av_packet_alloc：这些是 FFmpeg 的导出函数，测试目标没有继承 core 那边的
// FFmpeg 编译定义，直接引用会变成裸符号引用而链不上（LNK2019）。
// AudioBitstreamAnalyzer 需要的只是几个结构体字段，栈上构造足够，
// 而且这样测试不持有任何需要释放的资源。
class LatmFixture {
public:
    LatmFixture() {
        par_.codec_type = AVMEDIA_TYPE_AUDIO;
        par_.codec_id = AV_CODEC_ID_AAC_LATM;
        par_.sample_rate = 48000;
        par_.ch_layout.nb_channels = 1;
        par_.ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;

        stream_.codecpar = &par_;
        stream_.time_base = AVRational{1, 48000};
        stream_index_[0] = &stream_;

        fmt_.nb_streams = 1;
        fmt_.streams = stream_index_;
        fmt_.iformat = &iformat_;

        analyzer_.Reset();
        analyzer_.RegisterStreams(&fmt_);
        ready_ = true;
    }

    bool ready() const {
        return ready_;
    }

    void Feed(const std::vector<uint8_t>& bytes) {
        // AVPacket 也是纯数据：OnPacket 只读 data/size/stream_index，
        // 不接管所有权，所以指向 vector 的缓冲区即可。
        AVPacket pkt{};
        pkt.data = const_cast<uint8_t*>(bytes.data());
        pkt.size = static_cast<int>(bytes.size());
        pkt.stream_index = 0;
        pkt.pts = 0;
        pkt.dts = 0;
        analyzer_.OnPacket(&pkt, &stream_);
    }

    const videoeye::model::AudioBitstreamResult& Finish() {
        return analyzer_.Finish();
    }

private:
    AVCodecParameters par_{};
    AVStream stream_{};
    AVStream* stream_index_[1] = {nullptr};
    AVInputFormat iformat_{};
    AVFormatContext fmt_{};
    AudioBitstreamAnalyzer analyzer_;
    bool ready_ = false;
};

} // namespace

// 正例：numSubFrames=3 表示这个 AudioMuxElement 里有 4 个 subFrame（4 个 AAC 帧）。
// 按"一包一帧"统计会少算 3 倍。
TEST(LatmPacketAccountingTest, NumSubFramesExpandsToFrameCount) {
    LatmFixture fx;
    ASSERT_TRUE(fx.ready());
    fx.Feed(MakeLoasPacket(/*num_sub_frames=*/3, /*use_same_stream_mux=*/false));

    const auto& result = fx.Finish();
    ASSERT_EQ(result.streams.size(), size_t(1));
    const auto& s = result.streams.front();
    EXPECT_EQ(s.parsed_packets, 1);
    EXPECT_EQ(s.frame_count, 4) << "numSubFrames=3 → 4 个 subFrame，每个是一帧";
    EXPECT_EQ(s.latm_num_sub_frames, 3);
    EXPECT_EQ(s.sample_rate, 48000);
    EXPECT_EQ(s.channels, 1);
}

// 正例（对照）：numSubFrames=0 是"一包一帧"的正常情形，不能被改坏
TEST(LatmPacketAccountingTest, ZeroSubFramesMeansOneFramePerPacket) {
    LatmFixture fx;
    ASSERT_TRUE(fx.ready());
    fx.Feed(MakeLoasPacket(/*num_sub_frames=*/0, /*use_same_stream_mux=*/false));

    const auto& result = fx.Finish();
    ASSERT_EQ(result.streams.size(), size_t(1));
    EXPECT_EQ(result.streams.front().frame_count, 1);
    EXPECT_EQ(result.streams.front().latm_num_sub_frames, -1)
        << "-1 表示「没有多 subFrame」，不是「numSubFrames=0」的另一种写法";
}

// useSameStreamMux=1 的后续包沿用上一条 StreamMuxConfig，其中也包括 numSubFrames。
// 只读第一个包的配置、后面按 1 帧算，会让整条流只有首包是对的。
TEST(LatmPacketAccountingTest, SubsequentPacketsReuseSubFrameCount) {
    LatmFixture fx;
    ASSERT_TRUE(fx.ready());
    fx.Feed(MakeLoasPacket(/*num_sub_frames=*/3, /*use_same_stream_mux=*/false));
    fx.Feed(MakeLoasPacket(/*num_sub_frames=*/0, /*use_same_stream_mux=*/true));
    fx.Feed(MakeLoasPacket(/*num_sub_frames=*/0, /*use_same_stream_mux=*/true));

    const auto& result = fx.Finish();
    ASSERT_EQ(result.streams.size(), size_t(1));
    const auto& s = result.streams.front();
    EXPECT_EQ(s.parsed_packets, 3);
    EXPECT_EQ(s.frame_count, 12) << "3 个包 × 4 帧；后两个包沿用首包的 numSubFrames";
}

// 反例：不是 LOAS 包（同步字不对）时不能算成 LATM 帧，要落到 invalid
TEST(LatmPacketAccountingTest, NonLoasPacketIsNotCountedAsFrames) {
    LatmFixture fx;
    ASSERT_TRUE(fx.ready());
    std::vector<uint8_t> garbage = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    fx.Feed(garbage);

    const auto& result = fx.Finish();
    ASSERT_EQ(result.streams.size(), size_t(1));
    const auto& s = result.streams.front();
    EXPECT_EQ(s.frame_count, 0);
    EXPECT_EQ(s.invalid_packets, 1);
}
