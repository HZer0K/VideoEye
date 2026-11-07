// 裸流播放时间轴回归：Annex-B ES 的包 pts/dts 恒为 AV_NOPTS_VALUE，但仍带
// packet->duration（实测 h264 demuxer 在 1/1200000 时基下给 48000 = 40ms）。
//
// 没有时间基准时 PacketTimestampSeconds 返回 NaN、PlaybackClock::PaceTo 直接
// return —— 解码线程全速吐帧、进度条恒 0、A/V 同步采样失效。这里钉住两件事：
//   ① 合成器只作用于「从未给过真实时间戳」的流，绝不污染容器时间轴；
//   ② 合成后的时间戳确实能被换算成有限、单调的秒值（做了要真的有用）。
//
// 末尾用仓库里的真实裸 H.264 语料做端到端验证：能打开、确实缺时间戳、能解码出帧。

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include "core/player/PlaybackClock.h"

namespace {

using videoeye::player::PacketTimestampSeconds;
using videoeye::player::PlaybackClock;
using videoeye::player::RawStreamTimestampFiller;

constexpr std::int64_t kNoPts = AV_NOPTS_VALUE;
constexpr std::int64_t kFrameDuration = 48000; // 1/1200000 时基 → 40ms/帧

AVPacket MakePacket(int stream_index, std::int64_t pts, std::int64_t dts, std::int64_t duration) {
    AVPacket packet = {};
    packet.stream_index = stream_index;
    packet.pts = pts;
    packet.dts = dts;
    packet.duration = duration;
    return packet;
}

// 建一个只有一条流、时基与 h264 demuxer 一致的 format 上下文，供换算函数使用。
AVFormatContext* MakeFormatContext() {
    AVFormatContext* format = avformat_alloc_context();
    if (!format)
        return nullptr;
    AVStream* stream = avformat_new_stream(format, nullptr);
    if (stream)
        stream->time_base = AVRational{1, 1200000};
    return format;
}

// 同上，但带上帧率：一部分裸流 demuxer 不给 packet->duration，只能从流帧率换算。
AVFormatContext* MakeFormatContextWithFps(int num, int den) {
    AVFormatContext* format = MakeFormatContext();
    if (!format)
        return nullptr;
    if (format->nb_streams > 0)
        format->streams[0]->avg_frame_rate = AVRational{num, den};
    return format;
}

} // namespace

TEST(RawStreamTimestampFillerTest, AccumulatesDurationForStreamWithoutTimestamps) {
    RawStreamTimestampFiller filler;
    for (int i = 0; i < 3; ++i) {
        AVPacket packet = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
        EXPECT_TRUE(filler.Fill(&packet));
        EXPECT_EQ(packet.pts, static_cast<std::int64_t>(i) * kFrameDuration);
        EXPECT_EQ(packet.dts, packet.pts);
    }
    EXPECT_FALSE(filler.HasRealTimestamps(0));
}

TEST(RawStreamTimestampFillerTest, NeverOverridesRealTimestamps) {
    RawStreamTimestampFiller filler;

    AVPacket real = MakePacket(0, 1000, 900, kFrameDuration);
    EXPECT_FALSE(filler.Fill(&real));
    EXPECT_EQ(real.pts, 1000);
    EXPECT_EQ(real.dts, 900);
    EXPECT_TRUE(filler.HasRealTimestamps(0));

    // 后半段丢了时间戳也不能被合成值污染（B 帧重排 / edit list 依赖原时间轴）。
    AVPacket missing = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    EXPECT_FALSE(filler.Fill(&missing));
    EXPECT_EQ(missing.pts, kNoPts);
    EXPECT_EQ(missing.dts, kNoPts);
}

TEST(RawStreamTimestampFillerTest, SkipsPacketsWithoutUsableDuration) {
    RawStreamTimestampFiller filler;
    AVPacket zero = MakePacket(0, kNoPts, kNoPts, 0);
    EXPECT_FALSE(filler.Fill(&zero));
    EXPECT_EQ(zero.pts, kNoPts);

    AVPacket negative = MakePacket(0, kNoPts, kNoPts, -1);
    EXPECT_FALSE(filler.Fill(&negative));
    EXPECT_EQ(negative.pts, kNoPts);

    AVPacket bad_stream = MakePacket(-1, kNoPts, kNoPts, kFrameDuration);
    EXPECT_FALSE(filler.Fill(&bad_stream));
    EXPECT_FALSE(filler.Fill(nullptr));

    // 被跳过的包不能偷偷推进累加器。
    AVPacket first = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    EXPECT_TRUE(filler.Fill(&first));
    EXPECT_EQ(first.pts, 0);
}

// --- 帧率兜底：packet->duration 缺失时用流帧率换算 ---
//
// 不是所有裸流 demuxer 都给 packet duration。此时唯一还能用的线索是 AVStream 上
// 的 avg_frame_rate：换算成 tick 后与 duration 那条路径得到的值必须一致
// （1/1200000 时基 + 25fps = 48000 = 实测的 40ms/帧），否则两种来源会产出两套节奏。

TEST(RawStreamTimestampFillerTest, UsesFrameRateWhenPacketDurationMissing) {
    AVFormatContext* format = MakeFormatContextWithFps(25, 1);
    ASSERT_NE(format, nullptr);

    RawStreamTimestampFiller filler;
    for (int i = 0; i < 3; ++i) {
        AVPacket packet = MakePacket(0, kNoPts, kNoPts, 0);
        ASSERT_TRUE(filler.Fill(&packet, format)) << "第 " << i << " 帧没被补上时间戳";
        EXPECT_EQ(packet.pts, static_cast<std::int64_t>(i) * kFrameDuration);
    }
    avformat_free_context(format);
}

TEST(RawStreamTimestampFillerTest, PrefersPacketDurationOverFrameRate) {
    AVFormatContext* format = MakeFormatContextWithFps(25, 1);
    ASSERT_NE(format, nullptr);

    // 包自带的 duration 更贴近实际码流（能反映变帧率），不能被帧率覆盖。
    RawStreamTimestampFiller filler;
    AVPacket packet = MakePacket(0, kNoPts, kNoPts, 96000);
    ASSERT_TRUE(filler.Fill(&packet, format));
    EXPECT_EQ(packet.pts, 0);

    AVPacket next = MakePacket(0, kNoPts, kNoPts, 96000);
    ASSERT_TRUE(filler.Fill(&next, format));
    EXPECT_EQ(next.pts, 96000);
    avformat_free_context(format);
}

// 反向守护：拿不到帧率就不要合成 —— 宁可没有时间轴，也不要编一套假节奏出来。
TEST(RawStreamTimestampFillerTest, RefusesToGuessWithoutFrameRate) {
    AVFormatContext* format = MakeFormatContext(); // 帧率保持 0/0
    ASSERT_NE(format, nullptr);

    RawStreamTimestampFiller filler;
    AVPacket packet = MakePacket(0, kNoPts, kNoPts, 0);
    EXPECT_FALSE(filler.Fill(&packet, format));
    EXPECT_EQ(packet.pts, kNoPts);
    avformat_free_context(format);

    // 连上下文都没有时同样只能放弃。
    AVPacket no_ctx = MakePacket(0, kNoPts, kNoPts, 0);
    EXPECT_FALSE(filler.Fill(&no_ctx, nullptr));
    EXPECT_EQ(no_ctx.pts, kNoPts);
    // 被拒绝的包不能偷偷推进累加器。
    AVPacket first = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    ASSERT_TRUE(filler.Fill(&first));
    EXPECT_EQ(first.pts, 0);
}

TEST(RawStreamTimestampFillerTest, TracksStreamsIndependently) {
    RawStreamTimestampFiller filler;
    AVPacket video = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    AVPacket audio = MakePacket(1, kNoPts, kNoPts, 1024);
    EXPECT_TRUE(filler.Fill(&video));
    EXPECT_TRUE(filler.Fill(&audio));
    EXPECT_EQ(video.pts, 0);
    EXPECT_EQ(audio.pts, 0);

    AVPacket video2 = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    AVPacket audio2 = MakePacket(1, kNoPts, kNoPts, 1024);
    EXPECT_TRUE(filler.Fill(&video2));
    EXPECT_TRUE(filler.Fill(&audio2));
    EXPECT_EQ(video2.pts, kFrameDuration);
    EXPECT_EQ(audio2.pts, 1024);
}

TEST(RawStreamTimestampFillerTest, ResetRestartsAtZero) {
    RawStreamTimestampFiller filler;
    AVPacket first = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    ASSERT_TRUE(filler.Fill(&first));
    filler.Reset();
    EXPECT_FALSE(filler.HasRealTimestamps(0));

    AVPacket after_reset = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    EXPECT_TRUE(filler.Fill(&after_reset));
    EXPECT_EQ(after_reset.pts, 0);
}

// 反向守护：不经过合成器的裸流包，时间戳确实是 NaN —— 证明"快放"是真实缺陷，
// 而不是测试自己造出来的假问题。
TEST(RawStreamTimestampTest, UnfilledRawPacketsHaveNoTimestamp) {
    AVFormatContext* format = MakeFormatContext();
    ASSERT_NE(format, nullptr);
    AVPacket packet = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
    EXPECT_TRUE(std::isnan(PacketTimestampSeconds(format, &packet)));
    avformat_free_context(format);
}

// 修复效果：合成后的包能喂给播放时钟，得到有限且单调递增的秒值。
TEST(RawStreamTimestampTest, SynthesizedTimestampsDriveFiniteMonotonicPacing) {
    AVFormatContext* format = MakeFormatContext();
    ASSERT_NE(format, nullptr);
    RawStreamTimestampFiller filler;
    PlaybackClock clock;
    clock.Reset();

    double previous = -1.0;
    for (int i = 0; i < 3; ++i) {
        AVPacket packet = MakePacket(0, kNoPts, kNoPts, kFrameDuration);
        ASSERT_TRUE(filler.Fill(&packet));
        const double seconds = PacketTimestampSeconds(format, &packet);
        ASSERT_TRUE(std::isfinite(seconds)) << "第 " << i << " 帧仍没有可用时间戳";
        EXPECT_GT(seconds, previous);
        clock.Sync(seconds);
        previous = seconds;
    }
    EXPECT_TRUE(clock.started);
    EXPECT_NEAR(previous, 0.08, 1e-9); // 2 * 48000 / 1200000
    avformat_free_context(format);
}

// 接入守护：纯逻辑单测再全，也挡不住"实现了但没接进解码循环"——本项目踩过
// 这类坑。把三个接入点钉死，删掉任一行回归立刻红。
TEST(RawStreamPlaybackWiringTest, DecodeThreadWiresSyntheticTimestampsIntoPacing) {
#ifndef VIDEOEYE_SOURCE_DIR
    GTEST_SKIP() << "未定义源码根目录";
#else
    const std::string path = std::string(VIDEOEYE_SOURCE_DIR) + "/core/player/PlaybackSession.cpp";
    std::ifstream input(path);
    ASSERT_TRUE(input.good()) << "读不到 " << path;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("raw_ts_filler.Fill(packet, format_ctx_)"), std::string::npos)
        << "解码循环必须给裸流包补时间戳，并把上下文传进去以启用帧率兜底";
    EXPECT_NE(source.find("current_packet_ts_sec = packet_ts_sec"), std::string::npos)
        << "必须把当前包时间戳交给帧交付";
    EXPECT_NE(source.find("frame_ts = current_packet_ts_sec"), std::string::npos)
        << "解码帧没有 pts 时要用合成时间戳兜底，否则 PaceTo 仍不会节流";
#endif
}

// 端到端：仓库里的真实裸 H.264 语料能被 FFmpeg 打开并解码出画面，
// 但容器时长与包时间戳都缺失 —— 这正是需要合成时间轴的场景。
TEST(RawEsPlaybackTest, AnnexBCorpusOpensAndDecodesWithoutContainerTimestamps) {
#ifndef VIDEOEYE_CODEC_CORPUS_DIR
    GTEST_SKIP() << "未定义裸流语料目录";
#else
    const std::string path = std::string(VIDEOEYE_CODEC_CORPUS_DIR) + "/real_x264_192x108_hrd.h264";
    AVFormatContext* format = nullptr;
    ASSERT_EQ(avformat_open_input(&format, path.c_str(), nullptr, nullptr), 0)
        << "裸 H.264 应当能被 FFmpeg 自动探测（播放侧不指定 demuxer）";
    ASSERT_EQ(avformat_find_stream_info(format, nullptr), 0);
    ASSERT_GE(format->nb_streams, 1u);
    EXPECT_STREQ(format->iformat->name, "h264");
    EXPECT_EQ(format->streams[0]->codecpar->codec_id, AV_CODEC_ID_H264);
    EXPECT_EQ(format->duration, AV_NOPTS_VALUE) << "裸流没有容器时长，进度条总量只能靠估算";
    EXPECT_GT(format->streams[0]->codecpar->width, 0);

    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);
    ASSERT_GE(av_read_frame(format, packet), 0);
    EXPECT_EQ(packet->pts, AV_NOPTS_VALUE);
    EXPECT_EQ(packet->dts, AV_NOPTS_VALUE);
    EXPECT_GT(packet->duration, 0) << "没有 duration 就无法合成时间轴";

    RawStreamTimestampFiller filler;
    EXPECT_TRUE(filler.Fill(packet));
    EXPECT_TRUE(std::isfinite(PacketTimestampSeconds(format, packet)));

    // 帧率兜底必须与 demuxer 给的 duration 落在**同一个步长**上（都表示"一帧"）。
    // 两者不一致就意味着"带 duration 的裸流"与"不带 duration 的裸流"会以不同速度
    // 播放。用真实语料断言，而不是自己造一个 25fps 的上下文糊过去。
    //
    // 这条同时钉住了 avg_frame_rate 优先于 r_frame_rate：语料的 r_frame_rate 是
    // 50/1（SPS 里的 VUI 上限），照它换算会得到 24000 而不是 48000。
    const AVStream* video_stream = format->streams[0];
    ASSERT_GT(video_stream->avg_frame_rate.num, 0);
    RawStreamTimestampFiller fps_filler;
    AVPacket no_duration = {};
    no_duration.stream_index = 0;
    no_duration.pts = AV_NOPTS_VALUE;
    no_duration.dts = AV_NOPTS_VALUE;
    no_duration.duration = 0;
    AVPacket no_duration_next = no_duration;
    ASSERT_TRUE(fps_filler.Fill(&no_duration, format)) << "有帧率时应当能推出步长";
    ASSERT_TRUE(fps_filler.Fill(&no_duration_next, format));
    EXPECT_EQ(no_duration_next.pts - no_duration.pts, packet->duration)
        << "帧率换算的帧间隔与 demuxer 给的 packet duration 不一致";

    // 解码器能真正出帧，否则"能打开"只是探测成功、不等于能播放。
    const AVCodec* codec = avcodec_find_decoder(format->streams[0]->codecpar->codec_id);
    ASSERT_NE(codec, nullptr);
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    ASSERT_NE(decoder, nullptr);
    ASSERT_GE(avcodec_parameters_to_context(decoder, format->streams[0]->codecpar), 0);
    decoder->pkt_timebase = format->streams[0]->time_base;
    decoder->time_base = format->streams[0]->time_base;
    ASSERT_EQ(avcodec_open2(decoder, codec, nullptr), 0);

    ASSERT_EQ(avcodec_send_packet(decoder, packet), 0);
    AVFrame* frame = av_frame_alloc();
    ASSERT_NE(frame, nullptr);
    int rc = avcodec_receive_frame(decoder, frame);
    if (rc == AVERROR(EAGAIN)) {
        avcodec_send_packet(decoder, nullptr);
        rc = avcodec_receive_frame(decoder, frame);
    }
    EXPECT_GE(rc, 0) << "裸 H.264 应当能解码出至少一帧画面";
    if (rc >= 0) {
        EXPECT_GT(frame->width, 0);
        EXPECT_GT(frame->height, 0);
    }

    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    av_packet_free(&packet);
    avformat_close_input(&format);
#endif
}
