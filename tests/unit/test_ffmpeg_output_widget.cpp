// FfmpegOutputWidget（FFmpeg 命令工作台「输出与解释」区）的接缝测试。
//
// 为什么值得单独测：这一区从 FfmpegPanel 抽出，把「命令一改就重算解释」「运行输出按
// 到达顺序合并两路、stderr 行加 [err] 前缀」「失败自动切到错误页」「优雅停止与被强杀
// 分开报」四条规则全压在组件内部 —— 它们写错都不崩，只会静默显示旧值 / 错值，或者
// 把"输出可能不可用"的强杀结果报成正常完成。
//
// 具体盯六件事：
//   1. 构造：组标题不变（拆分类 QGroupBox 的视觉锚点）、三个页签文案与默认页、状态行初值。
//   2. RefreshExplanation：列表条数与 Explain 一致、默认提示"点击上方任意一项"；
//      空命令安静清空、非法命令只显示错误说明。
//   3. AppendLog：stdout 不加前缀进「运行日志」；stderr 加 [err] 进「运行日志」且
//      原文进「错误输出」；两个通道不串。
//   4. PrepareForRun：清两本日志、状态置"运行中"、切回「运行日志」页。
//   5. Failed 终态：状态行含"失败"、自动切到「错误输出」页、StatusMessage 指向错误页。
//   6. Stopped 终态：QuitSent → "正常收尾"且无警告；Killed → "输出可能不可用"且有警告。
//      （用 QuitSent / Killed 两档对比，避开 Terminated 在 Windows 与 Unix 上语义不同。）
//
// 用 offscreen 平台跑，不需要显示器。

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTextBrowser>

#include <vector>

#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "ui/ffmpeg_panel/FfmpegOutputWidget.h"

using videoeye::ffmpeg::CommandParseStatus;
using videoeye::ffmpeg::FfmpegCommandExplanation;
using videoeye::ffmpeg::FfmpegCommandExplainer;
using videoeye::ffmpeg::FfmpegRunResult;
using videoeye::ffmpeg::FfmpegRunStatus;
using videoeye::ffmpeg::FfmpegStopStage;
using videoeye::ffmpeg::ParsedCommand;
using videoeye::ui::FfmpegOutputWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_ffmpeg_output_widget";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 页签固定为 0=命令解释 / 1=运行日志 / 2=错误输出（测试按位置取页）。
QPlainTextEdit* TabTextEdit(QTabWidget* tabs, int index) {
    return tabs ? qobject_cast<QPlainTextEdit*>(tabs->widget(index)) : nullptr;
}

// 状态行三个 QLabel 没有 objectName，用稳定前缀识别（文案前缀本身就是被测契约的一部分）。
QLabel* LabelWithPrefix(FfmpegOutputWidget& widget, const QString& prefix) {
    for (QLabel* label : widget.findChildren<QLabel*>()) {
        if (label->text().startsWith(prefix)) {
            return label;
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 构造：组标题 + 三个页签 + 状态行初值
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, ConstructorBuildsThreeTabsAndIdleStatus) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);

    // 组件就是 QGroupBox 本身：标题必须还是「输出与解释」（拆分类的视觉锚点）
    EXPECT_EQ(QStringLiteral("输出与解释"), widget.title());
    ASSERT_EQ(3, tabs->count());
    EXPECT_EQ(QStringLiteral("命令解释"), tabs->tabText(0));
    EXPECT_EQ(QStringLiteral("运行日志"), tabs->tabText(1));
    EXPECT_EQ(QStringLiteral("错误输出"), tabs->tabText(2));
    EXPECT_EQ(0, tabs->currentIndex());

    QLabel* status = LabelWithPrefix(widget, QStringLiteral("状态: "));
    QLabel* exit_code = LabelWithPrefix(widget, QStringLiteral("退出码: "));
    QLabel* elapsed = LabelWithPrefix(widget, QStringLiteral("耗时: "));
    ASSERT_TRUE(status != nullptr);
    ASSERT_TRUE(exit_code != nullptr);
    ASSERT_TRUE(elapsed != nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("未运行")));
    EXPECT_TRUE(exit_code->text().contains(QStringLiteral("—")));
    EXPECT_TRUE(elapsed->text().contains(QStringLiteral("—")));
}

// ---------------------------------------------------------------------------
// 2. RefreshExplanation：列表与 Explain 一致；空 / 非法命令不残留
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, RefreshExplanationMirrorsExplainerAndHandlesBadInput) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QListWidget* list = widget.findChild<QListWidget*>();
    QTextBrowser* detail = widget.findChild<QTextBrowser*>();
    ASSERT_TRUE(list != nullptr);
    ASSERT_TRUE(detail != nullptr);

    const QString command = QStringLiteral("ffmpeg -i in.mp4 -c:v libx264 -crf 23 out.mp4");
    widget.RefreshExplanation(command);

    const ParsedCommand parsed = videoeye::ffmpeg::ParseCommandLine(command);
    ASSERT_EQ(CommandParseStatus::Ok, parsed.status);
    const FfmpegCommandExplanation explanation = FfmpegCommandExplainer::Explain(parsed.arguments);
    EXPECT_GT(explanation.tokens.size(), 0u);
    EXPECT_EQ(static_cast<int>(explanation.tokens.size()), list->count());
    EXPECT_TRUE(detail->toPlainText().contains(QStringLiteral("点击上方任意一项")));

    // 空命令：安静清空，不留上一次的残影
    widget.RefreshExplanation(QString());
    EXPECT_EQ(0, list->count());
    EXPECT_TRUE(detail->toPlainText().isEmpty());

    // 非法命令（shell 操作符）：列表为空，详情给出错误说明
    widget.RefreshExplanation(QStringLiteral("ffmpeg -i in.mp4 | out.mp4"));
    EXPECT_EQ(0, list->count());
    EXPECT_FALSE(detail->toPlainText().isEmpty());
}

// ---------------------------------------------------------------------------
// 3. AppendLog：双通道分流 + [err] 前缀隔离
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, AppendLogKeepsChannelsSeparateAndMarksStderr) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    QPlainTextEdit* error = TabTextEdit(tabs, 2);
    ASSERT_TRUE(log != nullptr);
    ASSERT_TRUE(error != nullptr);

    widget.AppendLog(QStringLiteral("frame= 10"), false);
    widget.AppendLog(QStringLiteral("Invalid argument"), true);

    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("frame= 10")));
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("[err] Invalid argument")));
    EXPECT_FALSE(log->toPlainText().contains(QStringLiteral("[err] frame= 10")));
    EXPECT_TRUE(error->toPlainText().contains(QStringLiteral("Invalid argument")));
    EXPECT_FALSE(error->toPlainText().contains(QStringLiteral("frame= 10")));
    EXPECT_FALSE(error->toPlainText().contains(QStringLiteral("[err]")));
}

// ---------------------------------------------------------------------------
// 4. PrepareForRun：清日志 + 状态"运行中" + 切回「运行日志」
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, PrepareForRunClearsLogsAndSwitchesToLogTab) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    QPlainTextEdit* error = TabTextEdit(tabs, 2);
    ASSERT_TRUE(log != nullptr);
    ASSERT_TRUE(error != nullptr);

    widget.AppendLog(QStringLiteral("old output"), false);
    widget.AppendLog(QStringLiteral("old error"), true);
    tabs->setCurrentIndex(0);

    widget.PrepareForRun();

    EXPECT_TRUE(log->toPlainText().isEmpty());
    EXPECT_TRUE(error->toPlainText().isEmpty());
    EXPECT_EQ(1, tabs->currentIndex());
    QLabel* status = LabelWithPrefix(widget, QStringLiteral("状态: "));
    QLabel* exit_code = LabelWithPrefix(widget, QStringLiteral("退出码: "));
    QLabel* elapsed = LabelWithPrefix(widget, QStringLiteral("耗时: "));
    ASSERT_TRUE(status != nullptr);
    ASSERT_TRUE(exit_code != nullptr);
    ASSERT_TRUE(elapsed != nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("运行中")));
    EXPECT_EQ(QStringLiteral("退出码: —"), exit_code->text());
    EXPECT_EQ(QStringLiteral("耗时: —"), elapsed->text());
}

// ---------------------------------------------------------------------------
// 5. Failed 终态：切「错误输出」+ StatusMessage 指向错误页
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, FailedResultSwitchesToErrorTabAndAnnouncesFailure) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    ASSERT_TRUE(log != nullptr);

    widget.PrepareForRun();
    widget.AppendLog(QStringLiteral("boom"), true);   // 错误页非空才值得切过去

    std::vector<QString> messages;
    QObject::connect(&widget, &FfmpegOutputWidget::StatusMessage,
                     [&messages](const QString& text) { messages.push_back(text); });

    FfmpegRunResult result;
    result.status = FfmpegRunStatus::Failed;
    result.exit_code = 1;
    result.elapsed_ms = 1234;
    result.error_message = QStringLiteral("boom");
    widget.ShowRunResult(result);

    QLabel* status = LabelWithPrefix(widget, QStringLiteral("状态: "));
    QLabel* exit_code = LabelWithPrefix(widget, QStringLiteral("退出码: "));
    QLabel* elapsed = LabelWithPrefix(widget, QStringLiteral("耗时: "));
    ASSERT_TRUE(status != nullptr);
    ASSERT_TRUE(exit_code != nullptr);
    ASSERT_TRUE(elapsed != nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("失败")));
    EXPECT_EQ(QStringLiteral("退出码: 1"), exit_code->text());
    EXPECT_EQ(QStringLiteral("耗时: 1.2 s"), elapsed->text());
    EXPECT_EQ(2, tabs->currentIndex());
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("错误: boom")));
    ASSERT_EQ(1u, messages.size());
    EXPECT_TRUE(messages[0].contains(QStringLiteral("错误输出")));
}

// ---------------------------------------------------------------------------
// 6. Stopped 终态：优雅收尾 vs 被强杀，文案与警告必须分开
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, StoppedResultDistinguishesCleanExitFromForcedKill) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    ASSERT_TRUE(log != nullptr);

    std::vector<QString> messages;
    QObject::connect(&widget, &FfmpegOutputWidget::StatusMessage,
                     [&messages](const QString& text) { messages.push_back(text); });

    // 收到 q、ffmpeg 自己收尾：输出文件完整，不该吓唬用户
    FfmpegRunResult clean;
    clean.status = FfmpegRunStatus::Stopped;
    clean.stop_stage = FfmpegStopStage::QuitSent;
    clean.exit_code = 0;
    widget.ShowRunResult(clean);

    QLabel* status = LabelWithPrefix(widget, QStringLiteral("状态: "));
    ASSERT_TRUE(status != nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("正常收尾")));
    EXPECT_FALSE(log->toPlainText().contains(QStringLiteral("警告: ffmpeg 是被强制终止的")));
    ASSERT_EQ(1u, messages.size());
    EXPECT_TRUE(messages[0].contains(QStringLiteral("输出文件完整")));

    // 被强杀：mp4/mov 可能缺 moov box，必须给出"输出可能不可用"的警告
    FfmpegRunResult killed;
    killed.status = FfmpegRunStatus::Stopped;
    killed.stop_stage = FfmpegStopStage::Killed;
    killed.exit_code = 1;
    widget.ShowRunResult(killed);

    EXPECT_TRUE(status->text().contains(QStringLiteral("输出可能不可用")));
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("警告: ffmpeg 是被强制终止的")));
    ASSERT_EQ(2u, messages.size());
    EXPECT_TRUE(messages[1].contains(QStringLiteral("输出可能不可用")));
}