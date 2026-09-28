#include <gtest/gtest.h>

#include "core/player/AnalysisSession.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

namespace {

class AVFormatContextGuard {
public:
    AVFormatContextGuard() : ctx_(avformat_alloc_context()) {
    }
    ~AVFormatContextGuard() {
        avformat_close_input(&ctx_);
    }
    AVFormatContext* get() {
        return ctx_;
    }

private:
    AVFormatContext* ctx_ = nullptr;
};

TEST(AnalysisSessionTest, Defaults) {
    videoeye::player::AnalysisSession session;
    // 默认只有容器结构分析开启, 其余分析开关与硬件解码均关闭
    EXPECT_FALSE(session.IsAnalysisEnabled());
    EXPECT_FALSE(session.IsFrameTypeAnalysisEnabled());
    EXPECT_FALSE(session.IsAudioFrameAnalysisEnabled());
    EXPECT_FALSE(session.IsPacketAnalysisEnabled());
    EXPECT_FALSE(session.IsEventAnalysisEnabled());
    EXPECT_FALSE(session.IsSyncAnalysisEnabled());
    EXPECT_FALSE(session.IsTimelineAnalysisEnabled());
    EXPECT_TRUE(session.IsContainerStructureEnabled());
    EXPECT_FALSE(session.IsMacroblockAnalysisEnabled());
    EXPECT_FALSE(session.IsSceneChangeAnalysisEnabled());
    EXPECT_FALSE(session.IsVisualDefectAnalysisEnabled());
    EXPECT_FALSE(session.IsHardwareDecodingEnabled());
}

TEST(AnalysisSessionTest, SettersPropagateToGetters) {
    videoeye::player::AnalysisSession session;

    session.SetAnalysisEnabled(true);
    session.SetFrameTypeAnalysisEnabled(true);
    session.SetAudioFrameAnalysisEnabled(true);
    session.SetPacketAnalysisEnabled(true);
    session.SetEventAnalysisEnabled(true);
    session.SetSyncAnalysisEnabled(true);
    session.SetTimelineAnalysisEnabled(true);
    session.SetContainerStructureEnabled(false);
    session.SetMacroblockAnalysisEnabled(true);
    session.SetSceneChangeAnalysisEnabled(true);
    session.SetVisualDefectAnalysisEnabled(true);
    session.SetHardwareDecodingEnabled(true);

    EXPECT_TRUE(session.IsAnalysisEnabled());
    EXPECT_TRUE(session.IsFrameTypeAnalysisEnabled());
    EXPECT_TRUE(session.IsAudioFrameAnalysisEnabled());
    EXPECT_TRUE(session.IsPacketAnalysisEnabled());
    EXPECT_TRUE(session.IsEventAnalysisEnabled());
    EXPECT_TRUE(session.IsSyncAnalysisEnabled());
    EXPECT_TRUE(session.IsTimelineAnalysisEnabled());
    EXPECT_FALSE(session.IsContainerStructureEnabled());
    EXPECT_TRUE(session.IsMacroblockAnalysisEnabled());
    EXPECT_TRUE(session.IsSceneChangeAnalysisEnabled());
    EXPECT_TRUE(session.IsVisualDefectAnalysisEnabled());
    EXPECT_TRUE(session.IsHardwareDecodingEnabled());

    // 关回去也要生效 (单写多读一致性)
    session.SetVisualDefectAnalysisEnabled(false);
    EXPECT_FALSE(session.IsVisualDefectAnalysisEnabled());
}

TEST(AnalysisSessionTest, VisualDefectOptionsRoundTrip) {
    videoeye::player::AnalysisSession session;

    videoeye::analyzer::VisualDefectOptions opts;
    opts.preset = videoeye::analyzer::VisualSamplingPreset::Fine;
    opts.capture_rgb = false;
    session.SetVisualDefectOptions(opts);

    const auto got = session.GetVisualDefectOptions();
    EXPECT_EQ(got.preset, videoeye::analyzer::VisualSamplingPreset::Fine);
    EXPECT_FALSE(got.capture_rgb);
}

TEST(AnalysisSessionTest, StreamAnalyzerDelegation) {
    videoeye::player::AnalysisSession session;

    AVFormatContextGuard format;
    ASSERT_NE(format.get(), nullptr);
    AVStream* video = avformat_new_stream(format.get(), nullptr);
    ASSERT_NE(video, nullptr);
    ASSERT_NE(video->codecpar, nullptr);
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    session.Start();
    AVPacket* packet = av_packet_alloc();
    ASSERT_NE(packet, nullptr);
    packet->stream_index = static_cast<int>(video->index);
    packet->size = 1234;
    packet->flags = AV_PKT_FLAG_KEY;
    session.AnalyzePacket(packet, format.get());
    session.AnalyzeVideoFrame(AV_PICTURE_TYPE_I);
    av_packet_free(&packet);

    const auto stats = session.GetStats();
    EXPECT_EQ(stats.video_packets, 1);
    EXPECT_EQ(stats.total_video_frames, 1);
    EXPECT_EQ(stats.i_frame_count, 1);
}

} // namespace
