// BitrateGopPage 的页面级行为测试（评审「页面组件补测」）。
//
// 静默回归点：
//   1) GOP 表格式化：起始/结束走 formatTime（HH:MM:SS）、字节 KB 保留 1 位、
//      平均码率取整、closed/open 与收尾状态文案；
//   2) 超长 GOP 的橙色高亮（时长超过 max_gop_seconds 只变色，不改数据）；
//   3) 异常表：类型文案、TimeRange 时间区间、单位化数值（s / kbps）；
//   4) 汇总的三处开关（帧类型是否已解析、目标峰值手动/自动、窗口长度）；
//   5) 参数往返（FillScanOptions）+ 扫描态互斥 + 点行 SeekRequested +
//      「关联场景切换」把记录与当前阈值一并抛出。
//
// 只构造控件、不渲染（offscreen 平台）；导出按钮与「未扫描就关联场景切换」
// 会弹模态框，不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>

#include <string>
#include <vector>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/BitrateGopResult.h"
#include "core/domain/model/GopInfo.h"
#include "core/domain/model/SceneChangeResult.h"
#include "core/domain/model/TimeRange.h"
#include "ui/analysis_panel/BitrateGopPage.h"

using videoeye::model::AnalysisResult;
using videoeye::model::BitrateAnomaly;
using videoeye::model::BitrateAnomalyType;
using videoeye::model::BitrateGopAnalysis;
using videoeye::model::GopInfo;
using videoeye::model::SceneChangeResult;
using videoeye::ui::BitrateGopPage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_bitrate_gop_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

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

// 目标峰值框 decimals=0，GOP 秒上限 decimals=1，页内各自唯一
QDoubleSpinBox* FindSpinByDecimals(QWidget* root, int decimals) {
    for (QDoubleSpinBox* spin : root->findChildren<QDoubleSpinBox*>()) {
        if (spin->decimals() == decimals)
            return spin;
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

QString Cell(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item == nullptr ? QString() : item->text();
}

void ClickCell(QTableWidget* table, int row, int column) {
    QMetaObject::invokeMethod(table, "cellClicked", Qt::DirectConnection, Q_ARG(int, row), Q_ARG(int, column));
}

// 一份典型结果：2 个 GOP（第 2 个 12 s > 默认上限 10 s）+ 2 条异常 + 2 条建议
BitrateGopAnalysis MakeBg() {
    BitrateGopAnalysis bg;
    bg.duration_seconds = 120.0;
    bg.total_frames = 100;
    bg.total_bytes = 1000000;
    bg.avg_bitrate_kbps = 1500.0;
    bg.peak_bitrate_kbps = 2200.0;
    bg.min_bitrate_kbps = 800.0;
    bg.median_bitrate_kbps = 1400.0;
    bg.p95_bitrate_kbps = 2000.0;
    bg.target_peak_kbps = 0.0;
    bg.peak_to_mean_ratio = 1.47;
    bg.i_frame_count = 5;
    bg.p_frame_count = 30;
    bg.b_frame_count = 65;
    bg.frame_types_known = true;
    bg.i_frame_bytes_ = 0; // 平均 I 帧回落为平均帧

    GopInfo g0;
    g0.index = 0;
    g0.stream_index = 0;
    g0.start_seconds = 65.0;
    g0.end_seconds = 67.0;
    g0.frame_count = 50;
    g0.byte_count = 102400; // 100.0 KB
    g0.i_count = 5;
    g0.p_count = 15;
    g0.b_count = 30;
    g0.max_frame_bytes = 20480; // 20.0 KB
    g0.closed_gop = true;
    g0.complete = true;
    GopInfo g1;
    g1.index = 1;
    g1.stream_index = 0;
    g1.start_seconds = 67.0;
    g1.end_seconds = 79.0; // 12 s，超过默认上限 10 s
    g1.frame_count = 100;
    g1.byte_count = 204800; // 200.0 KB
    g1.i_count = 1;
    g1.p_count = 20;
    g1.b_count = 79;
    g1.max_frame_bytes = 40960;
    g1.closed_gop = false;
    g1.complete = false;
    bg.gops = {g0, g1};
    bg.gop_duration_mean = 6.5;
    bg.gop_duration_max = 12.0;
    bg.gop_frames_min = 50;
    bg.gop_frames_max = 100;
    bg.long_gop_count = 1;
    bg.closed_gop_count = 1;
    bg.open_gop_count = 1;
    bg.key_interval_mean = 12.0;
    bg.key_interval_stddev = 0.0;

    BitrateAnomaly a0;
    a0.type = BitrateAnomalyType::LongGop;
    a0.start_seconds = 67.0;
    a0.end_seconds = 79.0;
    a0.value = 12.0;
    a0.threshold = 10.0;
    a0.unit = "s";
    a0.gop_index = 1;
    a0.detail = "GOP #2 时长 12.000 s 超过上限 10.000 s";
    a0.suggestion = "提高关键帧密度";
    BitrateAnomaly a1;
    a1.type = BitrateAnomalyType::PeakOvershoot;
    a1.start_seconds = 3.0;
    a1.end_seconds = 5.0;
    a1.value = 2500.0;
    a1.threshold = 2200.0;
    a1.unit = "kbps";
    a1.detail = "码率峰值 2500 kbps 超过目标 2200 kbps";
    a1.suggestion = "检查场景切换处的码控";
    bg.anomalies = {a0, a1};
    bg.suggestions = {"GOP #2 超长：建议把关键帧间隔压到 10 s 内", "峰值超标：开启 VBV 约束"};
    bg.i_frame_seconds = {65.0, 67.0};
    return bg;
}

} // namespace

// 构造后未扫描 -> 引导文案 + 三个空的子页
TEST(BitrateGopPageTests, UnanalyzedShowsInitialSummaryAndEmptyTables) {
    EnsureApp();
    BitrateGopPage page;

    EXPECT_NE(FindLabelContaining(&page, QStringLiteral("点击「开始分析」扫描当前文件")), nullptr);
    auto* tabs = page.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 3);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("GOP 列表"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("异常"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("优化建议"));

    QTableWidget* gop = TableInTab(&page, 0, QStringLiteral("序号"));
    QTableWidget* anomaly = TableInTab(&page, 1, QStringLiteral("类型"));
    ASSERT_NE(gop, nullptr);
    ASSERT_NE(anomaly, nullptr);
    EXPECT_EQ(gop->columnCount(), 11);
    EXPECT_EQ(gop->rowCount(), 0);
    EXPECT_EQ(anomaly->rowCount(), 0);

    auto* suggestions = tabs->widget(2)->findChild<QListWidget*>();
    ASSERT_NE(suggestions, nullptr);
    EXPECT_EQ(suggestions->count(), 0);
}

// GOP 表的格式化 + 超长高亮 + 汇总关键字段
TEST(BitrateGopPageTests, ResultFillsGopTableAndHighlightsLongGop) {
    EnsureApp();
    BitrateGopPage page;
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("点击「开始分析」扫描当前文件"));
    ASSERT_NE(summary, nullptr);

    AnalysisResult result;
    result.bitrate_gop = MakeBg();
    page.SetResult(result);

    QTableWidget* gop = TableInTab(&page, 0, QStringLiteral("序号"));
    QTableWidget* anomaly = TableInTab(&page, 1, QStringLiteral("类型"));
    ASSERT_NE(gop, nullptr);
    ASSERT_NE(anomaly, nullptr);
    ASSERT_EQ(gop->rowCount(), 2);

    // 65 s -> 00:01:05；102400 B -> 100.0 KB；409.6 kbps -> 410
    EXPECT_EQ(Cell(gop, 0, 0), QStringLiteral("0"));
    EXPECT_EQ(Cell(gop, 0, 1), QStringLiteral("00:01:05"));
    EXPECT_EQ(Cell(gop, 0, 2), QStringLiteral("00:01:07"));
    EXPECT_EQ(Cell(gop, 0, 3), QStringLiteral("2.000"));
    EXPECT_EQ(Cell(gop, 0, 4), QStringLiteral("50"));
    EXPECT_EQ(Cell(gop, 0, 5), QStringLiteral("100.0"));
    EXPECT_EQ(Cell(gop, 0, 6), QStringLiteral("410"));
    EXPECT_EQ(Cell(gop, 0, 7), QStringLiteral("5I / 15P / 30B"));
    EXPECT_EQ(Cell(gop, 0, 8), QStringLiteral("20.0"));
    EXPECT_EQ(Cell(gop, 0, 9), QStringLiteral("closed"));
    EXPECT_EQ(Cell(gop, 0, 10), QStringLiteral("已收尾"));
    EXPECT_NE(gop->item(0, 3)->foreground().color(), QColor("#ef6c00"));

    // 12 s > 默认上限 10 s：只给时长列橙色，不改数值
    EXPECT_EQ(Cell(gop, 1, 3), QStringLiteral("12.000"));
    EXPECT_EQ(gop->item(1, 3)->foreground().color(), QColor("#ef6c00"));
    EXPECT_EQ(Cell(gop, 1, 5), QStringLiteral("200.0"));
    EXPECT_EQ(Cell(gop, 1, 9), QStringLiteral("open"));
    EXPECT_EQ(Cell(gop, 1, 10), QStringLiteral("未收尾"));

    // 异常表：类型文案 + 时间区间 + 单位化数值
    ASSERT_EQ(anomaly->rowCount(), 2);
    EXPECT_EQ(Cell(anomaly, 0, 0), QStringLiteral("超长 GOP"));
    EXPECT_EQ(Cell(anomaly, 0, 1), QStringLiteral("00:01:07.000 - 00:01:19.000"));
    EXPECT_EQ(Cell(anomaly, 0, 2), QStringLiteral("12.00 s"));
    EXPECT_EQ(Cell(anomaly, 0, 3), QStringLiteral("10.00 s"));
    EXPECT_EQ(Cell(anomaly, 0, 4), QStringLiteral("GOP #2 时长 12.000 s 超过上限 10.000 s"));
    EXPECT_EQ(Cell(anomaly, 1, 0), QStringLiteral("码率峰值超标"));
    EXPECT_EQ(Cell(anomaly, 1, 1), QStringLiteral("00:00:03.000 - 00:00:05.000"));
    EXPECT_EQ(Cell(anomaly, 1, 2), QStringLiteral("2500 kbps"));
    EXPECT_EQ(Cell(anomaly, 1, 3), QStringLiteral("2200 kbps"));

    // 汇总：窗口 / 均值 / 帧类型比例 / GOP 数 / 异常数
    EXPECT_TRUE(summary->text().contains(QStringLiteral("窗口 <b>1.00</b> 秒")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("平均 <b>1500</b> kbps")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("I 5.0% / P 30.0% / B 65.0%")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("目标峰值 0 kbps（自动=均值×2）")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("GOP <b>2</b> 个")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("异常 2 条")));

    // 优化建议列表
    auto* suggestions = page.findChild<QTabWidget*>()->widget(2)->findChild<QListWidget*>();
    ASSERT_NE(suggestions, nullptr);
    ASSERT_EQ(suggestions->count(), 2);
    EXPECT_EQ(suggestions->item(0)->text(), QStringLiteral("GOP #2 超长：建议把关键帧间隔压到 10 s 内"));
}

// 汇总的三处开关：帧类型未解析 / 目标峰值手动 / 无视频帧兜底
TEST(BitrateGopPageTests, SummarySwitchesOnFrameTypesAndTargetPeak) {
    EnsureApp();
    BitrateGopPage page;
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("点击「开始分析」扫描当前文件"));
    ASSERT_NE(summary, nullptr);

    AnalysisResult result;
    result.bitrate_gop = MakeBg();
    result.bitrate_gop.frame_types_known = false;
    result.bitrate_gop.i_frame_count = 3;
    result.bitrate_gop.p_frame_count = 0;
    result.bitrate_gop.b_frame_count = 0;
    result.bitrate_gop.unknown_frame_count = 97;
    page.SetResult(result);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("I 3 帧（其余未解析，勾选「精确帧类型」重新分析）")));

    // 「手动 / 自动」由页面参数决定（spin -> options_），走真实链路设置
    QDoubleSpinBox* peak_spin = FindSpinByDecimals(&page, 0);
    ASSERT_NE(peak_spin, nullptr);
    peak_spin->setValue(3000.0);
    result.bitrate_gop.target_peak_kbps = 3000.0;
    page.SetResult(result);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("目标峰值 3000 kbps（手动）")));

    result.bitrate_gop.total_frames = 0;
    page.SetResult(result);
    EXPECT_EQ(summary->text(), QStringLiteral("未检测到视频帧，无法进行码率与 GOP 分析。"));
}

// 异常表上限 2000 行
TEST(BitrateGopPageTests, AnomalyTableCapsAt2000Rows) {
    EnsureApp();
    BitrateGopPage page;
    AnalysisResult result;
    result.bitrate_gop = MakeBg();
    result.bitrate_gop.anomalies.clear();
    for (int i = 0; i < 2005; ++i) {
        BitrateAnomaly anomaly;
        anomaly.type = BitrateAnomalyType::LongGop;
        anomaly.start_seconds = i * 0.1;
        anomaly.end_seconds = i * 0.1;
        anomaly.value = 12.0;
        anomaly.threshold = 10.0;
        anomaly.unit = "s";
        result.bitrate_gop.anomalies.push_back(anomaly);
    }
    page.SetResult(result);

    QTableWidget* anomaly = TableInTab(&page, 1, QStringLiteral("类型"));
    ASSERT_NE(anomaly, nullptr);
    EXPECT_EQ(anomaly->rowCount(), 2000);
}

// 窗口下拉切换重算汇总（display_window_ 必须同步到文案里）
TEST(BitrateGopPageTests, WindowComboUpdatesSummary) {
    EnsureApp();
    BitrateGopPage page;
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("点击「开始分析」扫描当前文件"));
    ASSERT_NE(summary, nullptr);

    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 4);
    EXPECT_EQ(combo->itemText(0), QStringLiteral("1.00 秒"));
    EXPECT_EQ(combo->itemText(1), QStringLiteral("0.50 秒"));

    AnalysisResult result;
    result.bitrate_gop = MakeBg();
    page.SetResult(result);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("窗口 <b>1.00</b> 秒")));

    combo->setCurrentIndex(3); // 5.00 秒
    EXPECT_TRUE(summary->text().contains(QStringLiteral("窗口 <b>5.00</b> 秒")));
}

// 点 GOP / 异常行 -> SeekRequested；越界行不跳
TEST(BitrateGopPageTests, SeekRequestedOnGopAndAnomalyRowClick) {
    EnsureApp();
    BitrateGopPage page;
    std::vector<double> seeks;
    QObject::connect(&page, &BitrateGopPage::SeekRequested, [&seeks](double seconds) { seeks.push_back(seconds); });

    AnalysisResult result;
    result.bitrate_gop = MakeBg();
    page.SetResult(result);

    QTableWidget* gop = TableInTab(&page, 0, QStringLiteral("序号"));
    QTableWidget* anomaly = TableInTab(&page, 1, QStringLiteral("类型"));
    ASSERT_NE(gop, nullptr);
    ASSERT_NE(anomaly, nullptr);

    ClickCell(gop, 1, 0);
    ASSERT_EQ(seeks.size(), 1u);
    EXPECT_DOUBLE_EQ(seeks[0], 67.0);

    ClickCell(anomaly, 1, 0);
    ASSERT_EQ(seeks.size(), 2u);
    EXPECT_DOUBLE_EQ(seeks[1], 3.0);

    ClickCell(gop, 9, 0);
    ClickCell(anomaly, -1, 0);
    EXPECT_EQ(seeks.size(), 2u);
}

// 参数默认值 + FillScanOptions 往返 + 扫描态互斥 + 进度条
TEST(BitrateGopPageTests, OptionsRoundTripAndScanLifecycle) {
    EnsureApp();
    BitrateGopPage page;

    QDoubleSpinBox* target_peak = FindSpinByDecimals(&page, 0);
    QDoubleSpinBox* gop_seconds = FindSpinByDecimals(&page, 1);
    ASSERT_NE(target_peak, nullptr);
    ASSERT_NE(gop_seconds, nullptr);
    EXPECT_DOUBLE_EQ(target_peak->value(), 0.0);
    EXPECT_DOUBLE_EQ(gop_seconds->value(), 10.0);
    auto* gop_frames = page.findChild<QSpinBox*>();
    ASSERT_NE(gop_frames, nullptr);
    EXPECT_EQ(gop_frames->value(), 300);
    QCheckBox* decode_types = page.findChild<QCheckBox*>();
    ASSERT_NE(decode_types, nullptr);
    EXPECT_FALSE(decode_types->isChecked());

    target_peak->setValue(2000.0);
    gop_seconds->setValue(8.0);
    gop_frames->setValue(250);
    decode_types->setChecked(true);
    videoeye::AnalysisOptions options;
    page.FillScanOptions(options);
    EXPECT_TRUE(options.analyze_bitrate_gop);
    EXPECT_DOUBLE_EQ(options.bitrate_gop_options.target_peak_kbps, 2000.0);
    EXPECT_DOUBLE_EQ(options.bitrate_gop_options.max_gop_seconds, 8.0);
    EXPECT_EQ(options.bitrate_gop_options.max_gop_frames, 250);
    EXPECT_TRUE(options.decode_frame_types);

    int scan_requests = 0;
    int cancel_requests = 0;
    QObject::connect(&page, &BitrateGopPage::ScanRequested, [&scan_requests] { ++scan_requests; });
    QObject::connect(&page, &BitrateGopPage::CancelRequested, [&cancel_requests] { ++cancel_requests; });
    QPushButton* start_button = FindButton(&page, QStringLiteral("开始分析"));
    QPushButton* cancel_button = FindButton(&page, QStringLiteral("取消"));
    ASSERT_NE(start_button, nullptr);
    ASSERT_NE(cancel_button, nullptr);

    EXPECT_FALSE(page.IsScanActive());
    EXPECT_TRUE(start_button->isEnabled());
    EXPECT_FALSE(cancel_button->isEnabled());

    start_button->click();
    EXPECT_EQ(scan_requests, 1);
    cancel_button->click(); // 禁用态点不动
    EXPECT_EQ(cancel_requests, 0);

    page.SetScanActive(true);
    EXPECT_TRUE(page.IsScanActive());
    EXPECT_FALSE(start_button->isEnabled());
    EXPECT_TRUE(cancel_button->isEnabled());
    cancel_button->click();
    EXPECT_EQ(cancel_requests, 1);
    page.SetScanActive(false);
    EXPECT_FALSE(page.IsScanActive());

    auto* progress = page.findChild<QProgressBar*>();
    ASSERT_NE(progress, nullptr);
    page.SetProgress(66);
    EXPECT_EQ(progress->value(), 66);
    page.SetProgressFormat(QStringLiteral("扫描中 %p%"));
    EXPECT_EQ(progress->format(), QStringLiteral("扫描中 %p%"));
}

// 「关联场景切换」把检测记录与当前 UI 阈值一并抛出（真正执行由面板编排）
TEST(BitrateGopPageTests, LinkSceneChangesEmitsRecordsAndOptions) {
    EnsureApp();
    BitrateGopPage page;
    AnalysisResult result;
    result.bitrate_gop = MakeBg();
    page.SetResult(result);

    SceneChangeResult r1;
    r1.frame_index = 10;
    r1.timestamp = 12.5;
    r1.score = 0.9;
    SceneChangeResult r2;
    r2.frame_index = 30;
    r2.timestamp = 40.0;
    r2.score = 0.5;
    page.SetSceneChanges({r1, r2}); // 仅刷新图表，不应崩

    int link_calls = 0;
    std::vector<SceneChangeResult> got_records;
    videoeye::BitrateGopOptions got_options;
    QObject::connect(&page, &BitrateGopPage::SceneLinkRequested,
                     [&](const std::vector<SceneChangeResult>& records, const videoeye::BitrateGopOptions& options) {
                         ++link_calls;
                         got_records = records;
                         got_options = options;
                     });

    QDoubleSpinBox* gop_seconds = FindSpinByDecimals(&page, 1);
    ASSERT_NE(gop_seconds, nullptr);
    gop_seconds->setValue(7.5);
    QPushButton* link_button = FindButton(&page, QStringLiteral("关联场景切换"));
    ASSERT_NE(link_button, nullptr);
    link_button->click();

    EXPECT_EQ(link_calls, 1);
    ASSERT_EQ(got_records.size(), 2u);
    EXPECT_DOUBLE_EQ(got_records[0].timestamp, 12.5);
    EXPECT_DOUBLE_EQ(got_records[1].timestamp, 40.0);
    EXPECT_DOUBLE_EQ(got_options.max_gop_seconds, 7.5);
}