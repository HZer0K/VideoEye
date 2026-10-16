// 播放器信号总线（导出 ExportBus / 容器 ContainerBus）的接缝测试。
//
// 为什么值得单独测: 这两组信号从 MediaPlayer 门面搬到独立总线，搬迁本身
// "编译过就算对了" —— 但三种错都不会崩:
//   1. 门面上那些信号忘了删 → 门面照旧背着一份与播放无关的契约（白搬）；
//   2. 总线上的信号名/参数被改 → 编译期可能靠隐式转换糊过去，UI 收的是错的值；
//   3. 控制器继续发自己那份信号而没发到总线上 → UI 一条也收不到，
//      进度框永远不弹 / 结构页一直空着（静默失效）。
//
// 盯四件事:
//   1. 门面不再声明这些信号（元对象层面断言，不是看代码）；
//   2. 总线上契约齐全（名字 + 参数签名）；
//   3. export_bus() / container_bus() 稳定返回同一个对象，且归属 UI 线程；
//   4. 控制器真的往总线发: 无媒体时 StartVideoFrameExport 必须经总线报 Error，
//      且不得发 Started（终态互斥）；容器分析对不存在的文件必须经总线报 Failed。
//
// 用 QCoreApplication 即可（不涉及控件），不需要 offscreen。

#include <gtest/gtest.h>

#include <functional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaObject>
#include <QString>
#include <QThread>

#include "core/player/AnalysisBus.h"
#include "core/player/ContainerBus.h"
#include "core/player/ExportBus.h"
#include "core/player/MediaPlayer.h"

namespace {

using videoeye::player::AnalysisBus;
using videoeye::player::ContainerBus;
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

const char* const kContainerSignatures[] = {
    "ContainerStructureReady(videoeye::model::ContainerStructureResult)",
    "ContainerStructureFailed(QString)",
};

const char* const kAnalysisSignatures[] = {
    "VideoFrameListReset()",
    "AudioFrameListReset()",
    "PacketListReset()",
    "AnalysisEventListReset()",
    "SyncSampleListReset()",
    "TimelineEventListReset()",
    "StreamStatsReady(videoeye::StreamStats)",
    "VideoFrameInfoReady(int,int,bool,qint64,double)",
    "AudioFrameInfoReady(int,qint64,double,int,int,int,int)",
    "PacketInfoReady(model::PacketInfo)",
    "AnalysisEventReady(model::AnalysisEvent)",
    "SyncSampleReady(model::SyncSample)",
    "TimelinePacketReady(model::PacketTiming)",
    "FrameTimingReady(model::FrameTimingInfo)",
    "TimelineEventReady(model::TimelineEvent)",
    "AudioVisualizationReady(model::AudioVisualizationFrame)",
    "AudioLevelReady(double,double)",
    "MacroblockInfoReady(videoeye::model::MacroblockFrameAnalysis)",
    "SceneChangeReady(model::SceneChangeResult)",
    "VisualDefectReset()",
    "VisualDefectFrameReady(model::FrameQualityMetric)",
    "VisualDefectReady(model::VisualDefect)",
    "VisualDefectStatsReady(int,int,model::ActivePictureArea)",
};

// 泵事件直到条件成立（后台任务的结果要排回 UI 线程才落地）。
bool PumpUntil(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return done();
}

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

// ---------------------------------------------------------------------------
// 5. 门面不再背容器结构分析的两条信号
// ---------------------------------------------------------------------------
TEST(ContainerBusTests, FacadeDoesNotDeclareContainerSignalsAnymore) {
    for (const char* sig : kContainerSignatures) {
        EXPECT_EQ(-1, SignalIndex(MediaPlayer::staticMetaObject, sig))
            << "MediaPlayer 还在声明容器信号 " << sig << " —— 它该在 ContainerBus 上";
        EXPECT_GE(SignalIndex(ContainerBus::staticMetaObject, sig), 0)
            << "ContainerBus 缺少信号 " << sig;
    }
}

// ---------------------------------------------------------------------------
// 6. 控制器真的往总线发: 打不开的文件 → Failed，且不得发 Ready（两条互斥）
// ---------------------------------------------------------------------------
TEST(ContainerBusTests, ControllerEmitsFailureThroughBusForUnreadableFile) {
    AppScope app_scope;

    const QString missing = QDir::tempPath() + "/videoeye_bus_no_such_file.mkv";
    QFile::remove(missing);

    MediaPlayer player;
    int ready = 0;
    int failed = 0;
    QString failure_message;
    QObject::connect(&player.container_bus(), &ContainerBus::ContainerStructureReady, &player,
                     [&](const videoeye::model::ContainerStructureResult&) { ++ready; });
    QObject::connect(&player.container_bus(), &ContainerBus::ContainerStructureFailed, &player,
                     [&](const QString& msg) {
                         ++failed;
                         failure_message = msg;
                     });

    player.RequestContainerStructureAnalysis(missing);

    ASSERT_TRUE(PumpUntil([&] { return failed > 0 || ready > 0; }, 10000))
        << "ContainerInspectionController 必须经总线给出终态, 否则结构页会一直空着";
    EXPECT_EQ(1, failed) << "打不开的文件必须报 Failed";
    EXPECT_EQ(0, ready) << "两条信号互斥: 失败时不得发 Ready";
    EXPECT_FALSE(failure_message.trimmed().isEmpty()) << "失败信号必须带原因";

    QFile::remove(missing);
}

// ---------------------------------------------------------------------------
// 7. 门面不再背实时分析的 23 条信号，总线上一条不少
// ---------------------------------------------------------------------------
TEST(AnalysisBusTests, FacadeDoesNotDeclareAnalysisSignalsAnymore) {
    for (const char* sig : kAnalysisSignatures) {
        EXPECT_EQ(-1, SignalIndex(MediaPlayer::staticMetaObject, sig))
            << "MediaPlayer 还在声明分析信号 " << sig << " —— 它该在 AnalysisBus 上";
        EXPECT_GE(SignalIndex(AnalysisBus::staticMetaObject, sig), 0)
            << "AnalysisBus 缺少信号 " << sig;
    }
}

// ---------------------------------------------------------------------------
// 8. analysis_bus() 稳定 + 归属 UI 线程
// ---------------------------------------------------------------------------
TEST(AnalysisBusTests, BusHandleIsStableAndLivesOnUiThread) {
    AppScope app_scope;

    MediaPlayer player;
    AnalysisBus& first = player.analysis_bus();
    EXPECT_EQ(&first, &player.analysis_bus()) << "analysis_bus() 每次都得给出同一个对象";
    EXPECT_TRUE(first.parent() != nullptr) << "总线必须有宿主, 不能是游离对象";
    EXPECT_EQ(player.thread(), first.thread()) << "总线要跟播放器同线程";
}
