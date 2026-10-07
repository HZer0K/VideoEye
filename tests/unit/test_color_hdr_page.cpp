// ColorHdrPage 的页面级行为测试（评审「页面组件补测」）。
//
// 这一页的三条链都属于静默回归高发区：
//   1) 缺失值高亮：容器没写 primaries / 没带 mastering display 时，「缺失 / 未标注」
//      必须橙色加粗 —— 掉了只会变成普通黑字，扫一眼根本看不出来；
//   2) 异常表只收 category=ColorHdr 的问题 —— 收错类别会把视频 / 音频问题
//      错误地塞进色彩页；
//   3) 扫描态互斥：开始 / 取消按钮与 IsScanActive 必须同步，卡在「扫描中」不崩也不报。
//
// 只构造控件、不渲染（offscreen 平台）；导出按钮会弹 QFileDialog 模态框，不进单测。

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>

#include <string>

#include "core/analysis/AnalysisOptions.h"
#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/ColorHdrResult.h"
#include "core/domain/model/DiagnosticIssue.h"
#include "core/domain/model/QcReport.h"
#include "ui/analysis_panel/ColorHdrPage.h"

using videoeye::model::AnalysisResult;
using videoeye::model::ColorHdrAnalysis;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::model::QcReport;
using videoeye::ui::ColorHdrPage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_color_hdr_page";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 完整标注的 HDR10：色彩四项全齐、位深 10bit；刻意不给 mastering display，
// 让 HDR 表出现「缺失」行来验证高亮分支。
ColorHdrAnalysis MakeHdr10Analysis() {
    ColorHdrAnalysis analysis;
    analysis.analyzed = true;
    analysis.stream_index = 0;
    analysis.color.codec_name = "hevc";
    analysis.color.profile_name = "Main 10";
    analysis.color.width = 3840;
    analysis.color.height = 2160;
    analysis.color.primaries = videoeye::model::ColorPrimariesKind::Bt2020;
    analysis.color.transfer = videoeye::model::TransferKind::Pq;
    analysis.color.matrix = videoeye::model::MatrixKind::Bt2020Ncl;
    analysis.color.range = videoeye::model::ColorRangeKind::Limited;
    analysis.color.primaries_name = "BT.2020";
    analysis.color.transfer_name = "PQ";
    analysis.color.matrix_name = "BT.2020 NCL";
    analysis.color.range_name = "Limited";
    analysis.color.pixel_format.name = "yuv420p10le";
    analysis.color.pixel_format.valid = true;
    analysis.color.pixel_format.bit_depth = 10;
    analysis.color.pixel_format.chroma_subsampling = "4:2:0";

    analysis.hdr.format_name = "HDR10";
    analysis.hdr.hdr = true;
    return analysis;
}

AnalysisResult MakeResultWith(const ColorHdrAnalysis& analysis) {
    AnalysisResult result;
    result.file_path = "demo.mkv";
    result.color_hdr = analysis;
    return result;
}

// color_info_table_ 与 hdr_info_table_ 表头完全相同（项目 / 值 / 说明），
// 只能经页内唯一 QTabWidget 的 tab 索引定位：0=色彩信息 / 1=HDR 元数据 / 2=异常组合。
QTableWidget* TableInTab(ColorHdrPage* page, int index) {
    QTabWidget* tabs = page->findChild<QTabWidget*>();
    if (!tabs || index < 0 || index >= tabs->count()) return nullptr;
    QWidget* tab = tabs->widget(index);
    return tab ? tab->findChild<QTableWidget*>() : nullptr;
}

QPushButton* FindButton(ColorHdrPage* page, const QString& text) {
    const auto buttons = page->findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

QLabel* FindLabelContaining(ColorHdrPage* page, const QString& needle) {
    const auto labels = page->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text().contains(needle)) return label;
    }
    return nullptr;
}

}  // namespace

// 未喂结果时刷新：空态文案 + 三张表全空（异常表连占位行都不该有）。
TEST(ColorHdrPageTests, UnanalyzedShowsEmptyState) {
    EnsureApp();
    ColorHdrPage page;

    page.Refresh();
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("暂无色彩/HDR 结果"));
    ASSERT_TRUE(summary != nullptr);

    for (int tab = 0; tab < 3; ++tab) {
        QTableWidget* table = TableInTab(&page, tab);
        ASSERT_TRUE(table != nullptr) << "tab " << tab;
        EXPECT_EQ(table->rowCount(), 0);
    }
}

// SetResult 填充两张信息表（行数与 domain 行构造器一致）、缺失值橙色加粗、
// 汇总行带 HDR 格式与「待核查」结论。
TEST(ColorHdrPageTests, SetResultFillsTablesAndMarksMissingValues) {
    EnsureApp();
    ColorHdrPage page;
    const ColorHdrAnalysis analysis = MakeHdr10Analysis();
    page.SetResult(MakeResultWith(analysis), QcReport{});

    // 两张信息表的行数必须与 domain 的行构造器一致（页面只负责摆行）
    QTableWidget* color_table = TableInTab(&page, 0);
    QTableWidget* hdr_table = TableInTab(&page, 1);
    ASSERT_TRUE(color_table != nullptr);
    ASSERT_TRUE(hdr_table != nullptr);
    EXPECT_EQ(color_table->rowCount(),
              static_cast<int>(videoeye::model::BuildColorRows(analysis).size()));
    EXPECT_EQ(hdr_table->rowCount(),
              static_cast<int>(videoeye::model::BuildHdrRows(analysis).size()));
    EXPECT_GT(color_table->rowCount(), 0);

    // 色彩表里能找到「像素格式 → yuv420p10le」
    bool found_pixel_format = false;
    for (int row = 0; row < color_table->rowCount(); ++row) {
        if (color_table->item(row, 0)->text() == QStringLiteral("像素格式") &&
            color_table->item(row, 1)->text() == QStringLiteral("yuv420p10le")) {
            found_pixel_format = true;
        }
    }
    EXPECT_TRUE(found_pixel_format);

    // 「缺失」值必须橙色加粗（没给 mastering display -> HDR 表必有缺失行）
    int missing_rows = 0;
    for (int row = 0; row < hdr_table->rowCount(); ++row) {
        QTableWidgetItem* item = hdr_table->item(row, 1);
        if (!item || item->text() != QStringLiteral("缺失")) continue;
        ++missing_rows;
        EXPECT_EQ(item->foreground().color(), QColor("#fb8c00"));
        EXPECT_TRUE(item->font().bold());
    }
    EXPECT_GT(missing_rows, 0);

    // 汇总行：HDR10 + PQ 但静态元数据不齐 -> 「存在待核查项」
    QLabel* summary = FindLabelContaining(&page, QStringLiteral("HDR10"));
    ASSERT_TRUE(summary != nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("存在待核查项")));
}

// 异常表只收 category=ColorHdr 的问题；只换报告时色彩表不受影响、空报告落占位行。
TEST(ColorHdrPageTests, IssueTableOnlyKeepsColorHdrCategory) {
    EnsureApp();
    ColorHdrPage page;

    DiagnosticIssue color_issue;
    color_issue.rule_id = "colorhdr.transfer_mismatch";
    color_issue.category = IssueCategory::ColorHdr;
    color_issue.severity = IssueSeverity::Warning;
    color_issue.detail = "传递函数与色域不匹配";
    color_issue.suggestion = "按 BT.2020 + PQ 重新标注";

    DiagnosticIssue video_issue;
    video_issue.rule_id = "video.gop.max_seconds";
    video_issue.category = IssueCategory::Video;
    video_issue.severity = IssueSeverity::Error;

    QcReport report;
    report.issues = {color_issue, video_issue};

    const ColorHdrAnalysis analysis = MakeHdr10Analysis();
    page.SetResult(MakeResultWith(analysis), report);

    QTableWidget* issue_table = TableInTab(&page, 2);
    ASSERT_TRUE(issue_table != nullptr);
    ASSERT_EQ(issue_table->rowCount(), 1);  // Video 那条必须被过滤
    EXPECT_EQ(issue_table->item(0, 0)->text(),
              QString::fromStdString(color_issue.SeverityText()));
    EXPECT_EQ(issue_table->item(0, 1)->text(), QString::fromStdString(color_issue.rule_id));
    EXPECT_EQ(issue_table->item(0, 2)->text(), QString::fromStdString(color_issue.detail));
    EXPECT_EQ(issue_table->item(0, 3)->text(), QString::fromStdString(color_issue.suggestion));
    EXPECT_TRUE(issue_table->item(0, 0)->font().bold());

    // 只换报告（规则改动后重新评估）：异常表切到占位行，色彩表行数不动
    const int color_rows_before = TableInTab(&page, 0)->rowCount();
    page.SetQcReport(QcReport{});
    ASSERT_EQ(issue_table->rowCount(), 1);
    EXPECT_EQ(issue_table->item(0, 0)->text(), QStringLiteral("无"));
    EXPECT_EQ(issue_table->item(0, 1)->text(), QStringLiteral("未发现异常的色彩/HDR 组合"));
    EXPECT_EQ(TableInTab(&page, 0)->rowCount(), color_rows_before);
}

// 选项往返（勾选态 -> options）+ 扫描态互斥（按钮使能 / IsScanActive / 进度条）。
TEST(ColorHdrPageTests, OptionsRoundTripAndScanLifecycle) {
    EnsureApp();
    ColorHdrPage page;

    QCheckBox* probe = page.findChild<QCheckBox*>();
    ASSERT_TRUE(probe != nullptr);
    EXPECT_TRUE(probe->isChecked());  // ColorHdrOptions::probe_decoded_frame 默认 true
    EXPECT_FALSE(page.IsScanActive());

    // FillScanOptions：以本页勾选状态覆盖 options，并打开 analyze_color_hdr
    videoeye::AnalysisOptions options;
    page.FillScanOptions(options);
    EXPECT_TRUE(options.analyze_color_hdr);
    EXPECT_TRUE(options.color_hdr_options.probe_decoded_frame);

    probe->setChecked(false);
    options = videoeye::AnalysisOptions{};
    page.FillScanOptions(options);
    EXPECT_FALSE(options.color_hdr_options.probe_decoded_frame);

    // 扫描态互斥 + 信号出口（不依赖面板编排）
    QPushButton* start = FindButton(&page, QStringLiteral("开始分析"));
    QPushButton* cancel = FindButton(&page, QStringLiteral("取消"));
    ASSERT_TRUE(start != nullptr);
    ASSERT_TRUE(cancel != nullptr);
    EXPECT_TRUE(start->isEnabled());
    EXPECT_FALSE(cancel->isEnabled());

    bool scan_requested = false;
    bool cancel_requested = false;
    QObject::connect(&page, &ColorHdrPage::ScanRequested,
                     [&scan_requested] { scan_requested = true; });
    QObject::connect(&page, &ColorHdrPage::CancelRequested,
                     [&cancel_requested] { cancel_requested = true; });

    start->click();
    EXPECT_TRUE(scan_requested);

    page.SetScanActive(true);
    EXPECT_TRUE(page.IsScanActive());
    EXPECT_FALSE(start->isEnabled());
    EXPECT_TRUE(cancel->isEnabled());
    cancel->click();
    EXPECT_TRUE(cancel_requested);

    page.SetScanActive(false);
    EXPECT_TRUE(start->isEnabled());
    EXPECT_FALSE(cancel->isEnabled());

    // 进度条：值 + 文案都由面板写
    page.SetProgress(42);
    page.SetProgressFormat(QStringLiteral("扫描中 42%"));
    QProgressBar* bar = page.findChild<QProgressBar*>();
    ASSERT_TRUE(bar != nullptr);
    EXPECT_EQ(bar->value(), 42);
    EXPECT_EQ(bar->format(), QStringLiteral("扫描中 42%"));
}