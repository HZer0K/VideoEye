#include <gtest/gtest.h>

#include "core/analyzer/StreamAnalyzer.h"

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

} // namespace
