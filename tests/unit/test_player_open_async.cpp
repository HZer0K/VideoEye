// 事务式异步打开（评审项 6）
//
// 修复前: avformat_open_input / avformat_find_stream_info 直接跑在 UI 线程上，打开
// 网络源/大文件/异常设备会把界面整段冻住。现在拆成两阶段事务:
//   Prepare (后台线程) 产出自我持有的 OpenResult  ->  Commit (UI 线程) 一次性采用或整体丢弃。
//
// 本文件锁定三条契约:
//   1. OpenResult 的事务语义 —— Commit 采用则资源进会话; 失败则整体丢弃且会话归零。
//   2. OpenAsync 的终态 —— 无论成功失败都恰好发一次 OpenFinished(bool)。
//   3. 过期结果必须被丢弃 —— 被后续打开取代的旧结果不得发 OpenFinished（否则界面会
//      按上一个媒体收尾）。
//
// 需要事件循环（OpenAsync 的结果经 Qt::QueuedConnection 排回 UI 线程）,
// 不需要 QPA 平台插件。

#include <gtest/gtest.h>

#include <functional>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#include "BlockingTcpEndpoint.h"
#include "core/player/MediaPlayer.h"
#include "core/player/OpenController.h"
#include "core/player/PlaybackSession.h"

namespace {

using namespace videoeye;

// 事件循环/退出: 只销毁自己创建的那个实例，免得后续用例重建时出现第二个 QCoreApplication。
struct AppScope {
    QCoreApplication* app = nullptr;
    bool owned = false;

    AppScope() {
        static int argc = 1;
        static char arg0[] = "test_player_open_async";
        static char* argv[] = {arg0, nullptr};
        app = QCoreApplication::instance();
        if (!app) {
            app = new QCoreApplication(argc, argv);
            owned = true;
        }
    }
    ~AppScope() {
        if (owned) delete app;
    }
    AppScope(const AppScope&) = delete;
    AppScope& operator=(const AppScope&) = delete;
};

bool PumpUntil(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return done();
}

void PumpFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
}

QString MissingPath(const QString& name) {
    const QString path = QDir::tempPath() + "/" + name;
    QFile::remove(path);
    return path;
}

} // namespace

// ===========================================================================
// 事务的提交侧: 采用 vs 丢弃
//
// 用一个"内存里分配、未真正打开"的 AVFormatContext 当契约替身即可: 要验证的是
// 所有权与状态的一次性转移，而不是 FFmpeg 能不能解析它。
// ===========================================================================
TEST(PlayerOpenTransaction, CommitAdoptsResultAtomically) {
    AppScope app_scope;

    player::PlaybackSession session;
    player::OpenController controller(session);

    player::OpenResult result;
    result.ok = true;
    result.format_ctx = avformat_alloc_context();
    ASSERT_NE(nullptr, result.format_ctx);
    result.video_stream_index = 2;
    result.audio_stream_index = 5;
    result.has_video = true;
    result.duration_ms = 4321;

    videoeye::model::StreamInfo info;
    QString error;
    const bool ok = controller.Commit(std::move(result), info, error);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.isEmpty());
    // 资源已整体转移进会话。
    EXPECT_NE(nullptr, session.format_ctx());
    EXPECT_EQ(2, session.video_stream_index());
    EXPECT_EQ(5, session.audio_stream_index());
    EXPECT_EQ(4321, session.duration_ms());
    EXPECT_EQ(videoeye::model::PlayerState::Idle, session.state());

    // OpenResult 已被取走所有权: 其析构不得再关掉已交给会话的上下文。
    EXPECT_EQ(nullptr, result.format_ctx);
    EXPECT_NE(nullptr, session.format_ctx());
}

TEST(PlayerOpenTransaction, CommitDiscardsFailedResultAndReleasesSession) {
    AppScope app_scope;

    player::PlaybackSession session;
    player::OpenController controller(session);

    // 先让会话里有一份"旧媒体"，验证失败提交会把它整体清掉。
    session.AdoptFormatContext(avformat_alloc_context());
    ASSERT_NE(nullptr, session.format_ctx());
    session.SetDuration(999);
    session.SetStreamIndices(1, 1);

    int failed_signals = 0;
    QString failed_message;
    QObject::connect(&controller, &player::OpenController::OpenFailed, &session,
                     [&failed_signals, &failed_message](const QString& msg) {
                         ++failed_signals;
                         failed_message = msg;
                     });

    player::OpenResult result;
    result.ok = false;
    result.error = QStringLiteral("模拟打开失败");
    // 失败也可能带一个已探测到的上下文: 它归 result 所有，提交被拒后由 result 析构关闭。
    result.format_ctx = avformat_alloc_context();
    ASSERT_NE(nullptr, result.format_ctx);

    videoeye::model::StreamInfo info;
    QString error;
    const bool ok = controller.Commit(std::move(result), info, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(QStringLiteral("模拟打开失败"), error);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(1, failed_signals) << "失败必须上报恰好一次 OpenFailed";
    EXPECT_EQ(QStringLiteral("模拟打开失败"), failed_message);
    // 整体丢弃: 会话的媒资回到空态 (duration 不属于资源, Release() 刻意不动它)。
    EXPECT_EQ(nullptr, session.format_ctx());
    EXPECT_EQ(-1, session.video_stream_index());
    EXPECT_EQ(-1, session.audio_stream_index());
}

// ===========================================================================
// OpenAsync 失败终态: 文件不存在也必须恰好发一次 OpenFinished(false)
// ===========================================================================
TEST(PlayerOpenAsync, MissingFileEmitsOpenFinishedFalseOnce) {
    AppScope app_scope;

    const QString missing = MissingPath("videoeye_open_async_missing.mp4");

    player::MediaPlayer player;
    int finished = 0;
    bool last_ok = true;
    QObject::connect(&player, &player::MediaPlayer::OpenFinished, &player,
                     [&finished, &last_ok](bool ok) { ++finished; last_ok = ok; });

    player.OpenAsync(missing);

    ASSERT_TRUE(PumpUntil([&] { return finished > 0; }, 10000))
        << "OpenAsync 失败后没有任何 OpenFinished: 界面会一直停在'正在打开'";
    PumpFor(300);   // 给可能的重复终态一个落地机会
    EXPECT_EQ(1, finished) << "一次打开只该有一个终态";
    EXPECT_FALSE(last_ok);
    EXPECT_FALSE(player.GetLastError().trimmed().isEmpty())
        << "失败必须带原因 (GetLastError)，界面才能告诉用户哪一步没过";
}

// ===========================================================================
// 被取代的旧打开结果必须被丢弃
//
// A 用"只监听、从不回包"的本地端口当输入，稳定卡在 avformat_open_input 上; 随后
// OpenAsync(B, 不存在的文件) 取代它。A 的结果不得发 OpenFinished —— 否则界面会拿
// 上一个媒体的失败去收尾当前这次打开。
// ===========================================================================
TEST(PlayerOpenAsync, StaleResultIsDroppedWhenSuperseded) {
    AppScope app_scope;

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法创建本地监听端口";

    const QString missing_b = MissingPath("videoeye_open_async_stale_b.mp4");

    player::MediaPlayer player;
    int finished = 0;
    bool last_ok = true;
    QObject::connect(&player, &player::MediaPlayer::OpenFinished, &player,
                     [&finished, &last_ok](bool ok) { ++finished; last_ok = ok; });

    // A: 卡在阻塞打开（不泵事件, 让它稳定停在中途）。
    player.OpenAsync(QString::fromStdString(endpoint.tcp_url()));
    QThread::msleep(300);

    // B 取代 A。
    player.OpenAsync(missing_b);

    ASSERT_TRUE(PumpUntil([&] { return finished > 0; }, 10000))
        << "第二个打开没有给出终态";
    PumpFor(600);   // 给迟到的 A 结果一个落地的机会
    EXPECT_EQ(1, finished)
        << "被后续打开取代的旧结果不得发 OpenFinished (否则界面的收尾对应的是上一个媒体)";
    EXPECT_FALSE(last_ok) << "到达的应当是 B(不存在文件) 的结果";

    QFile::remove(missing_b);
}
