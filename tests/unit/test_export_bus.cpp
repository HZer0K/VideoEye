// 导出信号总线（core/player/ExportBus.h）的接缝测试。
//
// 为什么值得单独测: 抽帧 / 音视频导出的十条信号从 MediaPlayer 门面搬到独立总线，
// 搬迁本身"编译过就算对了" —— 但三种错都不会崩:
//   1. 门面上那十条忘了删 → 门面照旧背着一份与播放无关的契约（白搬）；
//   2. 总线上的信号名/参数被改 → 编译期可能靠隐式转换糊过去，UI 收的是错的值；
//   3. ExportController 继续发自己那份信号而没发到总线上 → UI 一条也收不到，
//      进度框永远不弹（静默失效）。
//
// 盯四件事:
//   1. 门面不再声明这十条信号（元对象层面断言，不是看代码）；
//   2. 总线上十条齐全（名字 + 参数签名）；
//   3. export_bus() 稳定返回同一个对象，且归属 UI 线程；
//   4. 控制器真的往总线发: 无媒体时 StartVideoFrameExport 必须经总线报 Error，
//      且不得发 Started（终态互斥）。
//
// 用 QCoreApplication 即可（不涉及控件），不需要 offscreen。

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QString>
#include <QThread>

#include "core/player/ExportBus.h"
#include "core/player/MediaPlayer.h"

namespace {

using videoeye::player::ExportBus;
using videoeye::player::MediaPlayer;

struct AppScope {
    QCoreApplication* app = nullptr;
    bool owned = false;

    AppScope() {
        static int argc = 1;
        static char arg0[] = "test_export_bus";
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

int SignalIndex(const QMetaObject& mo, const char* signature) {
    return mo.indexOfSignal(signature);
}

const char* const kExportSignatures[] = {
    "VideoFrameExportStarted(int)",
    "VideoFrameExportProgress(int)",
    "VideoFrameExportFinished(QString)",
    "VideoFrameExportCanceled(int,QString)",
    "VideoFrameExportError(QString)",
    "MediaExportStarted(qint64)",
    "MediaExportProgress(int)",
    "MediaExportFinished(QString)",
    "MediaExportCanceled(QString)",
    "MediaExportError(QString)",
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. 门面不再背这十条信号（否则等于白搬一趟）
// ---------------------------------------------------------------------------
TEST(ExportBusTests, FacadeDoesNotDeclareExportSignalsAnymore) {
    for (const char* sig : kExportSignatures) {
        EXPECT_EQ(-1, SignalIndex(MediaPlayer::staticMetaObject, sig))
            << "MediaPlayer 还在声明导出信号 " << sig << " —— 它该在 ExportBus 上";
    }
}

// ---------------------------------------------------------------------------
// 2. 总线上十条齐全（名字 + 参数签名与老契约一致）
// ---------------------------------------------------------------------------
TEST(ExportBusTests, BusDeclaresTheFullExportContract) {
    for (const char* sig : kExportSignatures) {
        EXPECT_GE(SignalIndex(ExportBus::staticMetaObject, sig), 0)
            << "ExportBus 缺少导出信号 " << sig;
    }
}

// ---------------------------------------------------------------------------
// 3. export_bus() 稳定 + 归属 UI 线程
// ---------------------------------------------------------------------------
TEST(ExportBusTests, BusHandleIsStableAndLivesOnUiThread) {
    AppScope app_scope;

    MediaPlayer player;
    ExportBus& first = player.export_bus();
    ExportBus& second = player.export_bus();
    EXPECT_EQ(&first, &second) << "export_bus() 每次都得给出同一个对象, 否则接线会连到不同的总线";
    EXPECT_TRUE(first.parent() != nullptr) << "总线必须有宿主, 不能是游离对象";
    EXPECT_EQ(player.thread(), first.thread()) << "总线要跟播放器同线程, 投递方式才和改之前一致";
}

// ---------------------------------------------------------------------------
// 4. 控制器真的往总线发: 无媒体抽帧 → Error，且不发 Started
// ---------------------------------------------------------------------------
TEST(ExportBusTests, ControllerEmitsErrorThroughBusWhenNoMediaOpened) {
    AppScope app_scope;

    MediaPlayer player;
    int error_count = 0;
    int started_count = 0;
    QString last_error;

    QObject::connect(&player.export_bus(), &ExportBus::VideoFrameExportError, &player,
                     [&](const QString& msg) {
                         ++error_count;
                         last_error = msg;
                     });
    QObject::connect(&player.export_bus(), &ExportBus::VideoFrameExportStarted, &player,
                     [&](int) { ++started_count; });

    player.StartVideoFrameExport(QDir::tempPath(), "jpg");

    EXPECT_EQ(1, error_count) << "ExportController 必须经总线把错误送出去, UI 才知道导出没起来";
    EXPECT_EQ(0, started_count) << "没起来就不该发 Started";
    EXPECT_FALSE(last_error.trimmed().isEmpty()) << "错误信号必须带原因";
}
