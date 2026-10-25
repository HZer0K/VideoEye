// 报告导出口径与 ReportExportController 的行为回归。
//
// 覆盖三件以前"错了不崩、只会悄悄给错答案"的事：
//   1. 单文件报告的文件名主干取哪个路径（报告里记的 vs 当前文件）；
//   2. 批量汇总导出前那一份 summary 的重算口径（ExportFailed 算失败、completed 的判据）；
//   3. 「无结果时点导出」这条延迟路径的两个不变量：取走即作废、失败即丢弃。
//      漏掉任何一个，用户都会在毫无察觉的情况下拿到一份不相干 / 空的报告。
//
// 控制器本身也在这里驱动：依赖全是 lambda（见 ReportExportController::Deps），
// 所以不需要造 QWidget，也不弹真对话框。

#include <string>
#include <utility>
#include <vector>

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "core/qc/BatchQcRunner.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"

#include "ui/reporting_panel/ReportExportController.h"
#include "ui/reporting_panel/report_export.h"

namespace {

using videoeye::qc::BatchItemStatus;
using videoeye::qc::BatchQcItemResult;
using videoeye::qc::BatchQcSummary;
using videoeye::qc::QcProfile;
using videoeye::qc::QcReportFormat;
using videoeye::qc::QcRunResult;
using videoeye::ui::DeferredExportTicket;
using videoeye::ui::RecomputeBatchSummary;
using videoeye::ui::ReportExportController;
using videoeye::ui::SingleExportBaseName;

// ---------------------------------------------------------------------------
// 纯口径：文件名主干
// ---------------------------------------------------------------------------

TEST(ReportExportTests, BaseNamePrefersReportPath) {
    // 报告里记的文件优先：批量 / 历史结果下它与"当前文件"可能不是同一个，
    // 而报告本体认的是 report.file_path。
    EXPECT_EQ(SingleExportBaseName("/a/b/report.mp4", "/c/d/current.mkv"), "report");
}

TEST(ReportExportTests, BaseNameFallsBackToCurrentPath) {
    // 没跑出结果（report.file_path 为空）才退回当前文件。
    EXPECT_EQ(SingleExportBaseName("", "/c/d/current.mkv"), "current");
}

TEST(ReportExportTests, BaseNameStripsDirectoryAndExtension) {
    EXPECT_EQ(SingleExportBaseName("plain", ""), "plain");
    // 路径里的点不算扩展名：只剥最后一个扩展名
    EXPECT_EQ(SingleExportBaseName("/x/y/my.movie.final.mov", ""), "my.movie.final");
}

TEST(ReportExportTests, BaseNameOfEmptyInputIsEmpty) {
    EXPECT_TRUE(SingleExportBaseName("", "").empty());
}

// ---------------------------------------------------------------------------
// 纯口径：汇总重算
// ---------------------------------------------------------------------------

BatchQcItemResult MakeItem(BatchItemStatus status, int critical, int error_count, int warning, int info) {
    BatchQcItemResult item;
    item.status = status;
    item.critical_count = critical;
    item.error_count = error_count;
    item.warning_count = warning;
    item.info_count = info;
    return item;
}

TEST(ReportExportTests, SummaryCountsEveryStatusBucket) {
    const std::vector<BatchQcItemResult> items = {
        MakeItem(BatchItemStatus::Succeeded, 1, 0, 0, 0), MakeItem(BatchItemStatus::Failed, 0, 2, 0, 0),
        MakeItem(BatchItemStatus::TimedOut, 0, 0, 3, 0),  MakeItem(BatchItemStatus::Cancelled, 0, 0, 0, 4),
        MakeItem(BatchItemStatus::Skipped, 0, 0, 0, 0),
    };
    const BatchQcSummary s = RecomputeBatchSummary(items);
    EXPECT_EQ(s.total, 5);
    EXPECT_EQ(s.succeeded, 1);
    EXPECT_EQ(s.failed, 1);
    EXPECT_EQ(s.timed_out, 1);
    EXPECT_EQ(s.cancelled, 1);
    EXPECT_EQ(s.skipped, 1);
    EXPECT_EQ(s.critical_count, 1);
    EXPECT_EQ(s.error_count, 2);
    EXPECT_EQ(s.warning_count, 3);
    EXPECT_EQ(s.info_count, 4);
}

TEST(ReportExportTests, SummaryTreatsExportFailedAsFailure) {
    // "分析成功但报告落盘失败"对汇总来说就是没成功 —— 与表格终态口径保持一致，
    // 否则汇总会说"全部通过"，而实际一个文件都没落盘。
    const std::vector<BatchQcItemResult> items = {
        MakeItem(BatchItemStatus::Succeeded, 0, 0, 0, 0),
        MakeItem(BatchItemStatus::ExportFailed, 0, 0, 0, 0),
    };
    const BatchQcSummary s = RecomputeBatchSummary(items);
    EXPECT_EQ(s.succeeded, 1);
    EXPECT_EQ(s.failed, 1);
}

TEST(ReportExportTests, SummaryCompletedIsFalseWhenAnythingCancelled) {
    // 取消过就意味着列表里有没跑到的数据，这份汇总不能自称完整。
    const std::vector<BatchQcItemResult> cancelled = {
        MakeItem(BatchItemStatus::Succeeded, 0, 0, 0, 0),
        MakeItem(BatchItemStatus::Cancelled, 0, 0, 0, 0),
    };
    EXPECT_FALSE(RecomputeBatchSummary(cancelled).completed);

    const std::vector<BatchQcItemResult> clean = {
        MakeItem(BatchItemStatus::Succeeded, 0, 0, 0, 0),
        MakeItem(BatchItemStatus::Failed, 0, 0, 0, 0),
    };
    EXPECT_TRUE(RecomputeBatchSummary(clean).completed);
}

TEST(ReportExportTests, SummaryOfEmptyListIsBlankButComplete) {
    const BatchQcSummary s = RecomputeBatchSummary({});
    EXPECT_EQ(s.total, 0);
    EXPECT_TRUE(s.completed); // 一个都没取消 —— 空列表谈不上不完整
}

TEST(ReportExportTests, SummaryIgnoresNonTerminalStatuses) {
    // 未结算的条目既不算成功也不算失败：算进任何一档都会让"完成 + 失败 + ..."
    // 对不上 total，于是汇总文本自己跟自己打架。
    const std::vector<BatchQcItemResult> items = {
        MakeItem(BatchItemStatus::Pending, 0, 0, 0, 0),
        MakeItem(BatchItemStatus::Running, 0, 0, 0, 0),
    };
    const BatchQcSummary s = RecomputeBatchSummary(items);
    EXPECT_EQ(s.total, 2);
    EXPECT_EQ(s.succeeded + s.failed + s.timed_out + s.cancelled + s.skipped, 0);
}

// ---------------------------------------------------------------------------
// 纯口径：挂起票据
// ---------------------------------------------------------------------------

TEST(ReportExportTests, TicketIsUnarmedUntilDirectoryPicked) {
    DeferredExportTicket ticket;
    EXPECT_FALSE(ticket.armed());
    EXPECT_TRUE(ticket.Take().isEmpty()); // 没登记就取不到东西

    ticket.Arm(QString()); // 用户取消了目录对话框 —— 空目录等于不登记
    EXPECT_FALSE(ticket.armed());
}

TEST(ReportExportTests, TicketTakeConsumesIt) {
    DeferredExportTicket ticket;
    ticket.Arm(QString::fromUtf8("/tmp/out"));
    ASSERT_TRUE(ticket.armed());
    EXPECT_EQ(ticket.Take(), QString::fromUtf8("/tmp/out"));
    // 取走即用掉：一次分析只对应一次导出，完成回调跑两遍也不会导出两遍
    EXPECT_FALSE(ticket.armed());
    EXPECT_TRUE(ticket.Take().isEmpty());
}

TEST(ReportExportTests, TicketClearDiscardsIt) {
    DeferredExportTicket ticket;
    ticket.Arm(QString::fromUtf8("/tmp/out"));
    ticket.Clear();
    EXPECT_FALSE(ticket.armed());
}

// ---------------------------------------------------------------------------
// 控制器
// ---------------------------------------------------------------------------

// 注入用的假环境：记录每个被调到的口子，不弹任何真对话框。
struct ExportHarness {
    bool has_file = true;
    bool busy = false;
    bool fresh = true;
    std::string current_path = "C:/media/current.mkv";
    QcRunResult result;
    QcProfile profile;
    std::vector<QcReportFormat> formats = {QcReportFormat::Json};
    std::vector<BatchQcItemResult> batch_results;
    std::string batch_root = "C:/media";

    int analysis_starts = 0;
    int directory_picks = 0;
    int summary_picks = 0;
    QString next_directory;
    QString next_summary_file;
    std::vector<std::pair<QString, QString>> notices;
    QString last_status;

    ReportExportController* controller = nullptr;

    ReportExportController::Deps MakeDeps() {
        ReportExportController::Deps deps;
        deps.has_file = [this]() { return has_file; };
        deps.busy = [this]() { return busy; };
        deps.has_fresh_result = [this]() { return fresh; };
        deps.current_path = [this]() { return current_path; };
        deps.single_result = [this]() { return result; };
        deps.profile = [this]() { return profile; };
        deps.formats = [this]() { return formats; };
        deps.batch_results = [this]() { return batch_results; };
        deps.batch_root = [this]() { return batch_root; };
        deps.start_single_analysis = [this]() { analysis_starts += 1; };
        deps.pick_output_directory = [this]() {
            directory_picks += 1;
            return next_directory;
        };
        deps.pick_summary_file = [this]() {
            summary_picks += 1;
            return next_summary_file;
        };
        deps.notice = [this](const QString& title, const QString& body) { notices.emplace_back(title, body); };
        return deps;
    }

    void Reset(ReportExportController* c) {
        controller = c;
        QObject::connect(c, &ReportExportController::StatusMessage,
                         [this](const QString& text) { last_status = text; });
    }
};

class ReportExportControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.isValid());
        harness_.next_directory = dir_.path();
        harness_.profile.id = "builtin";
        harness_.profile.name = "通用";
        harness_.result.ok = true;
        harness_.result.report.file_path = "C:/media/report.mp4";
        controller_ = new ReportExportController(harness_.MakeDeps());
        harness_.Reset(controller_);
    }
    void TearDown() override {
        delete controller_;
    }

    // 单文件报告落盘后的预期路径（base 取自报告里记的文件名）
    QString ExpectedReport() const {
        return dir_.filePath("report.json");
    }

    QTemporaryDir dir_;
    ExportHarness harness_;
    ReportExportController* controller_ = nullptr;
};

TEST_F(ReportExportControllerTest, NoFileNoticesAndDoesNothing) {
    harness_.has_file = false;
    controller_->ExportSingleReport();
    EXPECT_EQ(harness_.directory_picks, 0);
    EXPECT_EQ(harness_.analysis_starts, 0);
    ASSERT_EQ(harness_.notices.size(), 1u);
    EXPECT_EQ(harness_.notices[0].first, QString::fromUtf8("未选择文件"));
}

TEST_F(ReportExportControllerTest, BusyNoticesAndDoesNothing) {
    harness_.busy = true;
    controller_->ExportSingleReport();
    EXPECT_EQ(harness_.directory_picks, 0);
    EXPECT_EQ(harness_.analysis_starts, 0);
    ASSERT_EQ(harness_.notices.size(), 1u);
    EXPECT_EQ(harness_.notices[0].first, QString::fromUtf8("任务进行中"));
}

TEST_F(ReportExportControllerTest, CancellingDirectoryPickerAborts) {
    harness_.next_directory = QString(); // 用户关掉了对话框
    controller_->ExportSingleReport();
    EXPECT_EQ(harness_.directory_picks, 1);
    EXPECT_EQ(harness_.analysis_starts, 0);
    EXPECT_FALSE(controller_->HasPendingExport());
    EXPECT_TRUE(harness_.last_status.isEmpty());
}

TEST_F(ReportExportControllerTest, FreshResultExportsImmediately) {
    controller_->ExportSingleReport();
    EXPECT_EQ(harness_.directory_picks, 1);
    EXPECT_EQ(harness_.analysis_starts, 0); // 结果新鲜，不需要先分析
    EXPECT_FALSE(controller_->HasPendingExport());
    EXPECT_TRUE(QFileInfo::exists(ExpectedReport()));
    EXPECT_TRUE(harness_.last_status.contains(QString::fromUtf8("报告已导出到")));
}

TEST_F(ReportExportControllerTest, StaleResultDefersExportUntilAnalysisSucceeds) {
    harness_.fresh = false;
    controller_->ExportSingleReport();
    // 先取目录挂起，再启动分析 —— 用户点一次就能拿到当前文件 + 当前模板的报告
    EXPECT_EQ(harness_.directory_picks, 1);
    EXPECT_EQ(harness_.analysis_starts, 1);
    EXPECT_TRUE(controller_->HasPendingExport());
    EXPECT_FALSE(QFileInfo::exists(ExpectedReport()));

    controller_->OnSingleAnalysisFinished(true);
    EXPECT_TRUE(QFileInfo::exists(ExpectedReport()));
    EXPECT_FALSE(controller_->HasPendingExport());
}

TEST_F(ReportExportControllerTest, DeferredTicketIsConsumedExactlyOnce) {
    harness_.fresh = false;
    controller_->ExportSingleReport();
    controller_->OnSingleAnalysisFinished(true);
    ASSERT_TRUE(QFileInfo::exists(ExpectedReport()));

    // 完成回调万一跑两遍：票据已取走，不该再导出一份
    QFile::remove(ExpectedReport());
    controller_->OnSingleAnalysisFinished(true);
    EXPECT_FALSE(QFileInfo::exists(ExpectedReport()));
}

TEST_F(ReportExportControllerTest, FailedAnalysisDiscardsDeferredDirectory) {
    harness_.fresh = false;
    controller_->ExportSingleReport();
    ASSERT_TRUE(controller_->HasPendingExport());

    controller_->OnSingleAnalysisFinished(false);
    EXPECT_FALSE(QFileInfo::exists(ExpectedReport())); // 失败的空报告不许落盘
    EXPECT_FALSE(controller_->HasPendingExport());     // 也不许留到下一次成功

    // 下一次成功也不该导出（票据已经被丢弃）
    controller_->OnSingleAnalysisFinished(true);
    EXPECT_FALSE(QFileInfo::exists(ExpectedReport()));
}

TEST_F(ReportExportControllerTest, AbortPendingExportDiscardsDirectory) {
    harness_.fresh = false;
    controller_->ExportSingleReport();
    ASSERT_TRUE(controller_->HasPendingExport());

    controller_->AbortPendingExport(); // 任务线程抛异常 / 被取消时的收口
    EXPECT_FALSE(controller_->HasPendingExport());
    controller_->OnSingleAnalysisFinished(true);
    EXPECT_FALSE(QFileInfo::exists(ExpectedReport()));
}

TEST_F(ReportExportControllerTest, AbortPendingExportIsIdempotent) {
    controller_->AbortPendingExport(); // 没有挂起时不该有任何副作用
    EXPECT_FALSE(controller_->HasPendingExport());
    EXPECT_TRUE(harness_.last_status.isEmpty());
}

TEST_F(ReportExportControllerTest, BatchSummaryNeedsResults) {
    controller_->ExportBatchSummary();
    EXPECT_EQ(harness_.summary_picks, 0);
    ASSERT_EQ(harness_.notices.size(), 1u);
    EXPECT_EQ(harness_.notices[0].first, QString::fromUtf8("没有结果"));
}

TEST_F(ReportExportControllerTest, BatchSummaryExportsToPickedPath) {
    harness_.batch_results = {
        MakeItem(BatchItemStatus::Succeeded, 1, 0, 0, 0),
        MakeItem(BatchItemStatus::ExportFailed, 0, 0, 0, 0),
    };
    const QString target = dir_.filePath("qc-summary.csv");
    harness_.next_summary_file = target;

    controller_->ExportBatchSummary();
    EXPECT_EQ(harness_.summary_picks, 1);
    EXPECT_TRUE(QFileInfo::exists(target));
    EXPECT_TRUE(harness_.last_status.contains(QString::fromUtf8("汇总已导出到")));
}

TEST_F(ReportExportControllerTest, BatchSummaryCancelledPickerAborts) {
    harness_.batch_results = {MakeItem(BatchItemStatus::Succeeded, 0, 0, 0, 0)};
    harness_.next_summary_file = QString();
    controller_->ExportBatchSummary();
    EXPECT_EQ(harness_.summary_picks, 1);
    EXPECT_TRUE(harness_.last_status.isEmpty());
}

} // namespace
