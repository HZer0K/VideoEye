// 动图循环 / 重播 / 静图不循环 —— 切片 C 的三条行为契约。
//
// ① 循环开启：播到结尾回到起点继续，**不发** PlaybackFinished（画面不清、进度条回 0）
// ② 循环关闭（默认）：播一遍就结束，PlaybackFinished 恰好一次
// ③ 静图（image2 + PNG）：is_animated_image 必须为 false；即使强行打开循环开关，
//    引擎也必须拒绝 —— 单帧循环等于反复解码同一帧，是纯空转
// ④ RestartFromStart：播完后再播，帧数必须重新走一遍（动图重播走这条路，
//    因为它不重新打开文件，靠 seek 回起点）
//
// 语料用仓库自带的 gif / png 编码器现场生成（见 TestMediaFactory），不往版本库塞二进制。
// 这里会真的跑解码线程：图片没有音频轨，不碰声卡，所以可以在 gtest 里跑。

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "core/player/OpenController.h"
#include "core/player/PlaybackSession.h"
#include "tests/support/TestMediaFactory.h"

namespace {

using namespace videoeye;
using namespace videoeye::player; // PlaybackSession / OpenController / OpenPrepareParams 都在这层

// 同一个进程里只能有一个 QCoreApplication（重复创建会 assert）。
void EnsureApp() {
    static int argc = 1;
    static char arg0[] = "test_animated_image_loop";
    static char* argv[] = {arg0, nullptr};
    if (QCoreApplication::instance() == nullptr)
        new QCoreApplication(argc, argv); // NOLINT: 故意泄漏到进程结束
}

std::string TempDir() {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "videoeye_animated_image_loop";
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

// 解码线程的信号是 queued 到本线程的，必须转事件循环才收得到。
bool WaitUntil(const std::function<bool()>& done, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (done())
            return true;
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}

// 打开产物。
//
// attempt 必须活到播放结束：OpenController::OpenAndProbe 会把 format_ctx 的
// interrupt_callback.opaque 指向它（用于打开超时/取消）。Prepare 一返回就析构，
// 播放期的 av_read_frame 会读到悬垂指针并随机返回 AVERROR_EXIT —— 表现是
// "刚播 30ms 就 read aborted (cancel) -> exit"，而且时快时坏、极难查。
// MediaPlayer 用成员 current_attempt_ 保证这件事，测试也得照做。
struct OpenedImage {
    std::shared_ptr<OpenAttempt> attempt;
    model::StreamInfo info;
};

bool OpenImage(PlaybackSession& session, const std::string& path, OpenedImage& opened) {
    OpenPrepareParams params;
    params.url = QString::fromStdString(path);
    opened.attempt = std::make_shared<OpenAttempt>();
    params.attempt = opened.attempt;
    OpenResult result = OpenController::Prepare(params);
    if (!result.ok) {
        ADD_FAILURE() << "Prepare 失败: " << result.error.toStdString();
        return false;
    }
    OpenController controller(session);
    QString error;
    if (!controller.Commit(std::move(result), opened.info, error)) {
        ADD_FAILURE() << "Commit 失败: " << error.toStdString();
        return false;
    }
    return true;
}

// 一次"打开 + 播放"的公共装配：返回帧计数与完成计数的引用由调用方持有。
struct PlaybackProbe {
    std::atomic<int> frames{0};
    std::atomic<int> finished{0};

    void Bind(PlaybackSession& session) {
        PlaybackSession::Hooks hooks;
        hooks.on_video_frame = [this](const VideoFrameContext&) { frames.fetch_add(1); };
        session.SetHooks(std::move(hooks));
        QObject::connect(&session, &PlaybackSession::PlaybackFinished, &session, [this]() { finished.fetch_add(1); });
    }
};

} // namespace

// GIF 语料本身要真的是"3 帧 + 解复用器 gif"，否则后面所有断言都是在验一份假样本。
TEST(AnimatedImageLoopTest, GifSampleIsReallyAnimated) {
    EnsureApp();
    const auto gif = videoeye_test::CreateAnimatedImage(TempDir(), "sample3", "gif", 3, 10);
    ASSERT_TRUE(gif.ok) << gif.error;
    ASSERT_EQ(3, gif.frame_count);

    AVFormatContext* fmt = nullptr;
    ASSERT_GE(avformat_open_input(&fmt, gif.path.c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(fmt, nullptr), 0);
    ASSERT_NE(nullptr, fmt->iformat->name);
    EXPECT_STREQ("gif", fmt->iformat->name);

    int packets = 0;
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(fmt, packet) >= 0) {
        ++packets;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&fmt);
    EXPECT_EQ(3, packets);
}

TEST(AnimatedImageLoopTest, LoopingReplaysFromStartInsteadOfFinishing) {
    EnsureApp();
    const auto gif = videoeye_test::CreateAnimatedImage(TempDir(), "loop3", "gif", 3, 10);
    ASSERT_TRUE(gif.ok) << gif.error;

    PlaybackSession session;
    OpenedImage opened;
    ASSERT_TRUE(OpenImage(session, gif.path, opened));
    EXPECT_TRUE(opened.info.is_image_input);
    EXPECT_TRUE(opened.info.is_animated_image) << "gif 解复用器必须被判为动图";

    PlaybackProbe probe;
    probe.Bind(session);
    session.SetAnimatedImageLoop(true);
    ASSERT_TRUE(session.Play());

    // 3 帧 × 100ms = 300ms 一轮。解出 6 帧就证明第二轮真的从头开始了。
    const bool looped = WaitUntil([&probe]() { return probe.frames.load() >= 6; }, 6000);
    session.Stop(false);

    EXPECT_TRUE(looped) << "只解出 " << probe.frames.load() << " 帧，循环没有回到起点";
    EXPECT_EQ(0, probe.finished.load()) << "循环期间不应宣布播放结束（否则 UI 会走播完收尾）";
}

TEST(AnimatedImageLoopTest, WithoutLoopItFinishesExactlyOnce) {
    EnsureApp();
    const auto gif = videoeye_test::CreateAnimatedImage(TempDir(), "once3", "gif", 3, 10);
    ASSERT_TRUE(gif.ok) << gif.error;

    PlaybackSession session;
    OpenedImage opened;
    ASSERT_TRUE(OpenImage(session, gif.path, opened));

    PlaybackProbe probe;
    probe.Bind(session);
    session.SetAnimatedImageLoop(false);
    ASSERT_TRUE(session.Play());

    ASSERT_TRUE(WaitUntil([&probe]() { return probe.finished.load() > 0; }, 6000));
    session.Stop(false);

    EXPECT_EQ(3, probe.frames.load());
    EXPECT_EQ(1, probe.finished.load());
}

// 静图 + 强行打开循环：引擎必须自己拒绝。这条守的是"EOF 分支里按下真实解复用器判定"，
// 而不是信调用方传进来的布尔值 —— 单帧图片误开循环就是无限解码同一帧。
TEST(AnimatedImageLoopTest, StillImageIgnoresLoopRequest) {
    EnsureApp();
    const auto png = videoeye_test::CreateStillImage(TempDir(), "still");
    ASSERT_TRUE(png.ok) << png.error;

    PlaybackSession session;
    OpenedImage opened;
    ASSERT_TRUE(OpenImage(session, png.path, opened));
    EXPECT_TRUE(opened.info.is_image_input);
    EXPECT_FALSE(opened.info.is_animated_image) << "单帧 PNG 不能被动图判定吃掉";

    PlaybackProbe probe;
    probe.Bind(session);
    session.SetAnimatedImageLoop(true); // 故意开：静图不该循环
    ASSERT_TRUE(session.Play());

    ASSERT_TRUE(WaitUntil([&probe]() { return probe.finished.load() > 0; }, 6000));
    session.Stop(false);

    EXPECT_EQ(1, probe.frames.load());
    EXPECT_EQ(1, probe.finished.load()) << "静图循环 = 反复解码同一帧，必须只播一次";
}

// 动图重播走 RestartFromStart（不重新打开文件）：帧数必须重新走一遍。
// 这条同时钉住"APNG 的容器时长是 NOPTS，Seek(0) 会被不可定位挡掉"这个前提 ——
// 若哪天 RestartFromStart 被改回 Seek(0)，这里会因为 PlaybackFinished 不再增加而红。
TEST(AnimatedImageLoopTest, RestartFromStartReplaysAllFrames) {
    EnsureApp();
    const auto apng = videoeye_test::CreateAnimatedImage(TempDir(), "restart", "apng", 3, 10);
    ASSERT_TRUE(apng.ok) << apng.error;

    PlaybackSession session;
    OpenedImage opened;
    ASSERT_TRUE(OpenImage(session, apng.path, opened));
    EXPECT_TRUE(opened.info.is_animated_image) << "APNG 解复用器必须被判为动图";

    PlaybackProbe probe;
    probe.Bind(session);
    ASSERT_TRUE(session.Play());
    ASSERT_TRUE(WaitUntil([&probe]() { return probe.finished.load() == 1; }, 6000));
    EXPECT_EQ(3, probe.frames.load());

    ASSERT_TRUE(session.RestartFromStart());
    const bool replayed = WaitUntil([&probe]() { return probe.finished.load() == 2; }, 6000);
    session.Stop(false);

    EXPECT_TRUE(replayed) << "重播没有走到第二轮结束，帧数=" << probe.frames.load();
    EXPECT_EQ(6, probe.frames.load());
}
