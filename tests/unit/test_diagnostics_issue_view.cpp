// 「问题清单」子页的回归（ui/analysis_panel/DiagnosticsIssueView）。
//
// 这块守的是三类"改错了不报错、只是用户看不懂"的口径：
//   1. 主表只有四列可扫读字段（严重度 / 问题 / 位置 / 类别）。说明与建议是长文本，
//      必须待在右侧详情里 —— 它们回到表里就会把表格撑成横向滚动，四列全被挤成缝。
//   2. 首页「异常直达」按问题身份选中行。目标被当前筛选挡住时必须先把筛选放开：
//      否则用户几轮之前选过"只看警告"，从首页点一条错误进来就是"点了没反应"。
//   3. 定位动作只对带时间的问题可用：文件级问题没有时间点，按钮置灰、不发 seek。
//      （"全局问题也发 seek(0)"会让播放器跳到片头，看起来像跳错了位置。）
//
// 只构造控件（offscreen），不渲染。

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>

#include <string>
#include <vector>

#include "core/domain/model/AnalysisResult.h"
#include "core/domain/model/DiagnosticIssue.h"
#include "core/domain/model/QcReport.h"
#include "core/domain/model/TimeRange.h"
#include "ui/analysis_panel/DiagnosticsIssueView.h"

using videoeye::model::AnalysisResult;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::model::QcReport;
using videoeye::model::TimeRange;
using videoeye::ui::DiagnosticsIssueView;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_diagnostics_issue_view";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

DiagnosticIssue MakeIssue(std::string rule_id, std::string title, IssueSeverity severity, IssueCategory category,
                          TimeRange range, int stream_index, double metric = 0.0, double threshold = 0.0) {
    DiagnosticIssue issue;
    issue.rule_id = std::move(rule_id);
    issue.title = std::move(title);
    issue.detail = "实测值超出阈值，容器声明与实际不一致";
    issue.suggestion = "重新封装该片段并核对索引表";
    issue.severity = severity;
    issue.category = category;
    issue.range = range;
    issue.stream_index = stream_index;
    issue.metric_value = metric;
    issue.threshold = threshold;
    return issue;
}

// 三条问题：一条视频级（带时间 + 阈值）、一条全局容器级、一条提示级音频。
std::vector<DiagnosticIssue> SampleIssues() {
    return {
        MakeIssue("video.gop.max_seconds", "GOP 偏长", IssueSeverity::Error, IssueCategory::Gop, TimeRange::At(12.5), 1,
                  6.0, 2.0),
        MakeIssue("container.structure.missing_atom", "容器结构残缺", IssueSeverity::Critical, IssueCategory::Container,
                  TimeRange::Global(), -1),
        MakeIssue("audio.clipping", "削波", IssueSeverity::Info, IssueCategory::Audio, TimeRange::At(3.25), 2),
    };
}

QString Cell(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item == nullptr ? QString() : item->text();
}

QTableWidget* IssueTable(QWidget* view) {
    return view->findChild<QTableWidget*>();
}

QComboBox* ComboByItems(QWidget* view, const QString& first_item_text) {
    for (QComboBox* combo : view->findChildren<QComboBox*>()) {
        if (combo->count() > 0 && combo->itemText(0).contains(first_item_text))
            return combo;
    }
    return nullptr;
}

QPushButton* JumpButton(QWidget* view) {
    for (QPushButton* button : view->findChildren<QPushButton*>()) {
        if (button->text().contains(QStringLiteral("跳转")))
            return button;
    }
    return nullptr;
}

void Feed(DiagnosticsIssueView& view, const std::vector<DiagnosticIssue>& issues) {
    // 视图对 result 与 report 都只借用引用（生命周期由调用方保证），所以这里用
    // 静态对象而不是栈对象 —— 栈对象出了函数就悬空，后续任何一次刷新都会踩野指针。
    static AnalysisResult result; // 曲线部分空着即可：本用例只关心问题表与详情
    static QcReport report;
    result = AnalysisResult{};
    report = QcReport{};
    report.issues = issues;
    report.score = 72.0;
    report.verdict = "警告";
    view.SetReport(result, report);
}

} // namespace

// 1) 主表 = 四列可扫读字段，行序与报告一致
TEST(DiagnosticsIssueViewTests, MainTableKeepsOnlyFourScannableColumns) {
    EnsureApp();
    DiagnosticsIssueView view;
    const std::vector<DiagnosticIssue> issues = SampleIssues();
    Feed(view, issues);

    QTableWidget* table = IssueTable(&view);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->columnCount(), 4);
    ASSERT_EQ(table->rowCount(), 3);
    EXPECT_EQ(Cell(table, 0, 0), QString::fromStdString(issues[0].SeverityText()));
    EXPECT_EQ(Cell(table, 0, 1), QStringLiteral("GOP 偏长"));
    EXPECT_EQ(Cell(table, 0, 2), QString::fromStdString(issues[0].range.ToString()));
    EXPECT_EQ(Cell(table, 0, 3), QString::fromStdString(issues[0].CategoryText()));

    // 说明 / 建议这类长文本不该出现在表里（回到表里就是横向滚动的开始）
    for (int column = 0; column < table->columnCount(); ++column) {
        EXPECT_NE(Cell(table, 0, column), QString::fromStdString(issues[0].detail));
        EXPECT_NE(Cell(table, 0, column), QString::fromStdString(issues[0].suggestion));
    }
}

// 2) 详情跟随选中行：说明 / 证据 / 建议三块都要在
TEST(DiagnosticsIssueViewTests, DetailPanelFollowsSelection) {
    EnsureApp();
    DiagnosticsIssueView view;
    Feed(view, SampleIssues());

    QTableWidget* table = IssueTable(&view);
    ASSERT_NE(table, nullptr);
    QTextBrowser* detail = view.findChild<QTextBrowser*>();
    ASSERT_NE(detail, nullptr);

    table->setCurrentCell(0, 0);
    const QString text = detail->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("GOP 偏长")));
    EXPECT_TRUE(text.contains(QStringLiteral("实测值超出阈值，容器声明与实际不一致")));
    EXPECT_TRUE(text.contains(QStringLiteral("重新封装该片段并核对索引表")));
    // 证据：规则 id 与实测/阈值
    EXPECT_TRUE(text.contains(QStringLiteral("video.gop.max_seconds")));
    EXPECT_TRUE(text.contains(QStringLiteral("6.00")));
    EXPECT_TRUE(text.contains(QStringLiteral("2.00")));

    // 换一行，详情必须跟着换（否则右侧永远显示第一条，等于没有详情区）
    table->setCurrentCell(2, 0);
    const QString second = detail->toPlainText();
    EXPECT_TRUE(second.contains(QStringLiteral("削波")));
    EXPECT_FALSE(second.contains(QStringLiteral("GOP 偏长")));
}

// 3) 严重度 / 类别筛选：单独与组合都要生效，且行号映射不能错位
TEST(DiagnosticsIssueViewTests, SeverityAndCategoryFiltersNarrowTheTable) {
    EnsureApp();
    DiagnosticsIssueView view;
    Feed(view, SampleIssues());

    QTableWidget* table = IssueTable(&view);
    ASSERT_NE(table, nullptr);
    QComboBox* severity = ComboByItems(&view, QStringLiteral("全部严重度"));
    QComboBox* category = ComboByItems(&view, QStringLiteral("全部类别"));
    ASSERT_NE(severity, nullptr);
    ASSERT_NE(category, nullptr);

    // 只看错误（issue[0]）
    severity->setCurrentIndex(severity->findData(static_cast<int>(IssueSeverity::Error)));
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(Cell(table, 0, 1), QStringLiteral("GOP 偏长"));
    // 行号变了，跳转仍要落到"错误"这一条上（筛选后行号 != 报告下标）
    EXPECT_EQ(table->rowCount(), 1);
    EXPECT_EQ(Cell(table, 0, 3), QStringLiteral("GOP"));

    // 错误 + 音频类别 = 空；筛选是"与"不是"或"
    category->setCurrentIndex(category->findData(static_cast<int>(IssueCategory::Audio)));
    EXPECT_EQ(table->rowCount(), 0);

    // 只留类别筛选（音频） -> 只剩提示级那条
    severity->setCurrentIndex(0);
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(Cell(table, 0, 1), QStringLiteral("削波"));

    // 回到"全部" -> 三条全回来
    category->setCurrentIndex(0);
    EXPECT_EQ(table->rowCount(), 3);
}

// 4) 首页"异常直达"：按身份选中正确的行（不是按行号猜）
TEST(DiagnosticsIssueViewTests, SelectIssuePicksTheRowWithMatchingIdentity) {
    EnsureApp();
    DiagnosticsIssueView view;
    const std::vector<DiagnosticIssue> issues = SampleIssues();
    Feed(view, issues);

    QTableWidget* table = IssueTable(&view);
    ASSERT_NE(table, nullptr);

    // 第三条（音频削波）：位置与流都要参与匹配，否则会命中第一条
    EXPECT_TRUE(view.SelectIssue(QStringLiteral("audio.clipping"), 3.25, 2));
    EXPECT_EQ(table->currentRow(), 2);
    EXPECT_EQ(view.CurrentIssueIndex(), 2);

    // 同一规则但位置不同的身份：精确匹配失败后应退到同规则的第一条，而不是不响应
    EXPECT_TRUE(view.SelectIssue(QStringLiteral("video.gop.max_seconds"), 999.0, 1));
    EXPECT_EQ(table->currentRow(), 0);

    // 报告里根本没有的规则 -> 未命中，界面保持原样
    EXPECT_FALSE(view.SelectIssue(QStringLiteral("no.such.rule"), 0.0, -1));
    EXPECT_EQ(table->currentRow(), 0);
}

// 5) 目标被筛选挡住时，"一步直达"必须先把筛选放开
TEST(DiagnosticsIssueViewTests, SelectIssueClearsFiltersThatWouldHideTheTarget) {
    EnsureApp();
    DiagnosticsIssueView view;
    Feed(view, SampleIssues());

    QTableWidget* table = IssueTable(&view);
    QComboBox* severity = ComboByItems(&view, QStringLiteral("全部严重度"));
    ASSERT_NE(table, nullptr);
    ASSERT_NE(severity, nullptr);

    // 用户先选了"只看提示"，此时错误那条根本不在表里
    severity->setCurrentIndex(severity->findData(static_cast<int>(IssueSeverity::Info)));
    ASSERT_EQ(table->rowCount(), 1);

    // 从首页点一条错误进来：必须命中，而不是"点了没反应"
    ASSERT_TRUE(view.SelectIssue(QStringLiteral("video.gop.max_seconds"), 12.5, 1));
    EXPECT_EQ(severity->currentIndex(), 0) << "命中前应把筛选重置为全部";
    EXPECT_EQ(table->rowCount(), 3);
    EXPECT_EQ(Cell(table, table->currentRow(), 1), QStringLiteral("GOP 偏长"));
}

// 6) 定位动作只对带时间的问题可用
TEST(DiagnosticsIssueViewTests, JumpIsOnlyEnabledForTimedIssues) {
    EnsureApp();
    DiagnosticsIssueView view;
    Feed(view, SampleIssues());

    QTableWidget* table = IssueTable(&view);
    QPushButton* jump = JumpButton(&view);
    ASSERT_NE(table, nullptr);
    ASSERT_NE(jump, nullptr);

    int seek_count = 0;
    double last_seconds = -1.0;
    QObject::connect(&view, &DiagnosticsIssueView::SeekRequested, [&](double seconds) {
        ++seek_count;
        last_seconds = seconds;
    });

    // 带时间的问题：可跳，且跳的是选中行的位置
    table->setCurrentCell(0, 0);
    EXPECT_TRUE(jump->isEnabled());
    jump->click();
    EXPECT_EQ(seek_count, 1);
    EXPECT_DOUBLE_EQ(last_seconds, 12.5);

    // 文件级问题：按钮置灰，点了也不发 seek（曾经会跳到片头）
    table->setCurrentCell(1, 0);
    EXPECT_FALSE(jump->isEnabled()) << "文件级问题没有可跳转的时间点";
    jump->click();
    EXPECT_EQ(seek_count, 1);
}

// 7) 清空后详情与表一起复位（换文件时不能让上一份结论留在屏幕上）
TEST(DiagnosticsIssueViewTests, ClearResetsTableAndDetail) {
    EnsureApp();
    DiagnosticsIssueView view;
    Feed(view, SampleIssues());
    QTableWidget* table = IssueTable(&view);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 3);

    view.Clear();
    EXPECT_EQ(table->rowCount(), 0);
    EXPECT_EQ(view.CurrentIssueIndex(), -1);
    QTextBrowser* detail = view.findChild<QTextBrowser*>();
    ASSERT_NE(detail, nullptr);
    EXPECT_TRUE(detail->toPlainText().contains(QStringLiteral("选择上方任意一条问题")));
    EXPECT_FALSE(JumpButton(&view)->isEnabled());
}
