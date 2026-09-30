#include <gtest/gtest.h>

#include "core/analysis/stream/StreamAnalyzer.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

namespace {

class AVFormatContextGuard {
public:
    AVFormatContextGuard() : ctx_(avformat_alloc_context()) {}
    ~AVFormatContextGuard() { avformat_close_input(&ctx_); }

    AVFormatContext* get() { return ctx_; }

private:
    AVFormatContext* ctx_ = nullptr;
};

TEST(StreamAnalyzerTest, ClassifiesPacketsAndFramesByMediaType) {
    AVFormatContextGuard format;
    ASSERT_NE(format.get(), nullptr);

    AVStream* video = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(video, nullptr);
    ASSERT_NE(video->codecpar, nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    AVStream* audio = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(audio, nullptr);
    ASSERT_NE(audio->codecpar, nullptr);
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;

    videoeye::analyzer::StreamAnalyzer analyzer;
    analyzer.Start();

    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);

    packet->stream_index = static_cast<int>(video->index);
    packet->size = 1200;
    packet->flags = AV_PKT_FLAG_KEY;
    analyzer.AnalyzePacket(packet, format.get());
    analyzer.AnalyzeVideoFrame(AV_PICTURE_TYPE_I);

    packet->stream_index = static_cast<int>(audio->index);
    packet->size = 400;
    packet->flags = 0;
    analyzer.AnalyzePacket(packet, format.get());
    analyzer.AnalyzeAudioFrame();

    av_packet_free(&packet);

    const auto stats = analyzer.GetStats();
    EXPECT_EQ(stats.total_packets, 2);
    EXPECT_EQ(stats.video_packets, 1);
    EXPECT_EQ(stats.audio_packets, 1);
    EXPECT_EQ(stats.total_video_frames, 1);
    EXPECT_EQ(stats.total_audio_frames, 1);
    EXPECT_EQ(stats.i_frame_count, 1);
    EXPECT_EQ(stats.key_frame_count, 1);
    EXPECT_EQ(stats.current_gop_size, 1);
}

// GOP / 关键帧统计必须只认视频流的关键帧.
// 回归: 旧实现用了 "media_type == VIDEO || !format_ctx" 兜底,
// 在拿不到 format_ctx 时会把音频关键包也计入 GOP, 污染统计.
TEST(StreamAnalyzerTest, GopCountsOnlyVideoKeyframes) {
    AVFormatContextGuard format;
    ASSERT_NE(format.get(), nullptr);

    AVStream* video = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(video, nullptr);
    ASSERT_NE(video->codecpar, nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    AVStream* audio = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(audio, nullptr);
    ASSERT_NE(audio->codecpar, nullptr);
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;

    videoeye::analyzer::StreamAnalyzer analyzer;
    analyzer.Start();

    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);

    // 视频关键帧
    packet->stream_index = static_cast<int>(video->index);
    packet->size = 1200;
    packet->flags = AV_PKT_FLAG_KEY;
    analyzer.AnalyzePacket(packet, format.get());

    // 音频关键帧: 绝不能算进 GOP / 关键帧计数
    packet->stream_index = static_cast<int>(audio->index);
    packet->size = 400;
    packet->flags = AV_PKT_FLAG_KEY;
    analyzer.AnalyzePacket(packet, format.get());

    // 视频非关键帧
    packet->stream_index = static_cast<int>(video->index);
    packet->size = 1000;
    packet->flags = 0;
    analyzer.AnalyzePacket(packet, format.get());

    av_packet_free(&packet);

    const auto stats = analyzer.GetStats();
    EXPECT_EQ(stats.total_packets, 3);
    EXPECT_EQ(stats.video_packets, 2);
    EXPECT_EQ(stats.audio_packets, 1);
    // 关键帧 / GOP 只统计视频关键帧 (音频关键帧被忽略)
    EXPECT_EQ(stats.key_frame_count, 1);
    EXPECT_EQ(stats.current_gop_size, 2);  // 视频帧: 1 关键 + 1 非关键
    EXPECT_EQ(stats.max_gop_size, 0);       // 只出现一个 GOP, 尚未闭合
}

// Reset 必须清空所有累计统计与历史.
TEST(StreamAnalyzerTest, ResetClearsStats) {
    AVFormatContextGuard format;
    ASSERT_NE(format.get(), nullptr);
    AVStream* video = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(video, nullptr);
    ASSERT_NE(video->codecpar, nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    videoeye::analyzer::StreamAnalyzer analyzer;
    analyzer.Start();

    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);
    packet->stream_index = static_cast<int>(video->index);
    packet->size = 1200;
    packet->flags = AV_PKT_FLAG_KEY;
    analyzer.AnalyzePacket(packet, format.get());
    analyzer.AnalyzeVideoFrame(AV_PICTURE_TYPE_I);
    av_packet_free(&packet);

    ASSERT_EQ(analyzer.GetStats().total_packets, 1);
    ASSERT_EQ(analyzer.GetStats().key_frame_count, 1);

    analyzer.Reset();
    const auto stats = analyzer.GetStats();
    EXPECT_EQ(stats.total_packets, 0);
    EXPECT_EQ(stats.total_bytes, 0);
    EXPECT_EQ(stats.video_packets, 0);
    EXPECT_EQ(stats.key_frame_count, 0);
    EXPECT_EQ(stats.current_gop_size, 0);
    EXPECT_EQ(stats.total_video_frames, 0);
}

} // namespace
