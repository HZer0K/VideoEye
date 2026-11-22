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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTextBrowser>

#include <vector>

#include "core/ffmpeg/FfmpegChangeGraph.h"
#include "core/ffmpeg/FfmpegCommandExplainer.h"
#include "core/ffmpeg/FfmpegCommandParser.h"
#include "ui/ffmpeg_panel/FfmpegChangeGraphView.h"
#include "ui/ffmpeg_panel/FfmpegOutputWidget.h"

using videoeye::ffmpeg::CommandParseStatus;
using videoeye::ffmpeg::FfmpegCommandExplainer;
using videoeye::ffmpeg::FfmpegCommandExplanation;
using videoeye::ffmpeg::FfmpegProgressSnapshot;
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

// 页签固定为 0=命令解释 / 1=运行日志 / 2=错误输出 / 3=转换图谱（测试按位置取页）。
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

} // namespace
// ---------------------------------------------------------------------------
// 1. 构造：组标题 + 四个页签 + 状态行初值
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, ConstructorBuildsFourTabsAndIdleStatus) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);

    // 组件就是 QGroupBox 本身：标题必须还是「输出与解释」（拆分类的视觉锚点）
    EXPECT_EQ(QStringLiteral("输出与解释"), widget.title());
    ASSERT_EQ(4, tabs->count());
    EXPECT_EQ(QStringLiteral("命令解释"), tabs->tabText(0));
    EXPECT_EQ(QStringLiteral("运行日志"), tabs->tabText(1));
    EXPECT_EQ(QStringLiteral("错误输出"), tabs->tabText(2));
    // 第四页是"这次运行改变了什么"，它与日志并列而不是塞进日志页
    EXPECT_EQ(QStringLiteral("转换图谱"), tabs->tabText(3));
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
    // 日志是攒批写进视图的（50ms），要立刻看到就得显式落地
    widget.FlushLog();

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
    widget.AppendLog(QStringLiteral("boom"), true); // 错误页非空才值得切过去

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
// ---------------------------------------------------------------------------
// 7. 覆盖被拒：退出码 0 也不能报「完成」
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, OverwriteRefusalIsNotReportedAsFinished) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    ASSERT_TRUE(log != nullptr);

    std::vector<QString> messages;
    QObject::connect(&widget, &FfmpegOutputWidget::StatusMessage,
                     [&messages](const QString& text) { messages.push_back(text); });

    // ffmpeg 在"输出文件已存在"时的退出码是 0，但它一个字节都没写 ——
    // 报"完成"会让用户以为产出已经更新，进而拿旧文件当新结果。
    FfmpegRunResult refused;
    refused.status = FfmpegRunStatus::Finished;
    refused.exit_code = 0;
    refused.overwrite_refused = true;
    widget.ShowRunResult(refused);

    QLabel* status = LabelWithPrefix(widget, QStringLiteral("状态: "));
    ASSERT_TRUE(status != nullptr);
    EXPECT_TRUE(status->text().contains(QStringLiteral("未写入")));
    EXPECT_FALSE(status->text().contains(QStringLiteral("完成")));
    // "原文件没被动过"必须说出来，否则用户会以为这次是失败而不是"什么都没做"
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("没有改写任何数据")));
    ASSERT_EQ(1u, messages.size());
    EXPECT_TRUE(messages[0].contains(QStringLiteral("没有写入")));
}

// ---------------------------------------------------------------------------
// 8. 进度行：有总时长才报百分比，没有就说"总时长未知"
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, ProgressShowsPercentOnlyWhenDurationIsKnown) {
    EnsureApp();

    FfmpegOutputWidget widget;
    widget.PrepareForRun();

    QLabel* progress = LabelWithPrefix(widget, QStringLiteral("进度: "));
    ASSERT_TRUE(progress != nullptr);
    EXPECT_EQ(QStringLiteral("进度: —"), progress->text());

    FfmpegProgressSnapshot normal;
    normal.percent = 42.5;
    normal.position_ms = 4000;
    normal.duration_ms = 10000;
    widget.ShowProgress(normal);
    EXPECT_TRUE(progress->text().contains(QStringLiteral("42.5%")));
    EXPECT_TRUE(progress->text().contains(QStringLiteral("00:04 / 00:10")));

    // 流式输入（Duration: N/A）：没有分母就不给百分比，只报已处理时长
    FfmpegProgressSnapshot streaming;
    streaming.position_ms = 7000;
    widget.ShowProgress(streaming);
    EXPECT_TRUE(progress->text().contains(QStringLiteral("总时长未知")));
    EXPECT_TRUE(progress->text().contains(QStringLiteral("00:07")));
    EXPECT_FALSE(progress->text().contains(QLatin1Char('%')));
}

// ---------------------------------------------------------------------------
// 9. 跑完了不能把最后一个 99.x% 留在状态行上
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, FinishedRunReplacesProgressWithCompleted) {
    EnsureApp();

    FfmpegOutputWidget widget;
    widget.PrepareForRun();
    FfmpegProgressSnapshot near_done;
    near_done.percent = 99.4;
    near_done.position_ms = 9940;
    near_done.duration_ms = 10000;
    widget.ShowProgress(near_done);

    FfmpegRunResult result;
    result.status = FfmpegRunStatus::Finished;
    result.exit_code = 0;
    widget.ShowRunResult(result);

    QLabel* progress = LabelWithPrefix(widget, QStringLiteral("进度: "));
    ASSERT_TRUE(progress != nullptr);
    EXPECT_EQ(QStringLiteral("进度: 完成"), progress->text());
}

// ---------------------------------------------------------------------------
// 10. 日志过滤：只改显示，清空后能完整撤回
// ---------------------------------------------------------------------------

namespace {

QLineEdit* FilterEdit(FfmpegOutputWidget& widget) {
    return widget.findChild<QLineEdit*>();
}

void FeedLines(FfmpegOutputWidget& widget) {
    widget.AppendLog(QStringLiteral("frame= 10"), false);
    widget.AppendLog(QStringLiteral("Invalid argument"), true);
    widget.AppendLog(QStringLiteral("frame= 20"), false);
    widget.FlushLog();
}

} // namespace

TEST(FfmpegOutputWidgetTests, LogFilterHidesNonMatchingLinesAndRestoresOnClear) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    QPlainTextEdit* error = TabTextEdit(tabs, 2);
    ASSERT_TRUE(log != nullptr);
    ASSERT_TRUE(error != nullptr);
    ASSERT_TRUE(FilterEdit(widget) != nullptr);

    FeedLines(widget);

    widget.SetLogFilter(QStringLiteral("frame"));
    // 命中的留下，没命中的消失 —— 两个页签用的是同一份历史，各筛各的
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("frame= 10")));
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("frame= 20")));
    EXPECT_FALSE(log->toPlainText().contains(QStringLiteral("Invalid argument")));
    EXPECT_TRUE(error->toPlainText().isEmpty()) << error->toPlainText().toStdString();

    // 清空过滤：内容必须完全回来（历史不是为了显示才留的）
    widget.SetLogFilter(QString());
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("[err] Invalid argument")));
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("frame= 10")));
    EXPECT_TRUE(error->toPlainText().contains(QStringLiteral("Invalid argument")));
}

TEST(FfmpegOutputWidgetTests, LogFilterIgnoresCase) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* error = TabTextEdit(tabs, 2);
    ASSERT_TRUE(error != nullptr);

    FeedLines(widget);

    // ffmpeg 的报错是 "Invalid ..."，用户不会先记住大小写再搜
    widget.SetLogFilter(QStringLiteral("invalid"));
    EXPECT_TRUE(error->toPlainText().contains(QStringLiteral("Invalid argument")));
}

TEST(FfmpegOutputWidgetTests, LinesArrivingWhileFilteredAreStillKeptInHistory) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    ASSERT_TRUE(log != nullptr);

    widget.SetLogFilter(QStringLiteral("frame"));
    // 运行中才到的行：不匹配就当没发生？不行 —— 清空过滤后它必须还在
    widget.AppendLog(QStringLiteral("Error opening input: No such file or directory"), true);
    widget.AppendLog(QStringLiteral("frame= 30"), false);
    widget.FlushLog();

    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("frame= 30")));
    EXPECT_FALSE(log->toPlainText().contains(QStringLiteral("No such file")));

    widget.SetLogFilter(QString());
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("No such file")));
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("frame= 30")));
}

TEST(FfmpegOutputWidgetTests, FilterCountFollowsCurrentTab) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    ASSERT_TRUE(FilterEdit(widget) != nullptr);

    FeedLines(widget);
    tabs->setCurrentIndex(1); // 运行日志：3 行，命中 2 行
    widget.SetLogFilter(QStringLiteral("frame"));

    QLabel* count = nullptr;
    for (QLabel* label : widget.findChildren<QLabel*>()) {
        if (label->text().startsWith(QStringLiteral("匹配 "))) {
            count = label;
        }
    }
    ASSERT_TRUE(count != nullptr);
    EXPECT_EQ(QStringLiteral("匹配 2 / 共 3 行"), count->text());

    // 切到错误输出：那边只有 1 行 stderr，一条都不命中 —— 「0 / 1」比空屏有用
    tabs->setCurrentIndex(2);
    EXPECT_EQ(QStringLiteral("匹配 0 / 共 1 行"), count->text());
}

TEST(FfmpegOutputWidgetTests, EscapeClearsFilter) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* log = TabTextEdit(tabs, 1);
    QLineEdit* edit = FilterEdit(widget);
    ASSERT_TRUE(log != nullptr);
    ASSERT_TRUE(edit != nullptr);

    FeedLines(widget);
    widget.SetLogFilter(QStringLiteral("frame"));
    ASSERT_FALSE(log->toPlainText().contains(QStringLiteral("Invalid argument")));

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(edit, &escape);

    EXPECT_TRUE(edit->text().isEmpty());
    EXPECT_TRUE(log->toPlainText().contains(QStringLiteral("Invalid argument")));
}

// ---------------------------------------------------------------------------
// 11. 诊断不受过滤影响：过滤词把报错筛没了也不能没有结论
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, DiagnosisSurvivesAStaleFilter) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);
    QPlainTextEdit* error = TabTextEdit(tabs, 2);
    ASSERT_TRUE(error != nullptr);

    widget.PrepareForRun();
    widget.AppendLog(QStringLiteral("Error opening input: No such file or directory"), true);
    widget.FlushLog();

    // 上一轮留下的过滤词，恰好一条 ffmpeg 输出都不匹配
    widget.SetLogFilter(QStringLiteral("zzz-nothing-matches"));
    EXPECT_TRUE(error->toPlainText().isEmpty());

    std::vector<QString> messages;
    QObject::connect(&widget, &FfmpegOutputWidget::StatusMessage,
                     [&messages](const QString& text) { messages.push_back(text); });

    FfmpegRunResult result;
    result.status = FfmpegRunStatus::Failed;
    result.exit_code = 1;
    widget.ShowRunResult(result);

    // 诊断是 VideoEye 的结论，必须穿透过滤显示出来
    EXPECT_TRUE(error->toPlainText().contains(QStringLiteral("诊断"))) << error->toPlainText().toStdString();
    ASSERT_FALSE(messages.empty());
    EXPECT_TRUE(messages.back().contains(QStringLiteral("路径不存在"))) << messages.back().toStdString();
}

// ---------------------------------------------------------------------------
// 14. 转换图谱页：不参与日志过滤，且新一轮运行不许留着上一轮的图
// ---------------------------------------------------------------------------

TEST(FfmpegOutputWidgetTests, ChangeGraphTabHasNoLogFilterAndResetsOnNewRun) {
    EnsureApp();

    FfmpegOutputWidget widget;
    QTabWidget* tabs = widget.findChild<QTabWidget*>();
    ASSERT_TRUE(tabs != nullptr);

    QLineEdit* filter_edit = FilterEdit(widget);
    ASSERT_TRUE(filter_edit != nullptr);
    QWidget* filter_bar = filter_edit->parentWidget();
    ASSERT_TRUE(filter_bar != nullptr);

    // 过滤栏只在两个日志页签出现：图谱页显示的是解析出来的结构，没有"日志行"可筛。
    tabs->setCurrentIndex(1);
    EXPECT_TRUE(filter_bar->isVisibleTo(&widget));
    tabs->setCurrentIndex(3);
    EXPECT_FALSE(filter_bar->isVisibleTo(&widget));

    videoeye::ffmpeg::ChangeGraph graph;
    graph.ok = true;
    graph.target = QStringLiteral("out.mp4");
    graph.output_measured = false;
    widget.ShowChangeGraphs({graph});
    EXPECT_FALSE(tabs->tabToolTip(3).isEmpty());

    // 新一轮运行：上一轮的图不能留着冒充这次的结果
    widget.PrepareForRun();
    EXPECT_TRUE(tabs->tabToolTip(3).isEmpty());
    EXPECT_FALSE(widget.ChangeGraphView()->HasGraph());
    EXPECT_FALSE(widget.ChangeGraphView()->EmptyHintText().isEmpty());
}
