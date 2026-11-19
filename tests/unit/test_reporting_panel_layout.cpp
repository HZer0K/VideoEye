// 「报告与批量 QC」页的布局回归 —— 上一轮补的是分析页 14 页（见
// test_analysis_page_layout.cpp），唯独漏了这一页：它不在 AnalysisPageCatalog 里，
// 建页方式是 MainWindow 直接 new，于是它是全项目**控件最多、却零几何覆盖**的一片。
//
// 为什么值得单独建一个用例（而不是并进分析页那个）
// ----------------------------------------------
// 这一页要真面板：ReportingPanel 自带后台任务协调器、导出控制器、三份视图组件，
// 建一次就是几百个控件。并进分析页用例会让那个用例从"纯布局"变成"起线程/读文件"，
// 失败时也分不清是哪一类页面坏了。这里只 new 面板、不跑分析，仍然是纯布局用例。
//
// 判据与阈值全部复用 tests/support/WidgetLayoutScan.h（与分析页同一份），
// 差别只在"怎么把数据喂进去"。
//
// 喂什么数据、为什么是这三条
// ------------------------
// 批量表 / 选中行详情 / 「打开报告文件」按钮在空态是空文本或禁用的，几何上看不出
// 问题；真跑一次批量扫描要扫目录、要起线程，不适合放在布局用例里。所以直接调
// BatchScanView 的 BeginScan / AppendResult / SetProgress / SetSummaryText ——
// 这四个正是 RunBatch 完成回调里用的同一组入口，喂出来的排布就是用户看到的那个。
//   * 一条 Succeeded 带 output_path → 选中后「打开报告文件」启用；
//   * 一条 Failed 带 error         → 详情区出多行文本（撑高 QLabel 才会暴露挤字）；
//   * 一条 TimedOut                → 状态列出现第三种文案宽度。
//
// 覆盖守护：面板不是 QTabWidget 分包的，逐 tab 这一趟在这儿基本白跑（tab 数为 0）。
// 更要紧的是**别照搬分析页那条"喂数据后可见控件必须变多"** —— 这一页量出来两轮都是
// 52 个（表格行是 item 不是 QWidget，铺行不产出控件），照搬就是写个恒假断言。
// 所以这里改成用"行真的画出来了"来钉 AppendResult 链路（见用例末尾）。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSettings>
#include <QSize>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "core/qc/BatchQcResults.h"
#include "tests/support/WidgetLayoutScan.h"
#include "ui/reporting_panel/BatchScanView.h"
#include "ui/reporting_panel/QcCompareDialog.h"
#include "ui/reporting_panel/ReportingPanel.h"

namespace {

using videoeye::qc::BatchItemStatus;
using videoeye::qc::BatchQcItemResult;
using videoeye::ui::BatchScanView;
using videoeye::ui::ReportingPanel;
using videoeye_test::ScanEveryTabState;
using videoeye_test::TabScanResult;

// offscreen：CI 无显示器，平台插件加载阶段会 qFatal。QApplication 必须活到进程结束
// 且 argv 是静态存储（QApplication 会保留指针）。
QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_reporting_panel_layout";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 面板的尺寸取"自己的 sizeHint"和 1000x760 里的大者。
//
// ❗ 这里绝不能用"固定 1000x760 再看有没有重叠"：这一页三个分组框 + 结果表(最小 220)
// + 详情(最小 80) + 摘要(最小 120) 的最小尺寸比 760 高，硬塞进去 QVBoxLayout 会把
// 子控件压成重叠 —— 那是测量方式造出来的假象（2026-10-30 在「报告与批量 QC」页
// 误报过一次，判据要先看「父高 vs 布局最小高」）。
void SizePanelToNatural(QWidget* panel) {
    const QSize hint = panel->sizeHint();
    panel->resize(std::max(1000, hint.width()) + 80, std::max(760, hint.height()) + 40);
}

BatchQcItemResult MakeItem(const std::string& path, BatchItemStatus status) {
    BatchQcItemResult item;
    item.path = path;
    item.status = status;
    item.elapsed_ms = 1234.0;
    switch (status) {
    case BatchItemStatus::Succeeded:
        item.score = 92.5;
        item.verdict = "通过";
        item.warning_count = 2;
        item.output_path = path + ".qc.html";
        break;
    case BatchItemStatus::Failed:
        // 刻意写长：详情标签是 wordWrap 的，长错误会把标签撑到多行，这才是"有数据"这一轮
        // 相对空态真正的几何差异（表格行是 item 不是 QWidget，铺行不增加任何可见控件）。
        item.error = "avformat_open_input 失败：Invalid data found when processing input"
                     "（容器魔数与扩展名不符，moov 缺失，且文件末尾 2 MB 全是零填充 —— "
                     "怀疑拷贝过程中断；建议原盘复核，不要直接判定为编码器问题）";
        item.verdict = "打开失败";
        break;
    case BatchItemStatus::TimedOut:
        item.error = "超过单项预算仍未结算（后台线程已放弃并请求取消）";
        item.verdict = "超时";
        break;
    default:
        break;
    }
    return item;
}

// 走 RunBatch 完成回调那四个入口，把"扫描完成"这一屏排出来。
// 返回批量视图（找不到时为 nullptr —— 依赖拆分改名了）。
BatchScanView* FeedBatchResults(ReportingPanel* panel) {
    auto* batch = panel->findChild<BatchScanView*>();
    if (!batch)
        return nullptr;

    batch->BeginScan();
    batch->AppendResult(MakeItem("/media/clip_ok.mp4", BatchItemStatus::Succeeded));
    batch->AppendResult(MakeItem("/media/clip_broken.mxf", BatchItemStatus::Failed));
    batch->AppendResult(MakeItem("/media/clip_slow.mkv", BatchItemStatus::TimedOut));
    batch->SetProgress(3, 3);
    batch->SetSummaryText("完成 3/3：通过 1，失败 1，超时 1");
    // 选中一行：详情区出多行文本、「打开报告文件」跟着启用。
    batch->SetBusy(false);
    if (auto* table = batch->findChild<QTableWidget*>()) {
        table->selectRow(0);
    }
    return batch;
}

// 一轮扫描：返回这一轮看到的可见控件数。
TabScanResult ScanPanel(ReportingPanel* panel, const std::string& where, std::vector<std::string>& findings) {
    panel->show();
    QApplication::processEvents();
    QApplication::processEvents();
    return ScanEveryTabState(panel, where, findings);
}

// 覆盖守护阈值。取得很松：改版加一列、去掉一行都不该红，但"链路失效"会大幅掉下去。
constexpr size_t kMinEmptyVisible = 40;
constexpr int kFedRows = 3;

} // namespace

TEST(ReportingPanelLayout, EmptyAndScannedStatesKeepControlsInsideLayouts) {
    ASSERT_NE(EnsureApp(), nullptr);

    // 面板一律堆分配、由 unique_ptr 收口：它在析构里回收后台线程，栈上对象 exception
    // 抛出去时也一样要跑析构，交给 unique_ptr 更省心。
    std::unique_ptr<ReportingPanel> panel(new ReportingPanel);
    SizePanelToNatural(panel.get());

    // ---- 第一轮：空态 ----
    std::vector<std::string> empty_findings;
    const TabScanResult empty = ScanPanel(panel.get(), "报告页(空态)", empty_findings);
    for (const std::string& finding : empty_findings)
        ADD_FAILURE() << finding;

    // ---- 第二轮：喂三条批量结果 + 选中一行 ----
    BatchScanView* batch = FeedBatchResults(panel.get());
    for (int i = 0; i < 3; ++i)
        QApplication::processEvents();
    ASSERT_NE(batch, nullptr) << "面板里找不到批量扫描视图 —— 依赖拆分可能改名了";

    // 第二轮只看几何（findings），**不看数量**：批量结果是 QTableWidget 的行，行是 item
    // 不是 QWidget，铺进去不增加任何可见控件（实测两轮都是 52 个）。这一轮真正的几何
    // 差异来自详情标签 —— 长错误文本把它撑成多行，才会挤到下面的摘要框。
    std::vector<std::string> data_findings;
    ScanPanel(panel.get(), "报告页(有结果)", data_findings);
    for (const std::string& finding : data_findings)
        ADD_FAILURE() << finding;

    panel->hide();

    // ---- 覆盖守护 ----
    // 第一条：面板真建出来了（控件数断崖式下跌说明视图组件没接进布局，或结构大改）。
    EXPECT_GE(empty.all_states, kMinEmptyVisible)
        << "空态扫到的可见控件只有 " << empty.all_states << " 个 —— 面板可能压根没建出来";

    // ❗ 这里**没有**"喂数据后可见控件必须变多"那条断言，是量过之后刻意不加的：
    // 两轮都是 52 个，写上去等于写了个恒假的检查 —— 看起来在守、其实什么都没守。
    // 换成用视图自己的渲染结果来判"数据真的进来了"：三行都画出来了
    // （visualItemRect 对隐藏行返回空矩形，所以懒渲染 / 只建首行会被这里抓住）。
    auto* table = batch->findChild<QTableWidget*>();
    ASSERT_NE(table, nullptr) << "批量视图里找不到结果表";
    EXPECT_EQ(static_cast<int>(batch->Results().size()), kFedRows) << "结果没进视图模型";
    EXPECT_EQ(table->rowCount(), kFedRows) << "结果没铺进表格";
    for (int row = 0; row < table->rowCount(); ++row) {
        QTableWidgetItem* item = table->item(row, 0);
        ASSERT_NE(item, nullptr) << "第 " << row << " 行没有单元格";
        EXPECT_FALSE(table->visualItemRect(item).isEmpty())
            << "第 " << row
            << " 行没被画出来（空矩形）—— AppendResult 只写了模型没更新视图，"
               "或该行被误判成隐藏。若是筛选口径有意改了，请同步本用例。";
    }
}

// 批量默认策略：单文件不设硬超时、无进度 120s、画质 QC 关闭。
//
// 这三项不是随手定的默认值：
//   * 内置模板会开启全解码画质 QC，长视频用固定小超时会被误判成 TimedOut；
//   * 无进度超时负责兜住"卡死但没报错"的文件；
//   * 逐帧解码是批量吞吐的主要成本，需要时由用户显式勾选。
// 这里钉住默认值本身，避免以后有人把它改回"固定 30s + 默认全解码"。
TEST(ReportingPanelLayout, BatchDefaultsKeepVisualQcOffAndNoHardTimeout) {
    ASSERT_NE(EnsureApp(), nullptr);

    BatchScanView view;
    const BatchScanView::Settings settings = view.CurrentSettings();
    EXPECT_EQ(settings.timeout_seconds, 0) << "单文件默认不应设硬超时";
    EXPECT_EQ(settings.idle_timeout_seconds, 120) << "无进度超时默认 120 秒";
    EXPECT_FALSE(settings.visual_qc) << "批量默认不做全解码画质 QC";
    EXPECT_EQ(settings.jobs, 4);
    EXPECT_TRUE(settings.recursive);
}

// 批量结果过期提示：追加到汇总行而不是覆盖它，且同一条提示只出现一次。
TEST(ReportingPanelLayout, BatchSummaryNoteAppendsWithoutClobberingTotals) {
    ASSERT_NE(EnsureApp(), nullptr);

    BatchScanView view;
    view.SetSummaryText(QStringLiteral("共 3 个，成功 2 个"));
    view.AppendSummaryNote(QStringLiteral("注意：结果基于旧模板"));
    view.AppendSummaryNote(QStringLiteral("注意：结果基于旧模板"));

    EXPECT_EQ(view.SummaryText(), QStringLiteral("共 3 个，成功 2 个\n注意：结果基于旧模板"));
}

// 对比对话框的核心口径：容差决定"算不算差异"，筛选决定"显示不显示"，
// 两者都只重算 CompareRuns，不重新分析文件。这里用两份最小结果钉住边界。
TEST(ReportingPanelLayout, CompareDialogToleranceAndOnlyDiffFilter) {
    ASSERT_NE(EnsureApp(), nullptr);

    videoeye::qc::QcRunResult left;
    left.ok = true;
    left.report.file_name = "left.mp4";
    left.report.verdict = "通过";
    left.report.score = 90.0;
    left.report.duration_seconds = 10.0;
    left.analysis.container_format = "MP4";
    left.analysis.duration_seconds = 10.0;
    left.analysis.file_size_bytes = 1048576;
    left.analysis.overall_bitrate_bps = 1000000;

    videoeye::qc::QcRunResult right = left;
    right.report.file_name = "right.mp4";
    right.report.duration_seconds = 10.02;
    right.analysis.duration_seconds = 10.02;

    videoeye::ui::QcCompareDialog dialog(left, right);
    // 严格口径：文件名 + 文件级时长 + 容器级时长，至少 3 项不同。
    EXPECT_GE(dialog.DifferentCount(), 3);

    auto* table = dialog.findChild<QTableWidget*>();
    ASSERT_NE(table, nullptr);
    auto row_of = [table](const QString& field) -> int {
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 1) && table->item(row, 1)->text() == field)
                return row;
        }
        return -1;
    };
    const int score_row = row_of(QStringLiteral("评分"));
    const int name_row = row_of(QStringLiteral("文件名"));
    ASSERT_GE(score_row, 0);
    ASSERT_GE(name_row, 0);

    auto* only_diff = dialog.findChild<QCheckBox*>();
    auto* tolerance = dialog.findChild<QDoubleSpinBox*>();
    ASSERT_NE(only_diff, nullptr);
    ASSERT_NE(tolerance, nullptr);

    // 一致项被隐藏，差异项仍然可见。
    only_diff->setChecked(true);
    EXPECT_TRUE(table->isRowHidden(score_row));
    EXPECT_FALSE(table->isRowHidden(name_row));

    // 1% 容差（10s 上即 0.1s）吞掉 0.02s 的时长差，只剩文件名不同。
    tolerance->setValue(1.0);
    EXPECT_EQ(dialog.DifferentCount(), 1);
    EXPECT_FALSE(table->isRowHidden(name_row));
}

// 批量行的「与当前文件对比」只对分析成功的条目开放，且点击时带出该行的原始路径
// （右侧会在面板里按同一模板重新分析，不需要批量结果驻留分析数据）。
TEST(ReportingPanelLayout, BatchCompareButtonOnlyEnabledForSucceededRows) {
    ASSERT_NE(EnsureApp(), nullptr);

    BatchScanView view;
    view.BeginScan();
    view.AppendResult(MakeItem("/media/clip_ok.mp4", BatchItemStatus::Succeeded));
    view.AppendResult(MakeItem("/media/clip_broken.mxf", BatchItemStatus::Failed));
    view.SetBusy(false);

    QPushButton* compare = nullptr;
    for (QPushButton* button : view.findChildren<QPushButton*>()) {
        if (button->text().contains(QStringLiteral("与当前文件对比"))) {
            compare = button;
            break;
        }
    }
    ASSERT_NE(compare, nullptr);

    auto* table = view.findChild<QTableWidget*>();
    ASSERT_NE(table, nullptr);

    // SetBusy(false) 会打开排序，行序不再等于追加顺序；按结果索引定位行。
    auto select_result = [table](int index) -> bool {
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem* item = table->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt() == index) {
                table->selectRow(row);
                return true;
            }
        }
        return false;
    };

    ASSERT_TRUE(select_result(1)); // 失败行：没有可比结果
    EXPECT_FALSE(compare->isEnabled());
    ASSERT_TRUE(select_result(0)); // 成功行
    EXPECT_TRUE(compare->isEnabled());

    QString emitted;
    QObject::connect(&view, &BatchScanView::CompareWithCurrentRequested,
                     [&emitted](const QString& path) { emitted = path; });
    compare->click();
    EXPECT_EQ(emitted, QStringLiteral("/media/clip_ok.mp4"));
}

// 批量参数跨会话保留：Load 把 QSettings 填进控件，Save 再把当前控件值写回。
// 用 IniFormat + 临时目录隔离，避免测试污染真实用户配置。
TEST(ReportingPanelLayout, BatchSettingsPersistRoundTrip) {
    ASSERT_NE(EnsureApp(), nullptr);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    struct FormatGuard {
        ~FormatGuard() {
            QSettings::setDefaultFormat(QSettings::NativeFormat);
        }
    } guard;

    {
        QSettings settings;
        settings.setValue(QStringLiteral("qc/batch/directory"), QStringLiteral("D:/media"));
        settings.setValue(QStringLiteral("qc/batch/extensions"), QStringLiteral("mp4,mkv"));
        settings.setValue(QStringLiteral("qc/batch/out_dir"), QStringLiteral("D:/reports"));
        settings.setValue(QStringLiteral("qc/batch/jobs"), 6);
        settings.setValue(QStringLiteral("qc/batch/timeout_seconds"), 45);
        settings.setValue(QStringLiteral("qc/batch/idle_timeout_seconds"), 90);
        settings.setValue(QStringLiteral("qc/batch/visual_qc"), true);
        settings.setValue(QStringLiteral("qc/batch/recursive"), false);
        settings.sync();
    }

    BatchScanView view;
    view.LoadPersistedSettings();
    const BatchScanView::Settings loaded = view.CurrentSettings();
    EXPECT_EQ(view.DirectoryText(), QStringLiteral("D:/media"));
    EXPECT_EQ(loaded.out_dir, "D:/reports");
    ASSERT_EQ(loaded.extensions.size(), 2u);
    EXPECT_EQ(loaded.extensions[0], "mp4");
    EXPECT_EQ(loaded.extensions[1], "mkv");
    EXPECT_EQ(loaded.jobs, 6);
    EXPECT_EQ(loaded.timeout_seconds, 45);
    EXPECT_EQ(loaded.idle_timeout_seconds, 90);
    EXPECT_TRUE(loaded.visual_qc);
    EXPECT_FALSE(loaded.recursive);

    // 反向：清空后由控件保存，键值应与加载前一致（证明 Save 覆盖了全部字段）。
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }
    view.SavePersistedSettings();
    QSettings saved;
    EXPECT_EQ(saved.value(QStringLiteral("qc/batch/directory")).toString(), QStringLiteral("D:/media"));
    EXPECT_EQ(saved.value(QStringLiteral("qc/batch/extensions")).toString(), QStringLiteral("mp4,mkv"));
    EXPECT_EQ(saved.value(QStringLiteral("qc/batch/out_dir")).toString(), QStringLiteral("D:/reports"));
    EXPECT_EQ(saved.value(QStringLiteral("qc/batch/jobs")).toInt(), 6);
    EXPECT_EQ(saved.value(QStringLiteral("qc/batch/timeout_seconds")).toInt(), 45);
    EXPECT_EQ(saved.value(QStringLiteral("qc/batch/idle_timeout_seconds")).toInt(), 90);
    EXPECT_TRUE(saved.value(QStringLiteral("qc/batch/visual_qc")).toBool());
    EXPECT_FALSE(saved.value(QStringLiteral("qc/batch/recursive")).toBool());
}
