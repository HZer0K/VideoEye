// 首页（总览）的布局回归：三个分组框必须真的进布局、按序排开、互不压字。
//
// 历史 bug（用户截图"分析状态文字重叠"）：OverviewPage::BuildUi 里 new 了
// 「分析状态」分组框，也给它建了布局、塞了结论行/按钮/异常表，但漏了
// `root->addWidget(analysis_group_)`。控件不进布局就永远停在默认几何
// 100x30 @ (0,0)，被画在页头上面 —— 标题「分析状态」与自己的结论行叠写，
// 看起来像"字号被压扁"。
//
// 为什么必须单独守这条：数据链路是对的（test_overview_analysis 全绿），
// 只是没上屏。纯逻辑测试和"构造控件查文案"的测试都发现不了，
// 只有比对控件的实际几何才抓得住：
//   1) 每个可见的直接子控件都被页面矩形包住，且两两不重叠；
//   2) 三个分组框自上而下 = 文件摘要 → 分析状态 → 完整媒体信息；
//   3) 分析状态分组的高度不小于自身 minimumSizeHint（没被压扁），
//      组内异常表可见。
//
// 只构造控件、不渲染（offscreen），不依赖显示器与主题。

#include <gtest/gtest.h>

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <algorithm>
#include <string>
#include <vector>

#include "core/domain/model/FrameData.h"
#include "ui/main_window/OverviewPage.h"

using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::model::StreamInfo;
using videoeye::ui::OverviewPage;

namespace {

QApplication* EnsureApp() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication* app = [] {
        static int argc = 1;
        static char name[] = "test_overview_page_layout";
        static char* argv[] = {name, nullptr};
        return new QApplication(argc, argv);
    }();
    return app;
}

// 分组框标题是 tr() 的源文本（无翻译器时原样返回），这里按标题找控件，
// 不去碰页面的私有成员。
QGroupBox* FindGroup(QWidget* page, const QString& title) {
    for (QGroupBox* group : page->findChildren<QGroupBox*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (group->title() == title)
            return group;
    }
    return nullptr;
}

std::vector<QGroupBox*> DirectGroups(QWidget* page) {
    std::vector<QGroupBox*> groups;
    for (QGroupBox* group : page->findChildren<QGroupBox*>(QString(), Qt::FindDirectChildrenOnly))
        groups.push_back(group);
    return groups;
}

DiagnosticIssue MakeIssue(IssueSeverity severity, const std::string& title) {
    DiagnosticIssue issue;
    issue.rule_id = "test.rule";
    issue.title = title;
    issue.detail = "detail";
    issue.severity = severity;
    issue.category = IssueCategory::Video;
    return issue;
}

// 一份"已打开文件 + 扫描成功"的首页状态：走的是 MainWindow 的真实喂数据路径。
void FeedOpenedFileWithResult(OverviewPage& page) {
    StreamInfo info;
    info.filename = "D:/tmp/overview_layout_probe.mp4";
    info.extractor.format = "QuickTime / MOV";
    info.extractor.duration = "1 min";
    info.extractor.file_size = "10.0 MiB";
    info.extractor.overall_bit_rate = "872 kb/s";
    info.video.format = "h264";
    info.video.width = "1280";
    info.video.height = "720";
    info.video.frame_rate = "25.000";
    info.video.bit_depth = "8";
    info.audio.format = "aac";
    info.audio.channels = "2";
    info.audio.sampling_rate = "48000";
    page.SetFileSummary(info);
    page.SetMediaInfoText(QStringLiteral("General\n  Complete name : probe.mp4\n"));

    OverviewPage::AnalysisSummary summary;
    summary.has_file = true;
    summary.has_result = true;
    summary.score = 91.5;
    summary.verdict = QStringLiteral("通过");
    summary.critical = 0;
    summary.error_count = 1;
    summary.warning = 2;
    summary.info = 1;
    summary.top_issues = {MakeIssue(IssueSeverity::Error, "参考帧缺失"), MakeIssue(IssueSeverity::Warning, "GOP 偏长")};
    page.SetAnalysisSummary(summary);
}

// 页面摆到一个真实尺寸并真正走一遍布局（show 之后布局才会落到控件上）。
void ShowAtRealisticSize(OverviewPage& page) {
    page.resize(1600, 900);
    page.show();
    QApplication::processEvents();
}

bool Intersects(const QRect& a, const QRect& b) {
    return a.intersects(b);
}

} // namespace

// 1) 任何一个可见的直接子控件都不许与兄弟重叠，且都要完整落在页面内。
//    漏 addWidget 的控件会停在 100x30 @ (0,0)，必然与页头那两个标签相交 ——
//    这条就是当年叠字的最小复现断言。
TEST(OverviewPageLayout, VisibleChildrenDoNotOverlapAndStayInsidePage) {
    EnsureApp();
    OverviewPage page;
    FeedOpenedFileWithResult(page);
    ShowAtRealisticSize(page);

    std::vector<QWidget*> visible;
    for (QWidget* child : page.findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!child->isVisible())
            continue;
        visible.push_back(child);
    }
    ASSERT_GE(visible.size(), 5u) << "首页应有页头标签、文件摘要/分析状态/媒体信息三个分组";

    for (QWidget* child : visible) {
        const QRect rect = child->geometry();
        EXPECT_TRUE(page.rect().contains(rect))
            << "控件越出页面: " << child->metaObject()->className() << " " << rect.x() << "," << rect.y() << " "
            << rect.width() << "x" << rect.height();
        EXPECT_GT(rect.width(), 0);
        EXPECT_GT(rect.height(), 0);
    }

    for (std::size_t i = 0; i < visible.size(); ++i) {
        for (std::size_t j = i + 1; j < visible.size(); ++j) {
            const QRect a = visible[i]->geometry();
            const QRect b = visible[j]->geometry();
            EXPECT_FALSE(Intersects(a, b))
                << "两个首页控件画在同一块区域（叠字）：\n  " << visible[i]->metaObject()->className() << " " << a.x()
                << "," << a.y() << " " << a.width() << "x" << a.height() << "\n  "
                << visible[j]->metaObject()->className() << " " << b.x() << "," << b.y() << " " << b.width() << "x"
                << b.height();
        }
    }
}

// 2) 三个分组框按文档顺序自上而下排列。
TEST(OverviewPageLayout, GroupsFollowDocumentedOrderTopDown) {
    EnsureApp();
    OverviewPage page;
    FeedOpenedFileWithResult(page);
    ShowAtRealisticSize(page);

    QGroupBox* summary = FindGroup(&page, QStringLiteral("文件摘要"));
    QGroupBox* analysis = FindGroup(&page, QStringLiteral("分析状态"));
    QGroupBox* mediainfo = FindGroup(&page, QStringLiteral("完整媒体信息"));
    ASSERT_NE(summary, nullptr);
    ASSERT_NE(analysis, nullptr);
    ASSERT_NE(mediainfo, nullptr);

    EXPECT_LT(summary->y(), analysis->y()) << "「分析状态」应排在「文件摘要」下面";
    EXPECT_LT(analysis->y(), mediainfo->y()) << "「完整媒体信息」应排在「分析状态」下面";

    // 分组框必须铺满页面可用宽度 —— 停在 100px 宽就是"没进布局"的指纹。
    for (QGroupBox* group : {summary, analysis, mediainfo}) {
        EXPECT_GT(group->width(), page.width() / 2) << "分组框宽度异常: " << group->title().toStdString();
    }
}

// 3) 分析状态分组不被压扁：高度不低于自己的 minimumSizeHint，异常表可见。
//    （只靠"不重叠"还不够 —— 布局真把它压到 30px 时也不会与兄弟重叠，
//    但标题会和内容挤在一起。）
TEST(OverviewPageLayout, AnalysisGroupKeepsItsMinimumHeight) {
    EnsureApp();
    OverviewPage page;
    FeedOpenedFileWithResult(page);
    ShowAtRealisticSize(page);

    QGroupBox* analysis = FindGroup(&page, QStringLiteral("分析状态"));
    ASSERT_NE(analysis, nullptr);
    ASSERT_TRUE(analysis->isVisible());

    EXPECT_GE(analysis->height(), analysis->minimumSizeHint().height())
        << "分析状态分组被压扁: " << analysis->height() << " < " << analysis->minimumSizeHint().height();

    // 组内三件套都要在：结论行、重新分析按钮、最重要的异常表。
    const QList<QTableWidget*> tables = analysis->findChildren<QTableWidget*>();
    ASSERT_EQ(tables.size(), 1);
    QTableWidget* issues = tables.first();
    EXPECT_TRUE(issues->isVisible());
    EXPECT_GE(issues->height(), issues->minimumHeight()) << "异常表被压到最小高度以下";
    EXPECT_EQ(issues->rowCount(), 2) << "两条 top_issues 应各占一行";
    EXPECT_NE(analysis->findChild<QPushButton*>(), nullptr) << "「重新分析」按钮应在分析状态组内";
}

// 4) 未打开文件时三个分组都收起，只留空态提示 —— 守住修复没把空态搞坏。
TEST(OverviewPageLayout, EmptyStateKeepsGroupsHidden) {
    EnsureApp();
    OverviewPage page;
    page.resize(1600, 900);
    page.show();
    QApplication::processEvents();

    for (QGroupBox* group : DirectGroups(&page)) {
        EXPECT_FALSE(group->isVisible()) << "未打开文件时不该显示分组: " << group->title().toStdString();
    }
    bool empty_hint_visible = false;
    for (QLabel* label : page.findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (label->isVisible() && label->wordWrap())
            empty_hint_visible = true;
    }
    EXPECT_TRUE(empty_hint_visible) << "空态提示应在未打开文件时可见";
}
