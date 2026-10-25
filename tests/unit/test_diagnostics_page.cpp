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
//   5) CancelScan 只请求取消、不伪造终态；ResetForNewFile 回到初始态；
//   6) 取消延迟预算（阶段 5）：请求 -> 界面状态更新必须落在 kCancelResponseBudgetMs 内，
//      请求 -> 后台收敛记 kMetricCancelSettleMs，收敛前看门狗只改进度条文案、
//      全程不弹模态框；空闲态误调取消不进"取消中"、也不留一个永不停止的看门狗。
//
// 本页已收敛掉与「事件与时间轴」页重复的实时时间轴视图：只剩「问题清单 / 规则与阈值」
// 两个子页，单测只覆盖这两页与扫描总控状态。
//
// 只构造控件、不渲染（offscreen 平台）；导出按钮会弹 QFileDialog 模态框，
// 「未扫描就跳转问题位置 / 关联场景切换」会弹 QMessageBox，均不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>

#include <functional>
#include <string>
#include <vector>

#include "core/domain/model/AnalysisOptions.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/QcRule.h"
#include "core/qc/QcRuleDefaults.h"
#include "infrastructure/logging/PerfMetrics.h"
#include "ui/analysis_panel/DiagnosticsPage.h"
#include "ui/charts/MetricChartWidget.h"

#include "BlockingTcpEndpoint.h"

using videoeye::PerfMetrics;
using videoeye::model::QcReport;
using videoeye::model::QcRule;
using videoeye::qc::DefaultQcRules;
using videoeye::ui::DiagnosticsPage;
using videoeye::ui::MetricChartWidget;

namespace {

// 模态框探针：取消路径（请求 -> 后台收敛）全程不许弹任何模态框。
// 弹框会把"打开即分析"这类自动流程卡在模态循环里等不到人点，连续取消还会叠出多个框。
// 只有 QApplication::notify 能看到所有事件 —— 事件过滤器得挂到具体对象上，
// 而弹框是运行时才 new 出来的，挂不上。
class ModalProbeApp : public QApplication {
public:
    ModalProbeApp(int& argc, char** argv) : QApplication(argc, argv) {
    }

    int dialogs_shown = 0;

    bool notify(QObject* receiver, QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::Show && qobject_cast<QDialog*>(receiver) != nullptr) {
            ++dialogs_shown;
        }
        return QApplication::notify(receiver, event);
    }
};

ModalProbeApp* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static ModalProbeApp* app = [] {
        static int argc = 1;
        static char name[] = "test_diagnostics_page";
        static char* argv[] = {name, nullptr};
        return new ModalProbeApp(argc, argv);
    }();
    return app;
}

int ModalDialogsShown() {
    return EnsureApp()->dialogs_shown;
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

// 取消收敛看门狗：SetupUi 里给了对象名，避免和将来加进来的其它定时器混淆
QTimer* FindCancelWatchdog(QWidget* root) {
    return root->findChild<QTimer*>(QStringLiteral("cancel_watchdog"));
}

QString ProgressFormat(QWidget* root) {
    const QProgressBar* bar = root->findChild<QProgressBar*>();
    return bar == nullptr ? QString() : bar->format();
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

// 构造后未扫描 -> 引导文案 + 四个子页 + 规则表已填默认规则 + 按钮初态
TEST(DiagnosticsPageTests, UnscannedPageShowsInitialStateAndRuleTable) {
    EnsureApp();
    DiagnosticsPage page;

    auto* tabs = page.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 5);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("问题清单"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("规则与阈值"));
    // 阶段 5 新增：证据与覆盖（每个分析维度一行）。这张表的作用是让
    // "未发现问题"能被回溯成"跑了、覆盖够了"，所以未扫描时也必须存在且为空。
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("证据与覆盖"));
    // 阶段 1 新增：参数变化时间线（每条视频轨的配置切换）
    EXPECT_EQ(tabs->tabText(3), QStringLiteral("参数变化"));
    // 帧级 POC 新增：帧顺序（每条 H.264 视频轨的逐帧 POC，按解码顺序）
    EXPECT_EQ(tabs->tabText(4), QStringLiteral("帧顺序"));

    // 未扫描时帧顺序表必须存在且为空 —— 与"证据与覆盖"同理，
    // "没有数据"和"没建这张表"在界面上不能长得一样。
    QTableWidget* frame_order_table = TableInTab(&page, 4, QStringLiteral("流"));
    ASSERT_NE(frame_order_table, nullptr);
    EXPECT_EQ(frame_order_table->rowCount(), 0);

    QTableWidget* issue_table = TableInTab(&page, 0, QStringLiteral("严重度"));
    QTableWidget* rule_table = TableInTab(&page, 1, QStringLiteral("启用"));
    QTableWidget* dimension_table = TableInTab(&page, 2, QStringLiteral("维度"));
    QTableWidget* parameter_table = TableInTab(&page, 3, QStringLiteral("流"));
    ASSERT_NE(issue_table, nullptr);
    ASSERT_NE(rule_table, nullptr);
    ASSERT_NE(dimension_table, nullptr);
    ASSERT_NE(parameter_table, nullptr);
    EXPECT_EQ(issue_table->columnCount(), 6);
    EXPECT_EQ(issue_table->rowCount(), 0);
    EXPECT_EQ(dimension_table->rowCount(), 0);
    EXPECT_EQ(parameter_table->rowCount(), 0);

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
    EXPECT_EQ(page.result()->container_format, "hls");
    EXPECT_DOUBLE_EQ(page.result()->duration_seconds, 8.0);

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

// ---------------------------------------------------------------------------
// 取消体验（阶段 5）
//
// 三条互相独立的保证，分开测：
//   1) **立刻更新状态**：请求 -> 按钮置灰 + 进度条转"取消中"，落在 kCancelResponseBudgetMs 内；
//   2) **只发请求、不伪造终态**：终态仍由 OnFacadeFinished/OnFacadeFailed 统一发；
//   3) **后台最终收敛**：终态一到，取消态清掉、看门狗停；收敛前看门狗只把等待时长
//      写进进度条，**绝不弹框**（弹框会把"打开即分析"这类自动流程卡在模态循环里）。
//
// 要测到"真的在跑"的取消，必须有一个不会自己结束的扫描：本地只监听、从不回包的
// TCP 端口就是这个东西（BlockingTcpEndpoint），FFmpeg 卡在读上，只能靠中断回调退出。
// ---------------------------------------------------------------------------

// 起一个卡住的扫描。返回 false 表示端口没阻塞住（扫描瞬间回了终态），调用方应跳过。
bool StartBlockingScan(DiagnosticsPage* page, const ScanTrace& trace,
                       const videoeye_test::BlockingTcpEndpoint& endpoint) {
    page->SetSourcePath(QString::fromStdString(endpoint.tcp_url()));
    page->StartScan(page->options());
    // 等它真的卡进去：阻塞输入不会自己结束，正常情况下这段时间里不会有终态。
    // 若终态自己来了说明端口没阻塞住，这次没测到取消路径。
    return !PumpEventsUntil([&trace] { return !trace.ends.empty(); }, 400);
}

// 空闲态（没有在跑的扫描）误调取消：不进"取消中"、也不留一个永远等不到终态的看门狗。
// 取消按钮在空闲态本来就是禁用的，这里是防面板那种把各页 CancelRequested 都转进来的
// 批量入口误调 —— 以前会留下一个每秒刷一次"仍在收尾"的定时器，而界面上什么都没在做。
TEST(DiagnosticsPageTests, CancelScanWithoutRunningScanDoesNotEnterCancellingState) {
    EnsureApp();
    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);

    auto& metrics = PerfMetrics::GetInstance();
    const auto before = metrics.Snapshot(videoeye::kMetricCancelResponseMs);
    const QString format_before = ProgressFormat(&page);

    page.CancelScan();

    EXPECT_EQ(ProgressFormat(&page), format_before) << "没有任务在跑时不该进入『取消中』态";
    EXPECT_FALSE(FindCancelWatchdog(&page)->isActive()) << "没有任务在跑时不该起看门狗";
    EXPECT_TRUE(trace.ends.empty());
    EXPECT_EQ(trace.started, 0);
    // 没有可取消的任务，"响应延迟"这个数没有意义，不该混进正常样本里
    EXPECT_EQ(metrics.Snapshot(videoeye::kMetricCancelResponseMs).count, before.count);
}

// 1) + 2)：取消后立刻更新状态，且响应延迟落在预算内；终态仍然不在这里发
TEST(DiagnosticsPageTests, CancelScanUpdatesStateImmediatelyWithinResponseBudget) {
    EnsureApp();
    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);
    if (!StartBlockingScan(&page, trace, endpoint)) {
        GTEST_SKIP() << "阻塞输入瞬间就回终态（端口未真正阻塞），本轮不算测到取消路径";
    }

    auto& metrics = PerfMetrics::GetInstance();
    const auto before = metrics.Snapshot(videoeye::kMetricCancelResponseMs);

    page.CancelScan();

    const auto after = metrics.Snapshot(videoeye::kMetricCancelResponseMs);
    EXPECT_EQ(after.count - before.count, 1) << "取消必须恰好记一次响应延迟";
    EXPECT_LE(after.last, videoeye::kCancelResponseBudgetMs)
        << "取消响应 " << after.last << "ms 超出预算：这条路径上夹了同步等待";

    EXPECT_EQ(ProgressFormat(&page), QStringLiteral("取消中..."));
    EXPECT_FALSE(FindButton(&page, QStringLiteral("取消"))->isEnabled());
    EXPECT_TRUE(trace.ends.empty()) << "取消只发请求，终态由 OnFacadeFinished/OnFacadeFailed 发";

    // 收尾：等后台真的退出来，别把还在跑的扫描留给析构函数
    ASSERT_TRUE(PumpEventsUntil([&trace] { return !trace.ends.empty(); }, 8000)) << "取消后后台没有收敛";
}

// 3)：收敛前看门狗只报等待时长（不弹框、不伪造终态），终态一到立刻停手
TEST(DiagnosticsPageTests, CancelWatchdogReportsWaitingUntilTerminalStopsIt) {
    EnsureApp();
    videoeye_test::BlockingTcpEndpoint endpoint;
    ASSERT_TRUE(endpoint.valid()) << "无法建立本地阻塞端口，本用例无法运行";

    ScanTrace trace;
    DiagnosticsPage page;
    trace.Observe(&page);
    if (!StartBlockingScan(&page, trace, endpoint)) {
        GTEST_SKIP() << "阻塞输入瞬间就回终态（端口未真正阻塞），本轮不算测到取消路径";
    }

    auto& metrics = PerfMetrics::GetInstance();
    const auto settle_before = metrics.Snapshot(videoeye::kMetricCancelSettleMs);
    const int modals_before = ModalDialogsShown();

    page.CancelScan();
    QTimer* watchdog = FindCancelWatchdog(&page);
    ASSERT_NE(watchdog, nullptr);
    EXPECT_TRUE(watchdog->isActive()) << "取消后收敛看门狗必须起来";

    // 直接进私有槽：看门狗的预算是 3 秒，单测里等不起，也不需要等 ——
    // 要验的是"超预算还没收敛时它会说什么"。
    // 中间**不能泵事件**：一泵就可能把后台的终态信号送进来，cancel_pending_ 就清掉了。
    ASSERT_TRUE(QMetaObject::invokeMethod(&page, "OnCancelWatchdog"));
    EXPECT_TRUE(ProgressFormat(&page).contains(QStringLiteral("收尾")))
        << "超预算未收敛必须把等待时长显示出来，否则用户只能以为卡死了";
    EXPECT_TRUE(trace.ends.empty()) << "看门狗绝不许伪造终态";
    EXPECT_FALSE(FindButton(&page, QStringLiteral("取消"))->isEnabled());
    EXPECT_EQ(ModalDialogsShown(), modals_before) << "取消未收敛时不许弹模态框";

    // 后台最终必须收敛（预算写进日志，这里只守"一定会收敛 + 恰好一个终态"）
    ASSERT_TRUE(PumpEventsUntil([&trace] { return !trace.ends.empty(); }, 8000)) << "取消后后台没有收敛";
    EXPECT_EQ(trace.ends.size(), 1u) << "一次扫描只应产生一个终态事件";

    const auto settle_after = metrics.Snapshot(videoeye::kMetricCancelSettleMs);
    EXPECT_EQ(settle_after.count - settle_before.count, 1) << "取消收敛延迟必须被记下来（请求 -> 后台真正退出）";
    EXPECT_GE(settle_after.last, 0.0);

    EXPECT_FALSE(watchdog->isActive()) << "终态到了看门狗必须停，否则它会一直每秒刷进度条";
    EXPECT_FALSE(ProgressFormat(&page).contains(QStringLiteral("取消中"))) << "收敛后必须离开『取消中』态";
    EXPECT_EQ(ModalDialogsShown(), modals_before) << "取消路径全程不许弹模态框";
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
