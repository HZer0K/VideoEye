// OpenController::Commit 的**信号顺序**契约（有顺序语义，改顺序不会有编译错误）。
//
// 回归点: 带专辑封面的纯音频文件（MP3/M4A/FLAC 很常见）:
//   * 封面走 FrameReady，模式切换走 MediaModeChanged(has_video=false)；
//   * 界面只有在收到 has_video=false 之后才进"纯音频"模式，此后到达的帧才会被
//     当成专辑封面存进音频可视化层；切模式那一手还会 Clear() 视频区。
// 所以两个信号的顺序反过来就是"封面先画进视频区、随即被切模式清掉" ——
// 用户看到的是既没有可视化、也没有封面，而代码里没有任何地方会报错。
//
// 这里不需要真实媒体：Commit 只吃一个已经准备好的 OpenResult。

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QImage>
#include <QString>

#include <algorithm>
#include <string>
#include <vector>

#include "core/player/OpenController.h"
#include "core/player/PlaybackSession.h"

namespace {

using namespace videoeye;

// QSignalSpy 之外的最小记录器：按顺序记下信号名即可，不依赖 QtTest 模块。
// 用 std::string 而不是 QString 存: gtest 打印 QString 会把每个 QChar 打成
// "2-byte object <6D-00>"，断言失败时根本看不出顺序错在哪。
struct SignalRecorder {
    std::vector<std::string> order;

    void Bind(player::OpenController& controller) {
        QObject::connect(&controller, &player::OpenController::MediaModeChanged, &controller,
                         [this](bool has_video) { order.push_back(has_video ? "mode:video" : "mode:audio"); });
        QObject::connect(&controller, &player::OpenController::FrameReady, &controller,
                         [this](const QImage&) { order.push_back("frame"); });
        QObject::connect(&controller, &player::OpenController::MediaInfoTextReady, &controller,
                         [this](const QString&) { order.push_back("info"); });
        QObject::connect(&controller, &player::OpenController::OpenFailed, &controller,
                         [this](const QString&) { order.push_back("failed"); });
    }
};

player::OpenResult MakeResult(bool has_video, bool with_cover) {
    player::OpenResult result;
    result.ok = true;
    result.has_video = has_video;
    result.video_stream_index = has_video ? 0 : -1;
    result.audio_stream_index = 1;
    if (with_cover) {
        result.cover_art = QImage(4, 4, QImage::Format_RGB32);
        result.cover_art.fill(Qt::darkGray);
    }
    result.media_info_text = "audio: AAC 44100 Hz";
    return result;
}

// 直接连接（同线程）不依赖事件循环，但 QObject 信号槽仍要求有 QCoreApplication。
// 只创建一次、进程退出时不管（与其它用例共用；重复创建会 assert）。
void EnsureApp() {
    static int argc = 1;
    static char arg0[] = "test_open_controller_commit_order";
    static char* argv[] = {arg0, nullptr};
    if (QCoreApplication::instance() == nullptr) {
        new QCoreApplication(argc, argv); // NOLINT: 故意泄漏到进程结束
    }
}

// 带封面的纯音频: 必须先声明"没有视频"，再送画面。顺序反了封面会被切模式清掉。
TEST(OpenControllerCommitOrder, AudioModePrecedesCoverFrame) {
    EnsureApp();

    player::PlaybackSession session;
    player::OpenController controller(session);
    SignalRecorder recorder;
    recorder.Bind(controller);

    model::StreamInfo info;
    QString error;
    ASSERT_TRUE(controller.Commit(MakeResult(/*has_video=*/false, /*with_cover=*/true), info, error));

    ASSERT_GE(recorder.order.size(), 2u) << "没有发出预期的信号";
    EXPECT_EQ("mode:audio", recorder.order.at(0)) << "界面还没切模式, 封面就会被画进视频区";
    EXPECT_EQ("frame", recorder.order.at(1));
    EXPECT_EQ((std::vector<std::string>{"mode:audio", "frame", "info"}), recorder.order);
}

// 真视频文件: 先声明"有视频"，且没有封面图时不该凭空发一帧。
TEST(OpenControllerCommitOrder, VideoModeWithoutCoverSendsNoFrame) {
    EnsureApp();
    player::PlaybackSession session;
    player::OpenController controller(session);
    SignalRecorder recorder;
    recorder.Bind(controller);

    model::StreamInfo info;
    QString error;
    ASSERT_TRUE(controller.Commit(MakeResult(/*has_video=*/true, /*with_cover=*/false), info, error));

    ASSERT_FALSE(recorder.order.empty());
    EXPECT_EQ("mode:video", recorder.order.front());
    EXPECT_EQ(recorder.order.end(), std::find(recorder.order.begin(), recorder.order.end(), "frame"))
        << "没有封面却发了一帧(会显示一张空图)";
}

// 失败路径: 只发 OpenFailed，不许先发一个 MediaModeChanged 把界面切成某种模式。
TEST(OpenControllerCommitOrder, FailureSendsOnlyOpenFailed) {
    EnsureApp();
    player::PlaybackSession session;
    player::OpenController controller(session);
    SignalRecorder recorder;
    recorder.Bind(controller);

    player::OpenResult result;
    result.ok = false;
    result.error = QStringLiteral("探测失败");

    model::StreamInfo info;
    QString error;
    EXPECT_FALSE(controller.Commit(std::move(result), info, error));
    EXPECT_EQ((std::vector<std::string>{"failed"}), recorder.order);
    EXPECT_EQ(QStringLiteral("探测失败"), error);
}

} // namespace
