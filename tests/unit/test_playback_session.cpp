// PlaybackSession 单元测试: 播放机械(状态机 / 资源生命周期)与 MediaPlayer 解耦后的行为。
//
// 这里只测"不需要真实媒体文件"的部分: 初始状态、没有媒体时的 Play/Seek、Stop 的位置归零、
// 资源交接与释放。解码循环本身需要真实文件与音频设备, 不放进 gtest。

#include <gtest/gtest.h>

#include "core/player/PlaybackSession.h"

namespace {

using videoeye::model::PlayerState;
using videoeye::model::SeekMode;
using videoeye::player::PlaybackSession;

} // namespace

TEST(PlaybackSessionTest, InitialStateIsIdleAndEmpty) {
    PlaybackSession session;
    EXPECT_EQ(session.state(), PlayerState::Idle);
    EXPECT_TRUE(session.format_ctx() == nullptr);
    EXPECT_TRUE(session.video_decoder() == nullptr);
    EXPECT_TRUE(session.audio_decoder() == nullptr);
    EXPECT_TRUE(session.audio_output() == nullptr);
    EXPECT_EQ(session.video_stream_index(), -1);
    EXPECT_EQ(session.audio_stream_index(), -1);
    EXPECT_EQ(session.current_position_ms(), 0);
    EXPECT_EQ(session.duration_ms(), 0);
    EXPECT_FALSE(session.IsRenderingSuppressed());
}

TEST(PlaybackSessionTest, PlayWithoutMediaIsRejected) {
    PlaybackSession session;
    EXPECT_FALSE(session.Play());
    EXPECT_EQ(session.state(), PlayerState::Idle);
}

TEST(PlaybackSessionTest, SeekWithoutMediaIsNoop) {
    PlaybackSession session;
    session.Seek(5000, SeekMode::ExactFrame);
    EXPECT_EQ(session.current_position_ms(), 0) << "没有打开媒体时不应改位置";
    EXPECT_EQ(session.state(), PlayerState::Idle);
}

TEST(PlaybackSessionTest, StopResetsPositionAndState) {
    PlaybackSession session;
    session.SetPosition(1234);
    session.Stop();
    EXPECT_EQ(session.state(), PlayerState::Stopped);
    EXPECT_EQ(session.current_position_ms(), 0);
}

TEST(PlaybackSessionTest, SetIdleTransitionsToIdle) {
    PlaybackSession session;
    session.Stop();
    ASSERT_EQ(session.state(), PlayerState::Stopped);
    session.SetIdle();
    EXPECT_EQ(session.state(), PlayerState::Idle) << "打开成功后应回到 Idle(可播放但未开始)";
}

TEST(PlaybackSessionTest, StreamIndicesAndDurationRoundTrip) {
    PlaybackSession session;
    session.SetStreamIndices(0, 2);
    EXPECT_EQ(session.video_stream_index(), 0);
    EXPECT_EQ(session.audio_stream_index(), 2);
    session.SetDuration(90000);
    EXPECT_EQ(session.duration_ms(), 90000);
    session.SetPosition(1500);
    EXPECT_EQ(session.current_position_ms(), 1500);
}

TEST(PlaybackSessionTest, RenderingSuppressionRoundTrip) {
    PlaybackSession session;
    session.SetRenderingSuppressed(true);
    EXPECT_TRUE(session.IsRenderingSuppressed());
    session.SetRenderingSuppressed(false);
    EXPECT_FALSE(session.IsRenderingSuppressed());
}

TEST(PlaybackSessionTest, ReleaseClearsOwnedResources) {
    PlaybackSession session;
    session.SetStreamIndices(1, 0);
    // 没有真实解码器可造, 这里只验证空状态下 Release() 安全且会复位流索引
    session.Release();
    EXPECT_TRUE(session.format_ctx() == nullptr);
    EXPECT_TRUE(session.video_decoder() == nullptr);
    EXPECT_TRUE(session.audio_decoder() == nullptr);
    EXPECT_TRUE(session.audio_output() == nullptr);
    EXPECT_EQ(session.video_stream_index(), -1);
    EXPECT_EQ(session.audio_stream_index(), -1);
}

TEST(PlaybackSessionTest, SetHooksWithoutMediaDoesNotInvokeThem) {
    PlaybackSession session;
    int calls = 0;
    PlaybackSession::Hooks hooks;
    hooks.on_packet = [&](const videoeye::player::PacketContext&) { ++calls; };
    hooks.on_video_frame = [&](const videoeye::player::VideoFrameContext&) { ++calls; };
    hooks.on_audio_frame = [&](const videoeye::player::AudioFrameContext&) { ++calls; };
    hooks.on_end_of_stream = [&]() { ++calls; };
    session.SetHooks(std::move(hooks));

    session.Play();
    session.Stop();
    EXPECT_EQ(calls, 0) << "没有媒体就不该有解码回调";
}

TEST(PlaybackSessionTest, DestructorIsSafeWithoutEverPlaying) {
    // 析构要能自停线程并释放资源: 这里没有起过线程, 只验证不崩不挂
    PlaybackSession session;
    session.SetStreamIndices(0, 1);
    session.SetPosition(500);
}
