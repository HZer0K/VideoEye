#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/analyzer/AnalysisTask.h"
#include "core/analyzer/QcRuleEngine.h"
#include "core/qc/QcProfile.h"
#include "core/qc/QcReportFormat.h"
#include "ui/reporting_panel/analysis_task.h"
#include "ui/reporting_panel/report_path.h"

namespace fs = std::filesystem;

using videoeye::analyzer::AnalysisResult;
using videoeye::analyzer::AnalysisStatus;
using videoeye::analyzer::QcRuleEngine;
using videoeye::qc::QcProfile;
using videoeye::qc::QcReportFormat;
using videoeye::qc::QcRunResult;
using videoeye::ui::AnalysisTask;
using videoeye::ui::ApplyExportPaths;
using videoeye::ui::RecycleTask;
using videoeye::ui::ReportBasePath;

namespace {

// 跨平台路径后缀判断（忽略分隔符差异）
bool EndsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---------------------------------------------------------------------------
// P0#2：批量导出路径唯一性 —— 不同子目录中的同名文件必须落到不同路径，
// 且保留相对子目录、去掉扩展名。
// ---------------------------------------------------------------------------
TEST(ReportBasePathTest, SameNameDifferentSubdirAreDistinct) {
    const std::string root = "C:/root";
    const std::string out = "C:/out";
    const auto p1 = ReportBasePath(out, root, "C:/root/sub1/clip.mp4").string();
    const auto p2 = ReportBasePath(out, root, "C:/root/sub2/clip.mp4").string();

    EXPECT_NE(p1, p2);  // 同名文件不得互相覆盖
    EXPECT_EQ(fs::path(p1).filename().string(), "clip");  // 去掉扩展名
    EXPECT_EQ(fs::path(p2).filename().string(), "clip");
    // 相对子目录被保留
    EXPECT_NE(fs::path(p1).parent_path().string().find("sub1"), std::string::npos);
    EXPECT_NE(fs::path(p2).parent_path().string().find("sub2"), std::string::npos);
}

TEST(ReportBasePathTest, DeepRelativeSubdirPreserved) {
    const auto p = ReportBasePath("C:/out", "C:/root", "C:/root/a/b/clip.mp4").string();
    EXPECT_EQ(fs::path(p).filename().string(), "clip");
    EXPECT_NE(fs::path(p).parent_path().string().find("a"), std::string::npos);
    EXPECT_NE(fs::path(p).parent_path().string().find("b"), std::string::npos);
}

TEST(ReportBasePathTest, NotUnderRootFallsBackToFileName) {
    // file 不在 root 之下：应退回只取文件名（不带扩展名），且不抛异常
    const auto p = ReportBasePath("C:/out", "C:/other", "C:/root/clip.mp4").string();
    EXPECT_EQ(fs::path(p).filename().string(), "clip");
    EXPECT_TRUE(fs::path(p).has_parent_path());
}

// ---------------------------------------------------------------------------
// P0#2：导出落盘 —— 成功时返回真实路径、文件确实生成；目标目录不可写时
// 标记 export_failed=true（让上层区分"分析成功 / 导出失败"）。
// ---------------------------------------------------------------------------
TEST(ApplyExportPathsTest, WritableDirWritesAllFormats) {
    const auto tmp = fs::temp_directory_path() / "ve_rp_writable_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const auto root = tmp / "root";
    fs::create_directories(root / "a", ec);
    const std::string file = (root / "a" / "b.mp4").string();

    QcRunResult result;
    result.ok = true;
    result.report.file_path = file;
    result.report.file_name = "b.mp4";
    result.report.container_format = "mp4";
    result.report.duration_seconds = 1.0;

    QcProfile profile;
    const std::vector<QcReportFormat> formats{QcReportFormat::Json, QcReportFormat::Csv,
                                              QcReportFormat::Html};

    ApplyExportPaths(result, tmp.string(), root.string(), file, formats, profile);

    EXPECT_FALSE(result.export_failed);
    // 主格式取 formats 的首项（Json）
    EXPECT_TRUE(EndsWith(result.output_path, ".json"));
    EXPECT_TRUE(fs::exists(result.output_path));
    EXPECT_TRUE(fs::exists(fs::path(result.output_path).replace_extension(".csv")));
    EXPECT_TRUE(fs::exists(fs::path(result.output_path).replace_extension(".html")));

    fs::remove_all(tmp, ec);
}

TEST(ApplyExportPathsTest, EmptyOutDirIsNoOp) {
    QcRunResult result;
    result.ok = true;
    QcProfile profile;
    ApplyExportPaths(result, "", "C:/root", "C:/root/a.mp4",
                     {QcReportFormat::Json}, profile);
    EXPECT_FALSE(result.export_failed);
    EXPECT_TRUE(result.output_path.empty());
}

TEST(ApplyExportPathsTest, FailedAnalysisIsNoOp) {
    QcRunResult result;
    result.ok = false;
    QcProfile profile;
    ApplyExportPaths(result, fs::temp_directory_path().string(), "C:/root", "C:/root/a.mp4",
                     {QcReportFormat::Json}, profile);
    EXPECT_FALSE(result.export_failed);
    EXPECT_TRUE(result.output_path.empty());
}

TEST(ApplyExportPathsTest, UnwritableDirMarksExportFailed) {
    const auto tmp = fs::temp_directory_path() / "ve_rp_unwritable_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // blocker 是一个普通文件：以它为 out_dir 时 create_directories 必然失败，
    // 后续写文件也会失败 —— 模拟"目标目录不可写"。
    const auto blocker = tmp / "blocker";
    { std::ofstream(blocker); }

    const auto root = tmp / "root";
    fs::create_directories(root / "a", ec);
    const std::string file = (root / "a" / "b.mp4").string();

    QcRunResult result;
    result.ok = true;
    result.report.file_path = file;
    result.report.file_name = "b.mp4";
    result.report.container_format = "mp4";

    QcProfile profile;
    ApplyExportPaths(result, blocker.string(), root.string(), file,
                     {QcReportFormat::Json}, profile);

    EXPECT_TRUE(result.export_failed);                 // 导出失败被标记
    EXPECT_TRUE(EndsWith(result.output_path, ".json")); // 路径仍填回（UI 可展示）
    EXPECT_FALSE(fs::exists(result.output_path));       // 实际没落盘

    fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// P0#1：线程回收 —— 对仍 joinable 的 std::thread 直接赋值会 terminate；
// RecycleTask 先 join 再 reset，连续分析两次不会崩。
// ---------------------------------------------------------------------------
TEST(AnalysisTaskTest, RecycleJoinsJoinableAndResets) {
    auto task = std::make_shared<AnalysisTask>();
    task->thread = std::thread([] { /* 立即返回 */ });
    RecycleTask(task);
    EXPECT_FALSE(task);  // 已 reset
}

TEST(AnalysisTaskTest, RestartAfterRecycleDoesNotTerminate) {
    std::shared_ptr<AnalysisTask> task = std::make_shared<AnalysisTask>();
    task->thread = std::thread([] {});
    RecycleTask(task);  // 第一次"运行"结束并回收

    // 第二次运行：若第一次仍 joinable，下面这行（对 std::thread 赋值）会 terminate
    task = std::make_shared<AnalysisTask>();
    task->thread = std::thread([] {});
    RecycleTask(task);
    EXPECT_FALSE(task);
}

TEST(AnalysisTaskTest, RecycleNullAndUnstartedSafe) {
    std::shared_ptr<AnalysisTask> nulltask;
    EXPECT_NO_THROW(RecycleTask(nulltask));

    auto empty = std::make_shared<AnalysisTask>();  // 默认构造，未启动线程
    RecycleTask(empty);
    EXPECT_FALSE(empty);
}

// ---------------------------------------------------------------------------
// P1#4：扫描状态映射 —— 完整 / 抽样 / 失败 / 取消 必须被正确翻译到报告标记，
// 抽样不得给出"通过"结论，失败 / 取消不得视为完整。
// ---------------------------------------------------------------------------
TEST(QcScanStatusTest, CompleteIsFull) {
    AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = AnalysisStatus::Complete;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_TRUE(report.completed);
    EXPECT_FALSE(report.partial);
}

TEST(QcScanStatusTest, SampledIsPartialAndNotPass) {
    AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = AnalysisStatus::Sampled;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_TRUE(report.completed);  // 抽样是"跑完"的部分结果，可结算
    EXPECT_TRUE(report.partial);    // 但标记为部分
    EXPECT_EQ(report.verdict, "抽样完成·仅供参考");  // 禁止给出"通过"
}

TEST(QcScanStatusTest, FailedIsNotCompleted) {
    AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = AnalysisStatus::Failed;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_FALSE(report.completed);
    EXPECT_FALSE(report.partial);
}

TEST(QcScanStatusTest, CancelledIsNotCompleted) {
    AnalysisResult r;
    r.file_path = "x.mp4";
    r.file_extension = "mp4";
    r.container_format = "mp4";
    r.scan_status = AnalysisStatus::Cancelled;

    const auto report = QcRuleEngine().Evaluate(r);
    EXPECT_FALSE(report.completed);
    EXPECT_FALSE(report.partial);
}

}  // namespace
