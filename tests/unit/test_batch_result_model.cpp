// 批量 QC 结果展示口径的回归（ui/reporting_panel/batch_result_model.h）。
//
// 守的是三条"错了不崩、只会悄悄给错答案"的口径：
//   1. 整批终态 —— 超时必须按"没干净跑完"算，取消优先于失败；
//   2. 汇总行 —— 十一个计数一个都不能漏、不能串位；
//   3. 表格行取值 —— 数值列必须以数值类型交给 Qt::DisplayRole，
//      先格式化成字符串再存会让排序变成字典序（"10" 排在 "9" 前面）。

#include "ui/reporting_panel/batch_result_model.h"

#include <string>

#include <QString>
#include <QVariant>

#include <gtest/gtest.h>

namespace {

using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueSeverity;
using videoeye::qc::BatchItemStatus;
using videoeye::qc::BatchQcItemResult;
using videoeye::qc::BatchQcSummary;
using videoeye::task::TaskState;
using videoeye::ui::BatchDetailText;
using videoeye::ui::BatchItemStatusText;
using videoeye::ui::BatchRowCells;
using videoeye::ui::BatchSummaryText;
using videoeye::ui::BatchTerminalState;
using videoeye::ui::MakeBatchRowCells;

BatchQcItemResult MakeItem(BatchItemStatus status, const std::string& path) {
    BatchQcItemResult item;
    item.path = path;
    item.status = status;
    return item;
}

} // namespace

// ---- 终态判定 ----

TEST(BatchResultModelTests, CleanRunIsSucceeded) {
    BatchQcSummary summary;
    summary.total = 3;
    summary.succeeded = 3;
    EXPECT_EQ(BatchTerminalState(summary, false), TaskState::Succeeded);
}

// 超时不算成功：那条目的后台线程被放弃、结果不完整。
TEST(BatchResultModelTests, TimedOutItemsMakeTheBatchFail) {
    BatchQcSummary summary;
    summary.succeeded = 2;
    summary.timed_out = 1;
    EXPECT_EQ(BatchTerminalState(summary, false), TaskState::Failed);
}

TEST(BatchResultModelTests, FailedItemsMakeTheBatchFail) {
    BatchQcSummary summary;
    summary.succeeded = 2;
    summary.failed = 1;
    EXPECT_EQ(BatchTerminalState(summary, false), TaskState::Failed);
}

// 用户点了取消，结论就该是 Canceled —— 哪怕同时有条目失败、有条目超时。
// 与其它终态判定的优先级保持一致（取消 > 异常 > 成功）。
TEST(BatchResultModelTests, CancellationWinsOverFailures) {
    BatchQcSummary summary;
    summary.succeeded = 1;
    summary.failed = 2;
    summary.timed_out = 3;
    EXPECT_EQ(BatchTerminalState(summary, true), TaskState::Canceled);
}

// ---- 汇总文本 ----

TEST(BatchResultModelTests, SummaryTextCarriesEveryCounter) {
    BatchQcSummary summary;
    summary.succeeded = 11;
    summary.failed = 22;
    summary.timed_out = 33;
    summary.cancelled = 44;
    summary.skipped = 55;
    summary.critical_count = 66;
    summary.error_count = 77;
    summary.warning_count = 88;
    summary.info_count = 99;
    summary.elapsed_ms = 1234.0;

    const QString text = BatchSummaryText(summary, 100);

    // 每个计数都要出现在文本里，且是"标签 + 数字"的形式（串位会立刻暴露）
    EXPECT_TRUE(text.contains(QStringLiteral("共 100 个")));
    EXPECT_TRUE(text.contains(QStringLiteral("完成 11")));
    EXPECT_TRUE(text.contains(QStringLiteral("失败 22")));
    EXPECT_TRUE(text.contains(QStringLiteral("超时 33")));
    EXPECT_TRUE(text.contains(QStringLiteral("取消 44")));
    EXPECT_TRUE(text.contains(QStringLiteral("跳过 55")));
    EXPECT_TRUE(text.contains(QStringLiteral("致命 66")));
    EXPECT_TRUE(text.contains(QStringLiteral("错误 77")));
    EXPECT_TRUE(text.contains(QStringLiteral("警告 88")));
    EXPECT_TRUE(text.contains(QStringLiteral("提示 99")));
    EXPECT_TRUE(text.contains(QStringLiteral("耗时 1234 ms")));
}

// "共 N 个"用发现到的条目数，不是 summary.total —— 被跳过 / 未跑到的也占一行。
TEST(BatchResultModelTests, SummaryTotalComesFromDiscoveredItems) {
    BatchQcSummary summary;
    summary.total = 1; // 故意与实参不一致
    EXPECT_TRUE(BatchSummaryText(summary, 42).contains(QStringLiteral("共 42 个")));
}

// ---- 详情文本 ----

TEST(BatchResultModelTests, DetailTextStartsWithFileAndStatus) {
    BatchQcItemResult item = MakeItem(BatchItemStatus::Succeeded, "/tmp/clip.mp4");
    item.score = 88.25;
    item.verdict = "警告";
    item.critical_count = 1;

    const QString text = BatchDetailText(item);

    EXPECT_TRUE(text.contains(QString::fromUtf8("/tmp/clip.mp4")));
    EXPECT_TRUE(text.contains(QString::fromUtf8("88.3"))); // 评分一位小数
    EXPECT_TRUE(text.contains(QString::fromUtf8("警告")));
    EXPECT_TRUE(text.contains(QString::fromUtf8("致命 1")));
}

// 分析没问题、报告落盘失败 —— 处置方式与 Failed 完全不同，详情里必须点明。
TEST(BatchResultModelTests, DetailTextCallsOutExportFailure) {
    const BatchQcItemResult ok = MakeItem(BatchItemStatus::Succeeded, "/tmp/a.mp4");
    const BatchQcItemResult export_failed = MakeItem(BatchItemStatus::ExportFailed, "/tmp/a.mp4");

    EXPECT_FALSE(BatchDetailText(ok).contains(QString::fromUtf8("落盘失败")));
    EXPECT_TRUE(BatchDetailText(export_failed).contains(QString::fromUtf8("落盘失败")));
}

TEST(BatchResultModelTests, DetailTextShowsErrorReasonWhenPresent) {
    BatchQcItemResult item = MakeItem(BatchItemStatus::Failed, "/tmp/broken.mp4");
    item.error = "moov atom not found";
    EXPECT_TRUE(BatchDetailText(item).contains(QString::fromUtf8("moov atom not found")));

    item.error.clear();
    EXPECT_FALSE(BatchDetailText(item).contains(QString::fromUtf8("原因")));
}

TEST(BatchResultModelTests, DetailTextShowsOutputPathWhenPresent) {
    BatchQcItemResult item = MakeItem(BatchItemStatus::Succeeded, "/tmp/a.mp4");
    item.output_path = "/out/a.json";
    EXPECT_TRUE(BatchDetailText(item).contains(QString::fromUtf8("/out/a.json")));
}

// 报告本体默认不驻留内存（keep_reports=false），issues 为空；
// 万一将来打开保留，详情里要能直接列出问题清单。
TEST(BatchResultModelTests, DetailTextListsIssuesWhenTheReportIsKept) {
    BatchQcItemResult item = MakeItem(BatchItemStatus::Succeeded, "/tmp/a.mp4");
    DiagnosticIssue issue;
    issue.severity = IssueSeverity::Critical;
    issue.title = "关键帧间隔过长";
    issue.detail = "实测 12.0s，阈值 10.0s";
    item.report.issues.push_back(issue);

    const QString text = BatchDetailText(item);
    EXPECT_TRUE(text.contains(QString::fromUtf8("问题清单")));
    EXPECT_TRUE(text.contains(QString::fromUtf8("关键帧间隔过长")));
    EXPECT_TRUE(text.contains(QString::fromUtf8("实测 12.0s")));
}

// ---- 表格行取值 ----

TEST(BatchResultModelTests, RowCellsCopyEveryColumn) {
    BatchQcItemResult item = MakeItem(BatchItemStatus::Failed, "/tmp/x.mkv");
    item.verdict = "不通过";
    item.score = 61.5;
    item.critical_count = 1;
    item.error_count = 2;
    item.warning_count = 3;
    item.info_count = 4;
    item.elapsed_ms = 250.0;
    item.output_path = "/out/x.json";

    const BatchRowCells cells = MakeBatchRowCells(item);

    EXPECT_EQ(cells.path, QString::fromUtf8("/tmp/x.mkv"));
    EXPECT_EQ(cells.verdict, QString::fromUtf8("不通过"));
    EXPECT_EQ(cells.output_path, QString::fromUtf8("/out/x.json"));
    EXPECT_DOUBLE_EQ(cells.score, 61.5);
    EXPECT_DOUBLE_EQ(cells.elapsed_ms, 250.0);
    EXPECT_EQ(cells.critical, 1);
    EXPECT_EQ(cells.error_count, 2);
    EXPECT_EQ(cells.warning, 3);
    EXPECT_EQ(cells.info, 4);
}

// 数值列的类型本身就是口径：铺行时直接塞进 DisplayRole，QVariant 就是数值类型。
// 存成字符串的话 QTableWidgetItem::operator< 会走字典序，"10" 会排在 "9" 前面。
TEST(BatchResultModelTests, NumericCellsProduceNumericVariants) {
    BatchQcItemResult item = MakeItem(BatchItemStatus::Succeeded, "/tmp/a.mp4");
    item.score = 9.0;
    item.critical_count = 10;

    const BatchRowCells cells = MakeBatchRowCells(item);

    const QVariant score_variant = QVariant::fromValue(cells.score);
    const QVariant count_variant = QVariant::fromValue(cells.critical);
    ASSERT_EQ(score_variant.typeId(), QMetaType::Double);
    ASSERT_EQ(count_variant.typeId(), QMetaType::Int);

    // 为什么必须存数值：QTableWidgetItem 的排序比较走 QVariant，存成字符串就退化成
    // 字典序 —— 下面这条就是当年那条 bug 的形态（10 排到了 9 前面）。
    const QVariant as_text_10 = QVariant(QString::number(10));
    const QVariant as_text_9 = QVariant(QString::number(9));
    EXPECT_LT(as_text_10.toString(), as_text_9.toString());
}

TEST(BatchResultModelTests, StatusTextIsNeverEmpty) {
    for (const BatchItemStatus status : {BatchItemStatus::Pending, BatchItemStatus::Running, BatchItemStatus::Succeeded,
                                         BatchItemStatus::Failed, BatchItemStatus::TimedOut, BatchItemStatus::Cancelled,
                                         BatchItemStatus::Skipped, BatchItemStatus::ExportFailed}) {
        EXPECT_FALSE(BatchItemStatusText(status).isEmpty())
            << "状态文本为空会让表格该格显示空白，看不出这条为什么没结果";
    }
}
