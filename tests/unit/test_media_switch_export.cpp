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

// 一个"解析要花点时间"的合法 EBML 输入: EBML header + 一大批顶级 Void 元素。
// 用来让"播放器被销毁"这件事稳定地落在解析中途, 而不是等任务早就跑完了才销毁。
QString WriteBigEbmlInput() {
    const QString path = QDir::tempPath() + "/videoeye_container_big.mkv";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();

    QByteArray head;
    head.append(char(0x1A)).append(char(0x45)).append(char(0xDF)).append(char(0xA3));
    head.append(char(0x83));                                              // size = 3
    head.append(char(0xEC)).append(char(0x81)).append(char(0x00));        // 载荷: 一个 Void

    // 一块 4096 个 Void 元素, 重复若干块 —— 元素个数决定解析耗时
    QByteArray chunk;
    chunk.resize(3 * 4096);
    for (int i = 0; i + 2 < chunk.size(); i += 3) {
        chunk[i] = char(0xEC);
        chunk[i + 1] = char(0x81);
        chunk[i + 2] = char(0x00);
    }

    f.write(head);
    for (int i = 0; i < 15; ++i) f.write(chunk);
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

// ===========================================================================
// 容器结构分析失败必须有终态（分析异常/失败被吞掉的回归）
//
// 修复前: StartContainerStructureAnalysis 的任务体里 catch 到异常只记一行日志就
// 正常返回，RunBlockingIo 顺手把任务记为 Succeeded —— 界面既收不到失败也收不到
// 成功，一个"看起来分析过了但什么都没出来"的空状态。现在失败路径必须走
// Failed 终态 + ContainerStructureFailed，且**不得**发布 ContainerStructureReady。
// ===========================================================================
TEST(MediaContainerStructure, FailureProducesFailedTerminalWithoutReady) {
    AppScope app_scope;

    const QString corrupt = WriteCorruptInput();
    ASSERT_FALSE(corrupt.isEmpty());

    player::MediaPlayer player;
    int ready = 0;
    int failed = 0;
    QString failure_message;
    QObject::connect(&player, &player::MediaPlayer::ContainerStructureReady, &player,
                     [&ready](const videoeye::model::ContainerStructureResult&) { ++ready; });
    QObject::connect(&player, &player::MediaPlayer::ContainerStructureFailed, &player,
                     [&failed, &failure_message](const QString& msg) {
                         ++failed;
                         failure_message = msg;
                     });

    // 公开入口: 播放器 Open 失败后进入"分析模式"用的就是这一条
    player.RequestContainerStructureAnalysis(corrupt);

    EXPECT_TRUE(PumpUntil([&] { return failed > 0 || ready > 0; }, 10000))
        << "容器结构分析既没给 Ready 也没给 Failed: 后台任务会永远停在 Running";
    EXPECT_GT(failed, 0) << "失败必须落到 ContainerStructureFailed, 不能只记一行日志了事";
    EXPECT_EQ(0, ready) << "失败路径不得发布有效结果: 半份结构树会被当结论显示出来";
    EXPECT_FALSE(failure_message.trimmed().isEmpty()) << "失败信号必须带原因, 界面才能告诉用户到底哪一步没过";

    QFile::remove(corrupt);
}

// ===========================================================================
// 过期失败信号必须被丢弃（复查 P1: 终态一次性 + UI 回调不得补写终态）
//
// 修复前: 失败信号是在 UI 线程落地的, 而落地那一刻 slot 上跑的可能是**新任务** ——
// 于是旧任务的失败既弹到了界面上, 又有机会改写新任务的终态(Succeeded -> Failed)。
// 现在终态由任务体自己声明, UI 回调只发信号, 且发之前先比对 current id。
//
// 用例刻意在这两次请求之间**只睡、不泵事件**, 让第一条失败消息稳定地压在 UI 队列
// 里, 从而复现"旧消息在新任务开始之后才落地"这个时序。
// ===========================================================================
TEST(MediaContainerStructure, StaleFailureIsDroppedAfterNewTaskStarts) {
    AppScope app_scope;

    const QString missing_a = QDir::tempPath() + "/videoeye_container_missing_a.mkv";
    const QString missing_b = QDir::tempPath() + "/videoeye_container_missing_b.mkv";
    QFile::remove(missing_a);
    QFile::remove(missing_b);

    player::MediaPlayer player;
    int failed = 0;
    int ready = 0;
    QObject::connect(&player, &player::MediaPlayer::ContainerStructureFailed, &player,
                     [&failed](const QString&) { ++failed; });
    QObject::connect(&player, &player::MediaPlayer::ContainerStructureReady, &player,
                     [&ready](const videoeye::model::ContainerStructureResult&) { ++ready; });

    player.RequestContainerStructureAnalysis(missing_a);
    // 关键: 不泵事件 —— 第一个任务的失败消息必须还留在队列里没落地
    QThread::msleep(500);
    player.RequestContainerStructureAnalysis(missing_b);

    ASSERT_TRUE(PumpUntil([&] { return failed > 0 || ready > 0; }, 10000))
        << "第二个任务的失败信号没有到达";
    PumpFor(300);
    EXPECT_EQ(1, failed) << "第一个任务的失败消息在新任务开始之后落地时必须被丢弃";
    EXPECT_EQ(0, ready);

    QFile::remove(missing_a);
    QFile::remove(missing_b);
}

// ===========================================================================
// 分析进行中销毁播放器（复查 P1: 空指针解引用）
//
// 修复前: `if (!ok || !self) { if (token.IsCanceled()) self->task_manager_.End(...); }`
// 在 self 已失效且令牌已取消时会直接解引用空 QPointer。现在任务体一进来先判 self,
// 中途每一处 self-> 之前也都判过。
//
// 任务体只按值捕获 QPointer self + QString, 所以 TaskManager 在关闭预算内放弃它
// (detach) 也不会踩到已销毁的对象 —— 这条用例同时守住这条生命周期约定。
// ===========================================================================
TEST(MediaContainerStructure, DestroyingPlayerDuringAnalysisIsSafe) {
    AppScope app_scope;

    const QString big = WriteBigEbmlInput();
    ASSERT_FALSE(big.isEmpty());

    auto* player = new player::MediaPlayer;
    player->RequestContainerStructureAnalysis(big);
    QThread::msleep(30);   // 让任务体真的进入 EBML 解析

    QElapsedTimer timer;
    timer.start();
    delete player;         // 析构: CancelAll + WaitForAll, 此时任务体仍在解析中途
    const qint64 elapsed = timer.elapsed();

    EXPECT_LT(elapsed, 10000) << "销毁播放器不得被后台分析任务拖住";
    QFile::remove(big);
}
