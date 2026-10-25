// 首页「分析状态」摘要组装口径的回归。
//
// 这份测试守的是三条**业务口径**，不是"排序函数对不对"：
//   1. 「扫描失败」只在没在扫描时成立 —— 扫描重启后旧错误必须让位给「进行中」，
//      否则首页同时挂一个转圈和一个红字；
//   2. Top-N 是严重度降序 + **同级稳定**（保持规则引擎的输出顺序）；
//      用 sort 而不是 stable_sort 会让同一档里的条目顺序随实现漂移，
//      用户前后两次看到的清单顺序不一样；
//   3. 摘要里的异常是**值拷贝** —— 首页活得比任何一次扫描结果都久，
//      留指针会在下一次扫描落地时悬空。
// 这些以前全在 MainWindow::RefreshOverviewAnalysis 的 UI 回调里，测不到。

#include "ui/main_window/overview_analysis.h"

#include <cstddef>
#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

namespace {

using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueSeverity;
using videoeye::model::QcReport;
using videoeye::ui::BuildOverviewAnalysisSummary;
using videoeye::ui::IssueSeverityRank;
using videoeye::ui::kOverviewTopIssueCount;
using videoeye::ui::OverviewAnalysisInput;
using videoeye::ui::OverviewAnalysisSummary;
using videoeye::ui::RankIssuesBySeverity;
using videoeye::ui::TopIssuesBySeverity;

DiagnosticIssue MakeIssue(IssueSeverity severity, const std::string& title) {
    DiagnosticIssue issue;
    issue.severity = severity;
    issue.title = title;
    issue.rule_id = "rule." + title;
    return issue;
}

} // namespace

TEST(OverviewAnalysisTests, SeverityRankIsOrderedBySeriousness) {
    EXPECT_GT(IssueSeverityRank(IssueSeverity::Critical), IssueSeverityRank(IssueSeverity::Error));
    EXPECT_GT(IssueSeverityRank(IssueSeverity::Error), IssueSeverityRank(IssueSeverity::Warning));
    EXPECT_GT(IssueSeverityRank(IssueSeverity::Warning), IssueSeverityRank(IssueSeverity::Info));
}

TEST(OverviewAnalysisTests, RankIssuesSortsBySeverityDescending) {
    const std::vector<DiagnosticIssue> issues = {
        MakeIssue(IssueSeverity::Info, "i1"),
        MakeIssue(IssueSeverity::Critical, "c1"),
        MakeIssue(IssueSeverity::Warning, "w1"),
        MakeIssue(IssueSeverity::Error, "e1"),
    };

    const std::vector<DiagnosticIssue> ranked = RankIssuesBySeverity(issues);

    ASSERT_EQ(ranked.size(), 4u);
    EXPECT_EQ(ranked[0].title, "c1");
    EXPECT_EQ(ranked[1].title, "e1");
    EXPECT_EQ(ranked[2].title, "w1");
    EXPECT_EQ(ranked[3].title, "i1");
}

// 同级保持输入顺序。这是"给用户看的清单"最关键的一条：规则引擎已经按流/规则 id
// 排过一遍，严重度重排只能当**次要键**，不能把同一档内部洗掉。
TEST(OverviewAnalysisTests, RankIssuesKeepsInputOrderWithinSameSeverity) {
    std::vector<DiagnosticIssue> issues;
    // 故意交错：三条 Error 夹在 Warning/Info 之间，同档内部是 e1,e2,e3
    issues.push_back(MakeIssue(IssueSeverity::Warning, "w1"));
    issues.push_back(MakeIssue(IssueSeverity::Error, "e1"));
    issues.push_back(MakeIssue(IssueSeverity::Info, "i1"));
    issues.push_back(MakeIssue(IssueSeverity::Error, "e2"));
    issues.push_back(MakeIssue(IssueSeverity::Warning, "w2"));
    issues.push_back(MakeIssue(IssueSeverity::Error, "e3"));

    const std::vector<DiagnosticIssue> ranked = RankIssuesBySeverity(issues);

    ASSERT_EQ(ranked.size(), 6u);
    EXPECT_EQ(ranked[0].title, "e1");
    EXPECT_EQ(ranked[1].title, "e2");
    EXPECT_EQ(ranked[2].title, "e3");
    EXPECT_EQ(ranked[3].title, "w1");
    EXPECT_EQ(ranked[4].title, "w2");
    EXPECT_EQ(ranked[5].title, "i1");
}

TEST(OverviewAnalysisTests, RankIssuesDoesNotModifyInput) {
    std::vector<DiagnosticIssue> issues = {
        MakeIssue(IssueSeverity::Info, "i1"),
        MakeIssue(IssueSeverity::Critical, "c1"),
    };

    RankIssuesBySeverity(issues);

    ASSERT_EQ(issues.size(), 2u);
    EXPECT_EQ(issues[0].title, "i1"); // 入参原样保留
    EXPECT_EQ(issues[1].title, "c1");
}

TEST(OverviewAnalysisTests, TopIssuesTruncatesToLimit) {
    std::vector<DiagnosticIssue> issues;
    for (int i = 0; i < 8; ++i) {
        issues.push_back(MakeIssue(IssueSeverity::Info, "i" + std::to_string(i)));
    }

    EXPECT_EQ(TopIssuesBySeverity(issues, 3).size(), 3u);

    // limit 大于总数时全给，不补空位
    EXPECT_EQ(TopIssuesBySeverity(issues, 100).size(), 8u);
}

// limit=0 的语义是"不展示"，不是"不限制"——把 0 当不限制会把整份问题清单灌进首页。
TEST(OverviewAnalysisTests, TopIssuesWithZeroLimitIsEmpty) {
    const std::vector<DiagnosticIssue> issues = {MakeIssue(IssueSeverity::Critical, "c1")};
    EXPECT_TRUE(TopIssuesBySeverity(issues, 0).empty());
}

TEST(OverviewAnalysisTests, TopIssuesOnEmptyInputIsEmpty) {
    const std::vector<DiagnosticIssue> empty;
    EXPECT_TRUE(TopIssuesBySeverity(empty).empty());
    EXPECT_TRUE(RankIssuesBySeverity(empty).empty());
}

TEST(OverviewAnalysisTests, DefaultLimitIsThePublishedConstant) {
    std::vector<DiagnosticIssue> issues;
    for (int i = 0; i < 20; ++i) {
        issues.push_back(MakeIssue(IssueSeverity::Info, "i" + std::to_string(i)));
    }
    EXPECT_EQ(TopIssuesBySeverity(issues).size(), kOverviewTopIssueCount);
}

TEST(OverviewAnalysisTests, SummaryCountsIssuesBySeverity) {
    QcReport report;
    report.issues.push_back(MakeIssue(IssueSeverity::Critical, "c1"));
    report.issues.push_back(MakeIssue(IssueSeverity::Critical, "c2"));
    report.issues.push_back(MakeIssue(IssueSeverity::Error, "e1"));
    report.issues.push_back(MakeIssue(IssueSeverity::Warning, "w1"));
    report.issues.push_back(MakeIssue(IssueSeverity::Warning, "w2"));
    report.issues.push_back(MakeIssue(IssueSeverity::Warning, "w3"));
    report.issues.push_back(MakeIssue(IssueSeverity::Info, "i1"));

    OverviewAnalysisInput input;
    input.has_file = true;
    input.has_result = true;
    input.report = &report;

    const OverviewAnalysisSummary summary = BuildOverviewAnalysisSummary(input);

    EXPECT_TRUE(summary.has_result);
    EXPECT_EQ(summary.critical, 2);
    EXPECT_EQ(summary.error_count, 1);
    EXPECT_EQ(summary.warning, 3);
    EXPECT_EQ(summary.info, 1);
}

TEST(OverviewAnalysisTests, SummaryCopiesScoreVerdictAndPartialFlag) {
    QcReport report;
    report.score = 62.5;
    report.verdict = "警告";
    report.completed = false; // 被取消 -> 结果不完整

    OverviewAnalysisInput input;
    input.has_file = true;
    input.has_result = true;
    input.report = &report;

    const OverviewAnalysisSummary summary = BuildOverviewAnalysisSummary(input);

    EXPECT_DOUBLE_EQ(summary.score, 62.5);
    EXPECT_EQ(summary.verdict, QString::fromUtf8("警告"));
    EXPECT_TRUE(summary.partial);

    report.completed = true;
    EXPECT_FALSE(BuildOverviewAnalysisSummary(input).partial);
}

TEST(OverviewAnalysisTests, SummaryTopIssuesAreSeverityOrderedAndCapped) {
    QcReport report;
    // 六条 Info 打头、两条 Critical 收尾：取前 5 条后 Critical 必须浮上来，
    // 否则"最重要的异常"里全是提示级，这条摘要就没有任何价值。
    for (int i = 0; i < 6; ++i) {
        report.issues.push_back(MakeIssue(IssueSeverity::Info, "i" + std::to_string(i)));
    }
    report.issues.push_back(MakeIssue(IssueSeverity::Error, "e0"));
    report.issues.push_back(MakeIssue(IssueSeverity::Critical, "c0"));
    report.issues.push_back(MakeIssue(IssueSeverity::Critical, "c1"));

    OverviewAnalysisInput input;
    input.has_file = true;
    input.has_result = true;
    input.report = &report;

    const OverviewAnalysisSummary summary = BuildOverviewAnalysisSummary(input);

    ASSERT_EQ(summary.top_issues.size(), static_cast<int>(kOverviewTopIssueCount));
    EXPECT_EQ(summary.top_issues[0].title, "c0");
    EXPECT_EQ(summary.top_issues[1].title, "c1");
    EXPECT_EQ(summary.top_issues[2].title, "e0");
    EXPECT_EQ(summary.top_issues[3].title, "i0");
    EXPECT_EQ(summary.top_issues[4].title, "i1");
}

// 摘要要跨事件存活（首页不会跟着每次扫描重建），所以必须是值拷贝。
TEST(OverviewAnalysisTests, SummaryTopIssuesAreValueCopies) {
    QcReport report;
    report.issues.push_back(MakeIssue(IssueSeverity::Critical, "c0"));

    OverviewAnalysisInput input;
    input.has_file = true;
    input.has_result = true;
    input.report = &report;

    OverviewAnalysisSummary summary = BuildOverviewAnalysisSummary(input);
    ASSERT_EQ(summary.top_issues.size(), 1);
    summary.top_issues[0].title = "改过了";

    EXPECT_EQ(report.issues[0].title, "c0"); // 源报告不受影响
}

TEST(OverviewAnalysisTests, SummaryWithoutResultLeavesCountersAtZero) {
    OverviewAnalysisInput input;
    input.has_file = true;
    input.has_result = false;
    input.report = nullptr;

    const OverviewAnalysisSummary summary = BuildOverviewAnalysisSummary(input);

    EXPECT_FALSE(summary.has_result);
    EXPECT_EQ(summary.critical, 0);
    EXPECT_TRUE(summary.top_issues.isEmpty());
}

// has_result=true 却没有报告是自相矛盾的入参，宁可降级成"没结果"也不要解引用空指针。
TEST(OverviewAnalysisTests, SummaryWithNullReportFallsBackToNoResult) {
    OverviewAnalysisInput input;
    input.has_file = true;
    input.has_result = true;
    input.report = nullptr;

    const OverviewAnalysisSummary summary = BuildOverviewAnalysisSummary(input);

    EXPECT_FALSE(summary.has_result);
    EXPECT_TRUE(summary.top_issues.isEmpty());
}

TEST(OverviewAnalysisTests, FailedIsSetOnlyWhenNotScanning) {
    OverviewAnalysisInput input;
    input.has_file = true;
    input.error = QString::fromUtf8("解码失败");
    input.scan_running = false;
    EXPECT_TRUE(BuildOverviewAnalysisSummary(input).failed);

    // 扫描重启后旧错误必须让位：否则首页一边转圈一边报上一次的红字。
    input.scan_running = true;
    const OverviewAnalysisSummary running = BuildOverviewAnalysisSummary(input);
    EXPECT_FALSE(running.failed);
    EXPECT_TRUE(running.scan_running);
    EXPECT_EQ(running.error, QString::fromUtf8("解码失败")); // 文本保留，只是不判失败
}

TEST(OverviewAnalysisTests, EmptyErrorIsNotAFailure) {
    OverviewAnalysisInput input;
    input.has_file = true;
    input.scan_running = false;
    input.error = QString();

    EXPECT_FALSE(BuildOverviewAnalysisSummary(input).failed);
}
