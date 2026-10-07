// ExportCoordinator（MainWindow 导出职责抽出）的接缝测试。
//
// 为什么值得单独测: 导出三入口 + 十条播放器导出信号接线 + 进度框「建 / 复用 / 收」
// 从 MainWindow 整体搬来。这类接线写错都不崩 —— 只会静默: 进度框不弹 / 不复用 /
// 不收, 帧导出与音视频导出的量程串味, 取消按钮路由到错误的取消接口, 提示文案漂移。
//
// 盯六件事:
//   1. 构造惰性: 不建进度框; HideProgress 无框时安全。
//   2. 视频帧 Started: 建框 (文案 / 「终止」按钮 / 模态 / autoClose / autoReset);
//      二次 Started 复用同一个框, 只改量程。
//   3. 帧进度: 超量程夹紧; 总数未知进 busy 档 (量程 0); 状态栏文案同步。
//   4. 帧 Finished: 收起进度框 + 「导出完成: 路径」。
//   5. 音视频 Started/Progress: 量程固定 100, 只动框、不刷状态栏。
//   6. 取消按钮: 点「终止」发「正在终止导出...」并路由到播放器取消接口
//      (播放器真实构造但从不打开媒体 —— 空闲时 Cancel 安全)。
//
// 驱动方式: 处理器是 private slots, 用 QMetaObject::invokeMethod 按名驱动
// (先例 tests/unit/test_reporting_panel_lifecycle.cpp)。信号本身是播放器侧契约,
// 不经过 MediaPlayer 发射。
//
// 不被测的路径 (会弹模态框, 单测里永久阻塞): 三个导出入口 (QMessageBox /
// QFileDialog / QInputDialog / MediaExportDialog::exec 链), 以及 Canceled / Error /
// 音视频 Finished 的收尾分支 (QMessageBox 确认框)。这些留给手工冒烟。
//
// 用 offscreen 平台跑, 不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QProgressDialog>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <vector>

#include "core/player/MediaPlayer.h"
#include "ui/main_window/ExportCoordinator.h"

namespace {

using videoeye::player::MediaPlayer;
using videoeye::ui::ExportCoordinator;

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_export_coordinator";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 记录协调器发出的状态栏文本 (含 timeout: 0 是临时消息的默认档)。
struct StatusSpy {
    std::vector<QString> texts;
    std::vector<int> timeouts;

    QString Last() const { return texts.empty() ? QString() : texts.back(); }

    void Attach(ExportCoordinator& coord) {
        QObject::connect(&coord, &ExportCoordinator::StatusMessage, &coord,
                         [this](const QString& text, int timeout) {
                             texts.push_back(text);
                             timeouts.push_back(timeout);
                         });
    }
};

// 被测体 + 宿主: 进度框 parent 是 host (扮演 MainWindow 的 dialog_parent 角色)。
struct Harness {
    QWidget host;
    MediaPlayer player;
    ExportCoordinator coord{&player, &host};
    StatusSpy status;

    Harness() { status.Attach(coord); }

    QProgressDialog* Dialog() const { return host.findChild<QProgressDialog*>(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. 构造惰性 + HideProgress 无框安全
// ---------------------------------------------------------------------------

TEST(ExportCoordinatorTests, ConstructorIsLazyAndHideProgressIsSafe) {
    EnsureApp();

    Harness h;
    EXPECT_EQ(nullptr, h.Dialog());       // 构造不建进度框
    h.coord.HideProgress();               // 无框时安全
    EXPECT_EQ(nullptr, h.Dialog());
    EXPECT_TRUE(h.status.texts.empty());  // 不发任何提示
}

// ---------------------------------------------------------------------------
// 2. 视频帧 Started: 建框 / 复用
// ---------------------------------------------------------------------------

TEST(ExportCoordinatorTests, VideoFrameStartedBuildsThenReusesDialog) {
    EnsureApp();

    Harness h;
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportStarted", Q_ARG(int, 3)));

    QProgressDialog* dlg = h.Dialog();
    ASSERT_NE(nullptr, dlg);
    EXPECT_TRUE(dlg->isVisible());
    EXPECT_EQ(3, dlg->maximum());
    EXPECT_EQ(0, dlg->value());
    EXPECT_EQ(QStringLiteral("正在导出视频帧..."), dlg->labelText());
    // 本版 Qt 的 QProgressDialog 只有 setCancelButtonText 没有 getter, 查按钮验证文案
    QPushButton* cancel_btn = dlg->findChild<QPushButton*>();
    ASSERT_NE(nullptr, cancel_btn);
    EXPECT_EQ(QStringLiteral("终止"), cancel_btn->text());
    EXPECT_EQ(Qt::ApplicationModal, dlg->windowModality());
    EXPECT_FALSE(dlg->autoClose());
    EXPECT_FALSE(dlg->autoReset());

    // 二次 Started 复用同一个框, 只改量程 (原 MainWindow if/else 语义)
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportStarted", Q_ARG(int, 250)));
    EXPECT_EQ(dlg, h.Dialog());
    EXPECT_EQ(250, dlg->maximum());
    EXPECT_EQ(0, dlg->value());
}

// ---------------------------------------------------------------------------
// 3. 帧进度: 夹紧 / busy 档 / 状态栏文案
// ---------------------------------------------------------------------------

TEST(ExportCoordinatorTests, FrameProgressClampsToTotalAndFallsBackToBusy) {
    EnsureApp();

    Harness h;
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportStarted", Q_ARG(int, 100)));
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportProgress", Q_ARG(int, 42)));

    QProgressDialog* dlg = h.Dialog();
    ASSERT_NE(nullptr, dlg);
    EXPECT_EQ(100, dlg->maximum());
    EXPECT_EQ(42, dlg->value());
    EXPECT_EQ(QStringLiteral("已导出 42 帧"), dlg->labelText());
    ASSERT_EQ(1u, h.status.texts.size());
    EXPECT_EQ(QStringLiteral("已导出 42 帧"), h.status.Last());
    EXPECT_EQ(0, h.status.timeouts.back());

    // 超额进度: 值夹到量程 (原实现 std::min 语义)
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportProgress", Q_ARG(int, 999)));
    EXPECT_EQ(100, dlg->value());

    // 总数未知: 量程 0 (busy 档), 值固定 0
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportStarted", Q_ARG(int, 0)));
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportProgress", Q_ARG(int, 7)));
    EXPECT_EQ(0, dlg->maximum());
    EXPECT_EQ(0, dlg->value());
    EXPECT_EQ(QStringLiteral("已导出 7 帧"), dlg->labelText());
}

// ---------------------------------------------------------------------------
// 4. 帧 Finished: 收起进度框 + 完成文案
// ---------------------------------------------------------------------------

TEST(ExportCoordinatorTests, FrameFinishedHidesProgressAndReportsPath) {
    EnsureApp();

    Harness h;
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportStarted", Q_ARG(int, 10)));
    QProgressDialog* dlg = h.Dialog();
    ASSERT_NE(nullptr, dlg);
    ASSERT_TRUE(dlg->isVisible());

    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportFinished",
                                          Q_ARG(QString, QStringLiteral("D:/videoeye_out"))));
    EXPECT_FALSE(dlg->isVisible());
    ASSERT_EQ(1u, h.status.texts.size());
    EXPECT_EQ(QStringLiteral("导出完成: D:/videoeye_out"), h.status.Last());
}

// ---------------------------------------------------------------------------
// 5. 音视频 Started/Progress: 量程 100, 不刷状态栏
// ---------------------------------------------------------------------------

TEST(ExportCoordinatorTests, MediaProgressDrivesDialogWithoutStatusSpam) {
    EnsureApp();

    Harness h;
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnMediaExportStarted", Q_ARG(qint64, 5000)));

    QProgressDialog* dlg = h.Dialog();
    ASSERT_NE(nullptr, dlg);
    EXPECT_TRUE(dlg->isVisible());
    EXPECT_EQ(100, dlg->maximum());
    EXPECT_EQ(0, dlg->value());
    EXPECT_EQ(QStringLiteral("正在导出音视频..."), dlg->labelText());

    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnMediaExportProgress", Q_ARG(int, 42)));
    EXPECT_EQ(100, dlg->maximum());
    EXPECT_EQ(42, dlg->value());
    EXPECT_EQ(QStringLiteral("正在导出音视频... 42%"), dlg->labelText());
    EXPECT_TRUE(h.status.texts.empty());  // 音视频进度只动框, 不刷状态栏
}

// ---------------------------------------------------------------------------
// 6. 取消按钮: 发终止提示 + 路由到播放器取消接口 (空闲安全)
// ---------------------------------------------------------------------------

TEST(ExportCoordinatorTests, CancelButtonRequestsTerminationAndReports) {
    EnsureApp();

    Harness h;
    ASSERT_TRUE(QMetaObject::invokeMethod(&h.coord, "OnVideoFrameExportStarted", Q_ARG(int, 10)));

    QProgressDialog* dlg = h.Dialog();
    ASSERT_NE(nullptr, dlg);
    QPushButton* cancel_btn = dlg->findChild<QPushButton*>();
    ASSERT_NE(nullptr, cancel_btn);
    EXPECT_EQ(QStringLiteral("终止"), cancel_btn->text());

    cancel_btn->click();  // 与用户点「终止」同一路径 (QProgressDialog::canceled -> 协调器)
    ASSERT_EQ(1u, h.status.texts.size());
    EXPECT_EQ(QStringLiteral("正在终止导出..."), h.status.Last());
}