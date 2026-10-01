// 换媒体必须终止进行中的导出（复查 P1-2）
//
// 修复前: MainWindow::OpenMedia 只调 CancelVideoFrameExport()，而且只在导出进度框可见时；
// MediaPlayer::OpenInternal 则两类导出都不取消。于是"媒体转码导出进行中打开新文件"
// 会留下一个仍在跑的旧任务 —— 它继续写旧文件，终态信号还会串回新媒体的界面。
//
// 修复: "取消全部导出"归 MediaPlayer::CancelAllExports()，由 OpenInternal() 统一执行
// （打开媒体的入口不止 UI 那一个），并顺带推进导出代际号，让旧任务的回包全部作废。
// UI 只负责重置自己的进度框。
//
// 用例设计: 用"只监听、从不回包"的本地端口当输入，让旧导出稳定地卡在阻塞 IO 里，
// 再 Open() 一个新文件，然后要求**新的导出请求必须能立刻开始** ——
// 只要旧任务没有被真正终止，它就一直占着"导出中"的位置，新请求会被永久排队。
// 这样既验证了"旧任务被取消"，也验证了"卡死的旧任务不再堵塞后续导出"。

#include <gtest/gtest.h>

#include <functional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#include "BlockingTcpEndpoint.h"
#include "core/exporter/MediaExporter.h"
#include "core/player/MediaPlayer.h"

namespace {

using namespace videoeye;

// 排队信号需要事件循环；退出时把**自己建的**那个销毁，免得后续用例再建就成了第二个实例。
struct AppScope {
    QCoreApplication* app = nullptr;
    bool owned = false;

    AppScope() {
        static int argc = 1;
        static char arg0[] = "test_media_switch_export";
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

struct ExportTerminals {
    int finished = 0;
    int error = 0;
    int canceled = 0;
    QString last_error;

    int Total() const { return finished + error + canceled; }
};

void WireTerminals(player::MediaPlayer& player, ExportTerminals& out) {
    QObject::connect(&player, &player::MediaPlayer::MediaExportFinished, &player,
                     [&out](const QString&) { ++out.finished; });
    QObject::connect(&player, &player::MediaPlayer::MediaExportError, &player,
                     [&out](const QString& msg) { ++out.error; out.last_error = msg; });
    QObject::connect(&player, &player::MediaPlayer::MediaExportCanceled, &player,
                     [&out](const QString&) { ++out.canceled; });
}

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

QString WriteCorruptInput() {
    const QString path = QDir::tempPath() + "/videoeye_switch_corrupt_in.bin";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    QByteArray junk(2048, '\x00');
    for (int i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 73 + 11) & 0xFF);
    f.write(junk);
    f.close();
    return path;
}

exporter::ExportOptions MakeOptions(const QString& input, const QString& output) {
    exporter::ExportOptions opt;
    opt.input_path = input;
    opt.output_path = output;
    opt.format = "mp4";
    opt.kind = exporter::ExportKind::Video;
    return opt;
}

} // namespace

TEST(MediaContextSwitch, OpenCancelsRunningMediaExport) {
    AppScope app_scope;

    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法创建本地监听端口";

    player::MediaPlayer player;
    ExportTerminals terminals;
    WireTerminals(player, terminals);

    // --- 起一个会卡在网络 IO 上的媒体导出 ---
    const QString old_output = QDir::tempPath() + "/videoeye_switch_old.mp4";
    QFile::remove(old_output);
    player.StartMediaExport(MakeOptions(QString::fromStdString(endpoint.tcp_url()), old_output));
    PumpFor(700);   // 让它真的卡进阻塞的打开/读取

    // --- 换媒体（打开一个不存在的文件即可: 关键是它触发了媒体上下文切换）---
    const QString missing = QDir::tempPath() + "/videoeye_switch_no_such_file.mp4";
    QFile::remove(missing);
    EXPECT_FALSE(player.Open(missing));

    // 旧任务的回包必须被作废（代际号已推进），界面不该收到任何终态 ——
    // 否则"旧导出完成"会盖掉新媒体的状态。
    PumpFor(1200);
    EXPECT_EQ(0, terminals.Total())
        << "旧导出的终态信号串回了新媒体上下文（导出代际号没有推进）";

    // --- 关键断言: 旧任务必须已被终止，新的导出请求不能被它永久堵住 ---
    const QString corrupt = WriteCorruptInput();
    ASSERT_FALSE(corrupt.isEmpty());
    const QString new_output = QDir::tempPath() + "/videoeye_switch_new.mp4";
    QFile::remove(new_output);

    player.StartMediaExport(MakeOptions(corrupt, new_output));
    ASSERT_TRUE(PumpUntil([&] { return terminals.Total() > 0; }, 10000))
        << "换媒体没有真正终止旧导出: 卡死的旧任务占着'导出中', 新请求被永久排队";
    EXPECT_GT(terminals.error, 0) << "损坏输入应当上报 MediaExportError; 实际错误数="
                                  << terminals.error;

    QFile::remove(corrupt);
    QFile::remove(old_output);
    QFile::remove(new_output);
    QFile::remove(missing);
}
