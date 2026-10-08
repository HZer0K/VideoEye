// DiagnosticsPage 的页面级行为测试（评审「页面组件补测」）。
//
// 这一页是「扫描总控」的落点，静默回归点：
//   1) 扫描终态三值枚举（Completed / Cancelled / Failed）**每次扫描恰好发一次**：
//      失败也必须发 —— 以前失败什么都不发，共用同一次扫描的几页永久停在扫描态
//      （取消按钮还亮着、"开始分析"永久禁用），与诊断页的"失败"互相矛盾；
//   2) 失败路径（silent=true）不弹模态框：原因写进汇总标签、按钮复位、导出禁用；
//   3) 成功路径（HLS 清单，纯 stdlib 解析，快且确定）端到端走到：
//      hasResult / 导出恢复 / 问题表与 qcReport() 逐行一致 / 汇总出评分；
//   4) 规则表：勾选开关与阈值改写落到 facade 规则集、非法阈值还原显示、
//      「恢复默认规则」重建；跨页钩子（BeforeScanHook）每次扫描前恰好跑一次；
//   5) CancelScan 只请求取消、不伪造终态；ResetForNewFile 回到初始态。
//
// 本页已收敛掉与「事件与时间轴」页重复的实时时间轴视图：只剩「问题清单 / 规则与阈值」
// 两个子页，单测只覆盖这两页与扫描总控状态。
//
// 只构造控件、不渲染（offscreen 平台）；导出按钮会弹 QFileDialog 模态框，
// 「未扫描就跳转问题位置 / 关联场景切换」会弹 QMessageBox，均不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>

#include <functional>
#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "ui/analysis_panel/DiagnosticsPage.h"
#include "ui/charts/MetricChartWidget.h"

using videoeye::model::DefaultQcRules;
using videoeye::model::QcReport;
using videoeye::model::QcRule;
using videoeye::ui::DiagnosticsPage;
using videoeye::ui::MetricChartWidget;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_diagnostics_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 泵事件直到条件满足；返回最终是否满足（扫描在工作线程里跑，靠排队信号回主线程）
bool PumpEventsUntil(const std::function<bool()>& done, int timeout_ms = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

// 页内唯一 QTabWidget 的第 tab_index 页里，按表头首列文案找表
QTableWidget* TableInTab(QWidget* root, int tab_index, const QString& first_header) {
    auto* tabs = root->findChild<QTabWidget*>();
    if (tabs == nullptr)
        return nullptr;
    QWidget* page = tabs->widget(tab_index);
    if (page == nullptr)
        return nullptr;
    for (QTableWidget* table : page->findChildren<QTableWidget*>()) {
        if (table->horizontalHeaderItem(0) != nullptr && table->horizontalHeaderItem(0)->text() == first_header) {
            return table;
        }
    }
    return nullptr;
}

QLabel* FindLabelContaining(QWidget* root, const QString& needle) {
    for (QLabel* label : root->findChildren<QLabel*>()) {
        if (label->text().contains(needle))
            return label;
    }
    return nullptr;
}

QPushButton* FindButton(QWidget* root, const QString& text) {
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

// 页内唯一自绘图表靠标题区分（问题清单）
MetricChartWidget* FindChartByTitle(QWidget* root, const QString& title) {
    for (MetricChartWidget* chart : root->findChildren<MetricChartWidget*>()) {
        if (chart->Title() == title)
            return chart;
    }
    return nullptr;
}

QString Cell(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item == nullptr ? QString() : item->text();
}

// 与 test_diagnostics_result_flow 同款的极小 HLS 清单：清单解析走纯 stdlib 路径，
// 不依赖 FFmpeg 解码，但完整穿过"工作线程 -> 回调 -> 排队投递 -> facade 落库 -> 信号"
QString WriteManifest() {
    const QString path = QDir::tempPath() + "/videoeye_diagnostics_page.m3u8";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write("#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXT-X-TARGETDURATION:4\n"
            "#EXT-X-MEDIA-SEQUENCE:0\n"
            "#EXTINF:4.0,\n"
            "seg0.ts\n"
            "#EXTINF:4.0,\n"
            "seg1.ts\n"
            "#EXT-X-ENDLIST\n");
    f.close();
    return path;
}

// 扫描生命周期观察者：在页面构造后立即挂上，StartScan 前就位
struct ScanTrace {
    int started = 0;
    int report_changed = 0;
    std::vector<DiagnosticsPage::ScanEndReason> ends;

    void Observe(DiagnosticsPage* page) {
        QObject::connect(page, &DiagnosticsPage::ScanStarted, [this] { ++started; });
        QObject::connect(page, &DiagnosticsPage::QcReportChanged, [this](const QcReport&) { ++report_changed; });
        QObject::connect(page, &DiagnosticsPage::ScanEnded,
                         [this](DiagnosticsPage::ScanEndReason reason) { ends.push_back(reason); });
    }
};

} // namespace

// 构造后未扫描 -> 引导文案 + 两个子页 + 规则表已填默认规则 + 按钮初态
TEST(DiagnosticsPageTests, UnscannedPageShowsInitialStateAndRuleTable) {
    EnsureApp();
    DiagnosticsPage page;

    auto* tabs = page.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 2);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("问题清单"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("规则与阈值"));

    QTableWidget* issue_table = TableInTab(&page, 0, QStringLiteral("严重度"));
    QTableWidget* rule_table = TableInTab(&page, 1, QStringLiteral("启用"));
    ASSERT_NE(issue_table, nullptr);
    ASSERT_NE(rule_table, nullptr);
    EXPECT_EQ(issue_table->columnCount(), 6);
    EXPECT_EQ(issue_table->rowCount(), 0);

    // 规则表在构造函数里就按 facade 的规则集填好（曾经是空指针解引用/闪退的源头）
    const std::vector<QcRule> defaults = DefaultQcRules();
    ASSERT_FALSE(defaults.empty());
    ASSERT_EQ(rule_table->rowCount(), static_cast<int>(defaults.size()));
    EXPECT_EQ(Cell(rule_table, 0, 1), QString::fromStdString(defaults[0].name));
    EXPECT_EQ(rule_table->item(0, 0)->checkState(), defaults[0].enabled ? Qt::Checked : Qt::Unchecked);
    EXPECT_EQ(Cell(rule_table, 0, 5),
              QString::number(defaults[0].threshold, 'f', 2) + QString::fromStdString(defaults[0].unit));

    QPushButton* start_button = FindButton(&page, QStringLiteral("开始分析"));
    QPushButton* cancel_button = FindButton(&page, QStringLiteral("取消"));
    QPushButton* export_button = FindButton(&page, QStringLiteral("导出报告"));
    ASSERT_NE(start_button, nullptr);
    ASSERT_NE(cancel_button, nullptr);
    ASSERT_NE(export_button, nullptr);
    EXPECT_TRUE(start_button->isEnabled());
    EXPECT_FALSE(cancel_button->isEnabled());
    EXPECT_FALSE(export_button->isEnabled());

    auto* progress = page.findChild<QProgressBar*>();
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->value(), 0);
    EXPECT_EQ(progress->format(), QStringLiteral("未开始"));

    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("点击「开始分析」对当前文件做一次完整扫描")), nullptr);
    EXPECT_NE(FindChartByTitle(&page, QStringLiteral("逐秒码率 / 帧率")), nullptr);
}

// 空路径 + silent：静默放弃（不弹"请先打开文件"），不发任何扫描信号、界面原样
TEST(DiagnosticsPageTests, StartScanWithoutSourceIsSilentNoOp) {
    EnsureApp();
    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);

    QLabel* summary = FindLabelContaining(&page, QStringLiteral("点击「开始分析」对当前文件做一次完整扫描"));
    ASSERT_NE(summary, nullptr);
    const QString before = summary->text();

    page.StartScan(page.options(), /*silent=*/true);

    EXPECT_EQ(trace.started, 0);
    EXPECT_TRUE(trace.ends.empty());
    EXPECT_EQ(summary->text(), before);
    EXPECT_EQ(page.findChild<QProgressBar*>()->format(), QStringLiteral("未开始"));
    EXPECT_FALSE(page.hasResult());
}

// 失败路径（silent=true 不弹框）：失败同样是终态 —— 按钮复位、进度"失败"、
// 汇总标签给出原因、导出保持禁用、开始按钮可再次点击
TEST(DiagnosticsPageTests, FailedScanEmitsTerminalStateAndResetsControls) {
    EnsureApp();
    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);

    QLabel* summary = FindLabelContaining(&page, QStringLiteral("点击「开始分析」对当前文件做一次完整扫描"));
    ASSERT_NE(summary, nullptr);
    const QString initial_summary = summary->text();

    page.SetSourcePath(QDir::tempPath() + "/videoeye_no_such_input_20261007.mkv");
    page.StartScan(page.options(), /*silent=*/true);
    EXPECT_EQ(trace.started, 1); // ScanStarted 是同步发出的

    ASSERT_TRUE(PumpEventsUntil([&trace] { return !trace.ends.empty(); })) << "失败终态未回包";
    ASSERT_EQ(trace.ends.size(), 1u);
    EXPECT_EQ(trace.ends[0], DiagnosticsPage::ScanEndReason::Failed);

    EXPECT_FALSE(page.hasResult());
    EXPECT_NE(summary->text(), initial_summary);
    EXPECT_FALSE(summary->text().isEmpty());

    auto* progress = page.findChild<QProgressBar*>();
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->value(), 0);
    EXPECT_EQ(progress->format(), QStringLiteral("失败"));

    EXPECT_TRUE(FindButton(&page, QStringLiteral("开始分析"))->isEnabled());
    EXPECT_FALSE(FindButton(&page, QStringLiteral("取消"))->isEnabled());
    EXPECT_FALSE(FindButton(&page, QStringLiteral("导出报告"))->isEnabled());
}

// 换文件复位：失败后再 ResetForNewFile -> 回到初始文案 / 进度 / 按钮态
TEST(DiagnosticsPageTests, ResetForNewFileRestoresInitialState) {
    EnsureApp();
    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);

    QLabel* summary = FindLabelContaining(&page, QStringLiteral("点击「开始分析」对当前文件做一次完整扫描"));
    ASSERT_NE(summary, nullptr);
    const QString initial_summary = summary->text();

    page.SetSourcePath(QDir::tempPath() + "/videoeye_no_such_input_20261007.mkv");
    page.StartScan(page.options(), /*silent=*/true);
    ASSERT_TRUE(PumpEventsUntil([&trace] { return !trace.ends.empty(); }));
    ASSERT_NE(summary->text(), initial_summary);

    page.ResetForNewFile();

    EXPECT_FALSE(page.hasResult());
    EXPECT_EQ(summary->text(), initial_summary);
    auto* progress = page.findChild<QProgressBar*>();
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->value(), 0);
    EXPECT_EQ(progress->format(), QStringLiteral("未开始"));
    EXPECT_EQ(TableInTab(&page, 0, QStringLiteral("严重度"))->rowCount(), 0);
    EXPECT_FALSE(FindButton(&page, QStringLiteral("导出报告"))->isEnabled());
}

// 成功路径（HLS 清单）：终态 Completed、导出恢复、问题表与 qcReport() 逐行一致、
// 汇总出评分与容器格式；跨页钩子每次扫描前恰好跑一次
TEST(DiagnosticsPageTests, SuccessfulManifestScanFillsIssueTableAndEnablesExport) {
    EnsureApp();
    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);

    int hook_calls = 0;
    page.SetBeforeScanHook([&hook_calls](videoeye::AnalysisOptions& options) {
        ++hook_calls;
        options.analyze_bitrate_gop = true; // 面板注入的跨页逻辑（如字幕阈值同步）
    });

    const QString manifest = WriteManifest();
    ASSERT_FALSE(manifest.isEmpty()) << "无法写入测试清单";

    page.SetSourcePath(manifest);
    page.StartScan(page.options(), /*silent=*/true);
    EXPECT_EQ(trace.started, 1);

    ASSERT_TRUE(PumpEventsUntil([&trace] { return !trace.ends.empty(); })) << "扫描未在超时前回包";
    ASSERT_EQ(trace.ends.size(), 1u);
    EXPECT_EQ(trace.ends[0], DiagnosticsPage::ScanEndReason::Completed);
    EXPECT_EQ(hook_calls, 1);
    EXPECT_GE(trace.report_changed, 1); // Evaluate 后外发报告刷新

    EXPECT_TRUE(page.hasResult());
    EXPECT_EQ(page.result().container_format, "hls");
    EXPECT_DOUBLE_EQ(page.result().duration_seconds, 8.0);

    auto* progress = page.findChild<QProgressBar*>();
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->value(), 100);
    EXPECT_EQ(progress->format(), QStringLiteral("分析完成"));

    EXPECT_TRUE(FindButton(&page, QStringLiteral("开始分析"))->isEnabled());
    EXPECT_FALSE(FindButton(&page, QStringLiteral("取消"))->isEnabled());
    EXPECT_TRUE(FindButton(&page, QStringLiteral("导出报告"))->isEnabled());

    // 问题表与报告逐行一致：严重度文案 / 类别文案 / 标题
    const QcReport& report = page.qcReport();
    QTableWidget* issue_table = TableInTab(&page, 0, QStringLiteral("严重度"));
    ASSERT_NE(issue_table, nullptr);
    ASSERT_EQ(issue_table->rowCount(), static_cast<int>(report.issues.size()));
    for (int i = 0; i < issue_table->rowCount(); ++i) {
        EXPECT_EQ(Cell(issue_table, i, 0), QString::fromStdString(report.issues[i].SeverityText()));
        EXPECT_EQ(Cell(issue_table, i, 1), QString::fromStdString(report.issues[i].CategoryText()));
        EXPECT_EQ(Cell(issue_table, i, 2), QString::fromStdString(report.issues[i].title));
    }

    QLabel* summary = FindLabelContaining(&page, QStringLiteral("评分"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("容器 hls")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("评分 <b>")));

    EXPECT_EQ(TableInTab(&page, 1, QStringLiteral("启用"))->rowCount(), static_cast<int>(page.rules().size()));

    QFile::remove(manifest);
}

// CancelScan 只发取消请求、不伪造终态：终态由 OnFacadeFinished/OnFacadeFailed 统一发
TEST(DiagnosticsPageTests, CancelScanDoesNotEmitTerminalSignal) {
    EnsureApp();
    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);

    page.CancelScan();

    EXPECT_TRUE(trace.ends.empty());
    EXPECT_EQ(trace.started, 0);
    EXPECT_EQ(page.findChild<QProgressBar*>()->format(), QStringLiteral("取消中..."));
    EXPECT_FALSE(FindButton(&page, QStringLiteral("取消"))->isEnabled());
}

// 规则表：勾选开关与阈值改写落到规则集；非法阈值还原显示；「恢复默认规则」重建
TEST(DiagnosticsPageTests, RuleTableEditsUpdateFacadeRulesAndRevertInvalidInput) {
    EnsureApp();
    DiagnosticsPage page;

    QTableWidget* rule_table = TableInTab(&page, 1, QStringLiteral("启用"));
    ASSERT_NE(rule_table, nullptr);
    const std::vector<QcRule> defaults = DefaultQcRules();
    ASSERT_EQ(rule_table->rowCount(), static_cast<int>(defaults.size()));

    // 勾选开关（用户点复选框的真实路径）必须落到 facade 规则集
    const bool initial_enabled = page.rules()[0].enabled;
    rule_table->item(0, 0)->setCheckState(initial_enabled ? Qt::Unchecked : Qt::Checked);
    EXPECT_EQ(page.rules()[0].enabled, !initial_enabled);

    // 阈值允许 "12.5s" 这类带单位输入，取前导数字部分
    rule_table->item(0, 5)->setText(QStringLiteral("12.5s"));
    EXPECT_DOUBLE_EQ(page.rules()[0].threshold, 12.5);

    // 非法输入不写入规则、还原为当前阈值的规范化显示
    rule_table->item(0, 5)->setText(QStringLiteral("abc"));
    EXPECT_DOUBLE_EQ(page.rules()[0].threshold, 12.5);
    EXPECT_EQ(Cell(rule_table, 0, 5), QString::number(12.5, 'f', 2) + QString::fromStdString(page.rules()[0].unit));

    // 恢复默认规则：规则集与表格一起回到默认快照
    QPushButton* reset_button = FindButton(&page, QStringLiteral("恢复默认规则"));
    ASSERT_NE(reset_button, nullptr);
    reset_button->click();
    EXPECT_EQ(page.rules()[0].enabled, defaults[0].enabled);
    EXPECT_DOUBLE_EQ(page.rules()[0].threshold, defaults[0].threshold);
    EXPECT_EQ(rule_table->item(0, 0)->checkState(), defaults[0].enabled ? Qt::Checked : Qt::Unchecked);
    EXPECT_EQ(Cell(rule_table, 0, 5),
              QString::number(defaults[0].threshold, 'f', 2) + QString::fromStdString(defaults[0].unit));
}
