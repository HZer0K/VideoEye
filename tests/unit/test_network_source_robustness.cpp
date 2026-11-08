// 网络源与流媒体健壮性（第五批）
//
// 覆盖目标
// --------
// 打开链路在本地文件上的行为已经由 OpenSession / RoleOpenAsync / GUI E2E 钉住；
// 网络源（尤其 HTTP）是完全另一条 IO 路径，过去的用例只到 TCP 层（BlockingTcpEndpoint
// 连协议头都不发）。真实世界里最常见的失败形态反而在 HTTP 语义里：
//   * 404 / 分片缺失 —— 要有终态、要有原因，不能挂着；
//   * body 只发一半就断 —— 必须收敛到失败，不能等"剩下的字节"等到天荒地老；
//   * 响应头回了但 body 不来（CDN 半死）—— Stop()/换媒体必须能立刻打断，且不能
//     占死打开任务槽导致下一次打开也失败；
//   * HLS 清单可读、引用的分片 404 —— 打开链路要识别为失败而不是"打开成功但没流"。
//
// 本地用 HttpTestServer 起可控 HTTP 服务（回环、按路径脚本化、支持 Range），
// 不依赖外网、不依赖机器上装了什么；每个用例的输入形态在 Route 里写明。
//
// 判据：只认 MediaPlayer 的公开终态（OpenFinished 恰好一次、ok、GetLastError）
// 与墙钟预算；不比对 FFmpeg 的具体错误文案（不同发行版会变）。

#include <gtest/gtest.h>

#include <functional>
#include <string>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#include "HttpTestServer.h"
#include "core/player/MediaPlayer.h"

namespace {

using namespace videoeye;
using videoeye::player::MediaPlayer;

// 打开（含后台 avformat 探测 + 收尾回 UI）的收敛预算。与产品侧 kOpenBudgetMs 一致，
// 不额外放宽：网络用例要抓的是"永远不收敛"，不是毫秒级抖动。
constexpr int kOpenBudgetMs = 5000;
// HLS 打开有一次清单解析 + 首个分片失败的过程，留出比单文件更宽的预算。
constexpr int kHlsBudgetMs = 8000;

// 事件循环/退出: 只销毁自己创建的那个实例，免得后续用例重建时出现第二个 QCoreApplication。
// 同时在进程内清掉代理变量 —— 本机常见 Clash / 系统代理会把 http 请求改道，
// 本地端口就永远等不到连接，用例会以完全误导的方式失败。
struct AppScope {
    QCoreApplication* app = nullptr;
    bool owned = false;

    AppScope() {
        qunsetenv("http_proxy");
        qunsetenv("https_proxy");
        qputenv("no_proxy", "127.0.0.1,localhost");

        static int argc = 1;
        static char arg0[] = "test_network_source_robustness";
        static char* argv[] = {arg0, nullptr};
        app = QCoreApplication::instance();
        if (!app) {
            app = new QCoreApplication(argc, argv);
            owned = true;
        }
    }
    ~AppScope() {
        if (owned)
            delete app;
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

std::string ReadCorpusFile(const char* relative) {
    QFile file(QString(VIDEOEYE_CORPUS_DIR) + "/" + relative);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

// 记录一次打开的全部终态信息。多次触发会被用例检出（"恰好一次"契约）。
struct OpenSink {
    int finished = 0;
    bool ok = false;

    void Attach(MediaPlayer& player) {
        QObject::connect(&player, &MediaPlayer::OpenFinished, &player, [this](bool value) {
            ++finished;
            ok = value;
        });
    }
};

} // namespace

// ===========================================================================
// HTTP 200：真实媒体经 HTTP 打开必须成功，且不能逼近预算
//
// FFmpeg 的 http 协议在打开 MP4 时会发 Range 请求；HttpTestServer 回 206 切片，
// 所以这条用例验证的是"打开链路能处理可 seek 的 HTTP 源"，而不是服务端脚本。
// ===========================================================================
TEST(NetworkSourceRobustness, HttpMediaOpensWithinBudget) {
    AppScope app_scope;

    const std::string body = ReadCorpusFile("container/real_h264_aac_faststart.mp4");
    ASSERT_FALSE(body.empty()) << "缺少真实语料 real_h264_aac_faststart.mp4";

    videoeye_test::HttpTestServer server({
        videoeye_test::HttpTestServer::BodyRoute("/clip.mp4", body, "video/mp4"),
    });
    ASSERT_TRUE(server.valid()) << "无法启动本地 HTTP 服务";

    MediaPlayer player;
    OpenSink sink;
    sink.Attach(player);

    QElapsedTimer timer;
    timer.start();
    player.OpenAsync(QString::fromStdString(server.url("/clip.mp4")));

    ASSERT_TRUE(PumpUntil([&]() { return sink.finished > 0; }, kOpenBudgetMs))
        << "HTTP 200 打开没有收敛，error=" << player.GetLastError().toStdString();
    EXPECT_TRUE(sink.ok) << "HTTP 200 + 真实媒体应当能打开: " << player.GetLastError().toStdString();
    EXPECT_EQ(1, sink.finished) << "一次打开只该有一个终态";
    EXPECT_LE(timer.elapsed(), kOpenBudgetMs) << "打开耗时逼近/超过预算";
}

// ===========================================================================
// HTTP 404：必须恰好一次失败终态，并带回原因
//
// 失败路径如果静默（OpenFinished 不发、GetLastError 为空），界面会停在"正在打开"
// 且没有任何解释 —— 这是首批"网络源打不开就整窗假死"的直接来源。
// ===========================================================================
TEST(NetworkSourceRobustness, Http404ReportsFailureOnce) {
    AppScope app_scope;

    videoeye_test::HttpTestServer server({}); // 未登记路径一律 404
    ASSERT_TRUE(server.valid());

    MediaPlayer player;
    OpenSink sink;
    sink.Attach(player);

    player.OpenAsync(QString::fromStdString(server.url("/missing.mp4")));

    ASSERT_TRUE(PumpUntil([&]() { return sink.finished > 0; }, kOpenBudgetMs))
        << "HTTP 404 没有收敛: 界面会一直停在'正在打开'";
    PumpFor(200); // 给可能的重复终态一个落地机会
    EXPECT_EQ(1, sink.finished) << "失败也必须恰好一个终态";
    EXPECT_FALSE(sink.ok);
    EXPECT_FALSE(player.GetLastError().trimmed().isEmpty()) << "失败必须带原因，界面才能解释哪一步没过";
}

// ===========================================================================
// body 截断：服务端声明了完整长度，只发一小段就断开
//
// 这种形态在真实网络里最常见（连接被中间设备掐断）。如果打开只按 Content-Length
// 一直等剩余字节，就会卡到中断回调才退出。判据是"能收敛到失败"，不是失败文案。
// ===========================================================================
TEST(NetworkSourceRobustness, TruncatedHttpBodyConvergesToFailure) {
    AppScope app_scope;

    const std::string body = ReadCorpusFile("container/real_h264_aac_faststart.mp4");
    ASSERT_FALSE(body.empty());

    videoeye_test::HttpTestServer server({
        videoeye_test::HttpTestServer::TruncateRoute("/broken.mp4", body, 256),
    });
    ASSERT_TRUE(server.valid());

    MediaPlayer player;
    OpenSink sink;
    sink.Attach(player);

    player.OpenAsync(QString::fromStdString(server.url("/broken.mp4")));

    ASSERT_TRUE(PumpUntil([&]() { return sink.finished > 0; }, kOpenBudgetMs))
        << "截断响应没有收敛: 打开在等永远来不了的剩余字节";
    PumpFor(200);
    EXPECT_EQ(1, sink.finished);
    EXPECT_FALSE(sink.ok) << "只读到 256 字节的 MP4 不应被当成打开成功";
    EXPECT_FALSE(player.GetLastError().trimmed().isEmpty());
}

// ===========================================================================
// body 不来（头已回）：Stop 必须立刻打断，且不能占死打开任务槽
//
// 复现 CDN 半死：响应头正常、body 永远不发。此时 avformat 阻塞在 http 的 read 上，
// 只有中断回调能让它退出。Stop 返回后立刻换一个真实本地文件打开，若旧任务仍占着
// 打开槽，这次打开就会卡住 —— 因此"下一次打开成功"是旧任务确实被释放的证据。
// ===========================================================================
TEST(NetworkSourceRobustness, StalledHttpBodyCanBeInterrupted) {
    AppScope app_scope;

    videoeye_test::HttpTestServer server({
        videoeye_test::HttpTestServer::StallRoute("/stall.mp4", /*stall_before_headers=*/false),
    });
    ASSERT_TRUE(server.valid());

    MediaPlayer player;
    OpenSink sink;
    sink.Attach(player);

    player.OpenAsync(QString::fromStdString(server.url("/stall.mp4")));
    // 先确认它真的卡住了（否则这条用例什么也没验证到）。
    PumpFor(400);
    ASSERT_EQ(0, sink.finished) << "静默服务端不该在 400ms 内给出终态";

    QElapsedTimer stop_timer;
    stop_timer.start();
    player.Stop();
    EXPECT_LT(stop_timer.elapsed(), 1000) << "Stop 被阻塞的网络打开拖住了";

    // 用真实本地文件确认旧的打开任务已经被中断释放。
    const std::string good_path = std::string(VIDEOEYE_CORPUS_DIR) + "/container/real_h264_aac_faststart.mp4";
    ASSERT_TRUE(QFile::exists(QString::fromStdString(good_path)));
    const int finished_before = sink.finished;
    player.OpenAsync(QString::fromStdString(good_path));

    ASSERT_TRUE(PumpUntil([&]() { return sink.finished > finished_before; }, kOpenBudgetMs))
        << "Stop 后再次打开本地文件没有收敛: 旧的阻塞打开仍占着任务槽";
    EXPECT_TRUE(sink.ok) << "本地文件应当能打开: " << player.GetLastError().toStdString();
}

// ===========================================================================
// HLS：清单可读、首个分片 404
//
// 只验证打开链路的终态：必须在 HLS 预算内收敛为失败，而不是"打开成功但没有流"
// 或挂在重试上。清单与分片都由同一台本地服务提供，URI 里的 {PORT} 自动替换。
// ===========================================================================
TEST(NetworkSourceRobustness, HlsPlaylistWithMissingSegmentFailsWithinBudget) {
    AppScope app_scope;

    const std::string playlist = "#EXTM3U\n"
                                 "#EXT-X-VERSION:3\n"
                                 "#EXT-X-TARGETDURATION:2\n"
                                 "#EXTINF:2.0,\n"
                                 "http://127.0.0.1:{PORT}/seg.ts\n"
                                 "#EXT-X-ENDLIST\n";

    videoeye_test::HttpTestServer server({
        videoeye_test::HttpTestServer::BodyRoute("/playlist.m3u8", playlist, "application/vnd.apple.mpegurl"),
        videoeye_test::HttpTestServer::StatusRoute("/seg.ts", 404),
    });
    ASSERT_TRUE(server.valid());

    MediaPlayer player;
    OpenSink sink;
    sink.Attach(player);

    player.OpenAsync(QString::fromStdString(server.url("/playlist.m3u8")));

    ASSERT_TRUE(PumpUntil([&]() { return sink.finished > 0; }, kHlsBudgetMs))
        << "HLS 分片 404 没有收敛: 打开链路可能挂在重试上";
    PumpFor(200);
    EXPECT_EQ(1, sink.finished);
    EXPECT_FALSE(sink.ok) << "首个分片 404 的 HLS 不应被当成打开成功";
    EXPECT_FALSE(player.GetLastError().trimmed().isEmpty());
}
