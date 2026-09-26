#include <gtest/gtest.h>

#include <string>

#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcComparator.h"

using videoeye::qc::CompareRuns;
using videoeye::qc::QcCompareRow;
using videoeye::qc::QcRunResult;
using videoeye::model::IssueSeverity;

namespace {
TEST(QcComparatorTest, ProducesRowsAndDetectsFileNameDiff) {
    QcRunResult left;
    left.ok = true;
    left.report.file_name = "source.mp4";
    left.report.duration_seconds = 10.0;
    left.report.score = 95.0;
    left.report.verdict = "通过";

    QcRunResult right;
    right.ok = true;
    right.report.file_name = "transcoded.mp4";  // 文件名必然不同
    right.report.duration_seconds = 10.0;
    right.report.score = 95.0;
    right.report.verdict = "通过";

    const auto cmp = CompareRuns(left, right);
    // 即便 analysis 是空的，至少应产出文件名/时长/评分等行
    ASSERT_FALSE(cmp.rows.empty());

    bool saw_diff = false;
    for (const QcCompareRow& row : cmp.rows) {
        if (row.diff == videoeye::qc::QcFieldDiff::Different) saw_diff = true;
    }
    EXPECT_TRUE(saw_diff) << "文件名不同应当至少产生一行 Different";
}

TEST(QcComparatorTest, SameFilesYieldNoDifferences) {
    QcRunResult left;
    left.ok = true;
    left.report.file_name = "clip.mp4";
    left.report.duration_seconds = 12.345;
    left.report.score = 80.0;
    left.report.verdict = "警告";
    left.report.container_format = "mov";

    QcRunResult right = left;  // 完全相等
    const auto cmp = CompareRuns(left, right);
    EXPECT_EQ(cmp.DifferentCount(), 0);
}

TEST(QcComparatorTest, ScoreAndDurationAreNumericRows) {
    QcRunResult left;
    left.report.score = 90.0;
    left.report.duration_seconds = 10.0;
    QcRunResult right;
    right.report.score = 70.0;  // 评分变化
    right.report.duration_seconds = 10.0;

    const auto cmp = CompareRuns(left, right);
    bool saw_numeric_diff = false;
    for (const QcCompareRow& row : cmp.rows) {
        if (row.numeric && row.diff == videoeye::qc::QcFieldDiff::Different) {
            saw_numeric_diff = true;
        }
    }
    EXPECT_TRUE(saw_numeric_diff);
}
}  // namespace
