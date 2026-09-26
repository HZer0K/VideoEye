#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/qc/QcAnalyzeRequest.h"
#include "core/model/QcReport.h"
#include "utils/Json.h"
#include "utils/QcReportExporter.h"

using videoeye::qc::QcRunResult;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::utils::QcExportBundle;
using videoeye::utils::QcReportExporter;
using videoeye::utils::QcReportOutputPath;
using videoeye::utils::JsonParse;
using videoeye::utils::JsonValue;

namespace {
DiagnosticIssue MakeIssue(const std::string& rule_id, IssueSeverity sev,
                                 IssueCategory cat, const std::string& title) {
    DiagnosticIssue issue;
    issue.rule_id = rule_id;
    issue.severity = sev;
    issue.category = cat;
    issue.title = title;
    issue.detail = "实测 " + title;
    issue.suggestion = "建议 " + title;
    issue.occurrence_count = 1;
    return issue;
}

QcExportBundle MakeBundle(int issue_count) {
    QcRunResult run;
    run.ok = true;
    run.report.file_name = "sample.mp4";
    run.report.file_path = "/tmp/sample.mp4";
    run.report.container_format = "mov";
    run.report.duration_seconds = 12.5;
    run.report.score = 42.0;
    run.report.verdict = "不通过";
    if (issue_count > 0) run.report.issues.push_back(
        MakeIssue("video.gop.max_seconds", IssueSeverity::Critical,
                  IssueCategory::Gop, "GOP 过长"));
    if (issue_count > 1) run.report.issues.push_back(
        MakeIssue("audio.loudness.range", IssueSeverity::Error,
                  IssueCategory::Audio, "响度超标"));
    if (issue_count > 2) run.report.issues.push_back(
        MakeIssue("container.missing_subtitle", IssueSeverity::Warning,
                  IssueCategory::Container, "缺字幕"));

    QcExportBundle bundle;
    bundle.profile_id = "broadcast";
    bundle.profile_name = "广播级";
    bundle.run = run;
    return bundle;
}

TEST(QcReportExporterTest, CsvHasOneRowPerIssue) {
    const std::string path = std::filesystem::temp_directory_path().string() +
                             "/videoeye_qc_export_csv_test.csv";
    ASSERT_TRUE(QcReportExporter::ExportCsv(path, MakeBundle(3)));

    std::ifstream in(path);
    std::string line;
    int lines = 0;
    std::string content;
    while (std::getline(in, line)) {
        ++lines;
        content += line + "\n";
    }
    // BOM + 表头 + 3 个 issue = 4 行
    EXPECT_EQ(lines, 4);
    EXPECT_NE(content.find("video.gop.max_seconds"), std::string::npos);
    EXPECT_NE(content.find("audio.loudness.range"), std::string::npos);
    EXPECT_NE(content.find("container.missing_subtitle"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(QcReportExporterTest, JsonSchemaIsStable) {
    const std::string path = std::filesystem::temp_directory_path().string() +
                             "/videoeye_qc_export_json_test.json";
    ASSERT_TRUE(QcReportExporter::ExportJson(path, MakeBundle(3)));

    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(ss.str(), root, &err)) << err;
    ASSERT_TRUE(root.IsObject());

    // 顶层 schema 关键字段齐备（实际结构：profile / file / summary / issues / metrics ...）
    EXPECT_TRUE(root.Has("profile"));
    EXPECT_TRUE(root.Has("summary"));
    EXPECT_TRUE(root.Has("issues"));
    const JsonValue* profile_obj = root.Find("profile");
    ASSERT_TRUE(profile_obj != nullptr);
    EXPECT_TRUE(profile_obj->Find("id") != nullptr);
    const JsonValue* summary = root.Find("summary");
    ASSERT_TRUE(summary != nullptr);
    EXPECT_TRUE(summary->Find("score") != nullptr);
    EXPECT_TRUE(summary->Find("verdict") != nullptr);
    const JsonValue* issues = root.Find("issues");
    ASSERT_TRUE(issues != nullptr);
    ASSERT_TRUE(issues->IsArray());
    EXPECT_EQ(issues->Size(), 3u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(QcReportExporterTest, EmptyIssuesStillExportsHeader) {
    const std::string path = std::filesystem::temp_directory_path().string() +
                             "/videoeye_qc_export_empty_test.csv";
    ASSERT_TRUE(QcReportExporter::ExportCsv(path, MakeBundle(0)));
    std::ifstream in(path);
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) ++lines;
    EXPECT_EQ(lines, 1);  // 只有表头
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(QcReportExporterTest, OutputPathUsesExtension) {
    const std::string dir = std::filesystem::temp_directory_path().string();
    auto expected = [&](const std::string& ext) {
        return (std::filesystem::path(dir) / ("clip" + ext)).string();
    };
    EXPECT_EQ(QcReportOutputPath(dir, "clip", videoeye::qc::QcReportFormat::Json), expected(".json"));
    EXPECT_EQ(QcReportOutputPath(dir, "clip", videoeye::qc::QcReportFormat::Csv), expected(".csv"));
    EXPECT_EQ(QcReportOutputPath(dir, "clip", videoeye::qc::QcReportFormat::Html), expected(".html"));
    EXPECT_EQ(QcReportOutputPath(dir, "clip", videoeye::qc::QcReportFormat::Pdf), expected(".pdf"));
}

TEST(QcReportExporterTest, BatchSummaryCsvOneRowPerFile) {
    videoeye::utils::QcBatchSummaryInput input;
    input.root = "/media";
    input.profile_id = "general";
    input.profile_name = "通用";
    for (int i = 0; i < 4; ++i) {
        videoeye::utils::QcBatchSummaryRow row;
        row.path = "/media/file" + std::to_string(i) + ".mp4";
        row.status = "完成";
        row.score = 80.0 + i;
        row.critical_count = i % 2;
        input.rows.push_back(row);
    }
    input.succeeded = 4;

    const std::string path = std::filesystem::temp_directory_path().string() +
                             "/videoeye_qc_summary_test.csv";
    ASSERT_TRUE(QcReportExporter::ExportBatchSummaryCsv(path, input));

    std::ifstream in(path);
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) ++lines;
    EXPECT_EQ(lines, 5);  // 表头 + 4 行

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
}  // namespace
