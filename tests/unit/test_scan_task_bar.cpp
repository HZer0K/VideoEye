// 扫描任务条的回归（ui/main_window/ScanTaskBar）。
//
// 这条栏要解决的是"任务状态和控制分居两页"：扫描由「质量诊断」页编排，取消按钮
// 也在那一页，用户在首页看到"分析中…"却被文案指去另一页找按钮。所以它必须做到
// 三件事，分开测：
//   1. 扫描期间出现、结束即隐藏（不能留一条"0%"挂在那里）；
//   2. 取消是一次意图（发一次信号、立刻进"取消中"并置灰按钮），**不伪造终态**；
//   3. 取消中不再被进度更新覆盖文案 —— 进度条还在往前爬会让用户以为取消没被受理。
//
// 只构造控件（offscreen），不依赖主窗口。

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

#include "ui/main_window/ScanTaskBar.h"

using videoeye::ui::ScanTaskBar;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_scan_task_bar";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

QProgressBar* Progress(QWidget* bar) {
    return bar->findChild<QProgressBar*>(QStringLiteral("ScanTaskProgress"));
}

QLabel* Stage(QWidget* bar) {
    return bar->findChild<QLabel*>(QStringLiteral("ScanTaskStage"));
}

QPushButton* CancelButton(QWidget* bar) {
    for (QPushButton* button : bar->findChildren<QPushButton*>()) {
        if (button->text().contains(QStringLiteral("取消")))
            return button;
    }
    return nullptr;
}

} // namespace

// 1) 空闲时不占位；扫描开始出现并显示阶段与进度
TEST(ScanTaskBarTests, HiddenWhenIdleAndShowsStageWhileRunning) {
    EnsureApp();
    ScanTaskBar bar;
    EXPECT_TRUE(bar.isHidden()) << "没在扫描时不该常驻一条空栏";

    bar.SetScanState(true, 42.0, QStringLiteral("解码中"));
    EXPECT_FALSE(bar.isHidden());
    ASSERT_NE(Progress(&bar), nullptr);
    EXPECT_EQ(Progress(&bar)->value(), 42);
    ASSERT_NE(Stage(&bar), nullptr);
    EXPECT_TRUE(Stage(&bar)->text().contains(QStringLiteral("解码中")));

    // 阶段文案为空时兜一个通用语：空白条比"正在分析"更像卡死
    bar.SetScanState(true, 60.0, QString());
    EXPECT_FALSE(Stage(&bar)->text().isEmpty());
    EXPECT_EQ(Progress(&bar)->value(), 60);

    // 终态：隐藏，且"取消中"标记复位（下一次扫描是全新状态）
    bar.SetScanState(false, 0.0, QString());
    EXPECT_TRUE(bar.isHidden());
    EXPECT_FALSE(bar.IsCancelling());
}

// 2) 取消：只发一次意图、立刻进"取消中"、按钮置灰，并且不再被进度覆盖
TEST(ScanTaskBarTests, CancelEmitsOnceAndFreezesTheWaitingCopy) {
    EnsureApp();
    ScanTaskBar bar;
    bar.SetScanState(true, 10.0, QStringLiteral("读取索引"));

    int cancels = 0;
    QObject::connect(&bar, &ScanTaskBar::CancelRequested, [&cancels] { ++cancels; });

    QPushButton* button = CancelButton(&bar);
    ASSERT_NE(button, nullptr);
    ASSERT_TRUE(button->isEnabled());
    button->click();

    EXPECT_EQ(cancels, 1);
    EXPECT_TRUE(bar.IsCancelling());
    EXPECT_FALSE(button->isEnabled()) << "取消请求已受理，按钮不该还能再点";
    const QString waiting = Stage(&bar)->text();
    EXPECT_TRUE(waiting.contains(QStringLiteral("取消中"))) << waiting.toStdString();

    // 后台收尾期间进度仍在推进：它不能把等待文案盖回去
    bar.SetScanState(true, 55.0, QStringLiteral("收尾"));
    EXPECT_EQ(Stage(&bar)->text(), waiting);
    // 再点一次也不该重复发（按钮已置灰，直接调 click 也只是空转）
    button->click();
    EXPECT_EQ(cancels, 1);
}

// 3) 取消中到达终态：整条隐藏、标记复位，下一次扫描从干净状态开始
TEST(ScanTaskBarTests, TerminalStateResetsTheCancellingFlag) {
    EnsureApp();
    ScanTaskBar bar;
    bar.SetScanState(true, 5.0, QStringLiteral("准备中"));
    CancelButton(&bar)->click();
    ASSERT_TRUE(bar.IsCancelling());

    bar.SetScanState(false, 0.0, QString());
    EXPECT_FALSE(bar.IsCancelling());
    EXPECT_TRUE(bar.isHidden());

    // 新一次扫描：按钮重新可用、文案回到正常阶段
    bar.SetScanState(true, 1.0, QStringLiteral("重新开始"));
    EXPECT_FALSE(bar.isHidden());
    QPushButton* button = CancelButton(&bar);
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(button->isEnabled());
    EXPECT_TRUE(Stage(&bar)->text().contains(QStringLiteral("重新开始")));
}
