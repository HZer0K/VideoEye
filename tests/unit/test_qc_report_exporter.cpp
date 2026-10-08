#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/qc/QcAnalyzeRequest.h"
#include "core/domain/model/QcReport.h"
#include "infrastructure/serialization/Json.h"
#include "core/reporting/QcReportExporter.h"
#include "tests/support/PdfVerifier.h"

using videoeye::qc::QcRunResult;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::reporting::QcExportBundle;
using videoeye::reporting::QcReportExporter;
using videoeye::reporting::QcReportOutputPath;
using videoeye::JsonParse;
using videoeye::JsonValue;

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
    // 3 个以上: 多页 PDF / 大批量用例才需要, 编号进标题便于定位
    for (int i = 3; i < issue_count; ++i) {
        run.report.issues.push_back(
            MakeIssue("video.extra.rule_" + std::to_string(i), IssueSeverity::Info,
                      IssueCategory::Video, "附加问题 " + std::to_string(i)));
    }

    QcExportBundle bundle;
    bundle.profile_id = "broadcast";
    bundle.profile_name = "广播级";
    bundle.run = run;
    return bundle;
}

// ---- 产物矩阵用的脚手架 ----

// 每个用例一份独立目录, 退出即删除; 顺带能断言"目录里只剩该有的东西"。
struct ScopedDir {
    std::filesystem::path path;
    explicit ScopedDir(const std::string& tag)
        : path(std::filesystem::temp_directory_path() /
               ("videoeye_report_" + tag + "_" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }
    ~ScopedDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::string File(const std::string& name) const { return (path / name).string(); }
};

std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// 最小可用的 RFC 4180 解析器。
//
// 为什么不能按 "\n 数行": detail / suggestion 里带换行是常态（比如建议里贴一段
// FFmpeg 命令行），带引号的字段内部换行会把一个记录劈成两个物理行 —— 用 getline
// 数出来的行数会比 issue 数多，而且多多少全看文案里写了几个换行。
std::vector<std::vector<std::string>> ParseCsvRecords(const std::string& content) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool in_quotes = false;
    std::size_t i = 0;
    if (content.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;   // UTF-8 BOM
    for (; i < content.size(); ++i) {
        const char c = content[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < content.size() && content[i + 1] == '"') { field += '"'; ++i; }
                else in_quotes = false;
            } else {
                field += c;   // 引号内的换行/逗号原样保留
            }
            continue;
        }
        if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            row.push_back(field);
            field.clear();
        } else if (c == '\n') {
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
        } else if (c != '\r') {
            field += c;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

// 四种"最容易导出错"的内容各来一份：
//   中文标题 / 中文结论（编码）
//   detail 里带换行（CSV 行数、PDF 单行渲染）
//   以 = 开头的标题（表格软件的公式注入）
//   metric 为 NaN、threshold 为 +Inf（JSON 里不能出现 NaN / Infinity 字面量）
QcExportBundle MakeTrickyBundle() {
    QcRunResult run;
    run.ok = true;
    run.report.file_name = "样本.mp4";
    run.report.file_path = "/tmp/样本.mp4";
    run.report.container_format = "mp4";
    run.report.duration_seconds = 9.75;
    run.report.file_size_bytes = 12345678;
    run.report.score = 61.5;
    run.report.verdict = "有条件通过";
    run.report.generated_at = "2026-10-08 23:00:00";

    DiagnosticIssue gop;
    gop.rule_id = "video.gop.max_seconds";
    gop.severity = IssueSeverity::Critical;
    gop.category = IssueCategory::Gop;
    gop.title = "GOP 过长";
    gop.detail = "实测 5.20 秒，超过 2.00 秒阈值\n建议检查编码器的关键帧间隔";
    gop.suggestion = "把 GOP 缩短到 2 秒以内";
    gop.metric_value = 5.2;
    gop.threshold = 2.0;
    gop.occurrence_count = 2;
    run.report.issues.push_back(gop);

    DiagnosticIssue injected;
    injected.rule_id = "container.injected_title";
    injected.severity = IssueSeverity::Error;
    injected.category = IssueCategory::Container;
    injected.title = "=cmd|'/c calc'!A0";   // 公式注入载荷
    injected.detail = "含 \"引号\" 与,逗号";
    injected.suggestion = "+1+1";
    injected.metric_value = std::nan("");
    injected.threshold = std::numeric_limits<double>::infinity();
    injected.occurrence_count = 1;
    run.report.issues.push_back(injected);

    DiagnosticIssue loudness;
    loudness.rule_id = "audio.loudness.range";
    loudness.severity = IssueSeverity::Warning;
    loudness.category = IssueCategory::Audio;
    loudness.title = "响度超标";
    loudness.detail = "整片响度 -18.5 LUFS";
    loudness.suggestion = "做一次响度归一化";
    loudness.metric_value = -18.5;
    loudness.threshold = -16.0;
    loudness.occurrence_count = 1;
    run.report.issues.push_back(loudness);

    DiagnosticIssue subtitle;
    subtitle.rule_id = "container.missing_subtitle";
    subtitle.severity = IssueSeverity::Info;
    subtitle.category = IssueCategory::Container;
    subtitle.title = "缺少字幕轨";
    subtitle.detail = "未发现任何字幕流";
    subtitle.suggestion = "";
    subtitle.metric_value = 0.0;
    subtitle.threshold = 0.0;
    subtitle.occurrence_count = 1;
    run.report.issues.push_back(subtitle);

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
    videoeye::reporting::QcBatchSummaryInput input;
    input.root = "/media";
    input.profile_id = "general";
    input.profile_name = "通用";
    for (int i = 0; i < 4; ++i) {
        videoeye::reporting::QcBatchSummaryRow row;
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

// ============================================================
// PDF：不再是"测试盲区"（阶段 1）
//
// 以前 PDF 只被断言"文件写出来了"，而一个字节都画不出来的 PDF 一样写得出来。
// 这里用 tests/support/PdfVerifier 校验真正决定"能不能打开"的那几件事：
// xref 偏移、trailer -> Catalog -> Pages -> Kids -> Page -> Contents/Font 的
// 引用链、内容流的 /Length，以及从内容流里还原出的文本（中文有没有变成 ?）。
// ============================================================

TEST(QcReportExporterTest, PdfSinglePageIsStructurallyValid) {
    ScopedDir dir("pdf_single");
    const std::string path = dir.File("report.pdf");
    const auto result = QcReportExporter::ExportPdf(path, MakeBundle(3));
    ASSERT_TRUE(result.ok);

    const auto pdf = videoeye_test::VerifyPdfFile(path);
    ASSERT_TRUE(pdf.ok) << pdf.error;
    EXPECT_EQ(pdf.version.compare(0, 5, "%PDF-"), 0);
    EXPECT_GT(pdf.xref_offset, 0);
    EXPECT_GT(pdf.object_count, 6);
    EXPECT_EQ(pdf.page_count, 1);
    ASSERT_EQ(pdf.page_objects.size(), 1u);
    ASSERT_EQ(pdf.content_objects.size(), 1u);
    EXPECT_GE(pdf.font_objects.size(), 3u);   // F1 / F2 / F3 都得在
    EXPECT_EQ(pdf.stream_length_mismatch, 0);
    EXPECT_GT(pdf.text_runs, 0);
    // 每行画完必须有换行操作符：少了它整页会首尾相接画在同一条基线上，
    // 阅读器里只能看到开头那一小段 —— 而文件本身照样"导出成功"。
    EXPECT_GE(pdf.text_lines, 10) << "文本只排了 " << pdf.text_lines << " 行, 疑似全部叠在一起";
}

TEST(QcReportExporterTest, PdfMultiPageIsStructurallyValid) {
    ScopedDir dir("pdf_multi");
    const std::string path = dir.File("report.pdf");
    // 每页 50 行上限: 固定抬头约 16 行, 每个 issue 4 行 -> 14 个 issue 必然溢出到第二页
    ASSERT_TRUE(QcReportExporter::ExportPdf(path, MakeBundle(14)).ok);

    const auto pdf = videoeye_test::VerifyPdfFile(path);
    ASSERT_TRUE(pdf.ok) << pdf.error;
    ASSERT_GE(pdf.page_count, 2) << "多页报告没有分页";
    // 每一页都必须指向一个真实存在、且 /Length 对得上的内容流
    EXPECT_EQ(pdf.content_objects.size(), static_cast<std::size_t>(pdf.page_count));
    const std::set<int> unique_contents(pdf.content_objects.begin(), pdf.content_objects.end());
    EXPECT_EQ(unique_contents.size(), pdf.content_objects.size()) << "多个页面共用了同一个内容流";
    EXPECT_EQ(pdf.stream_length_mismatch, 0);
    // 分页后每一页都要能还原出文本
    EXPECT_GT(pdf.text_runs, pdf.page_count);
}

TEST(QcReportExporterTest, PdfEmptyReportIsStillValid) {
    ScopedDir dir("pdf_empty");
    const std::string path = dir.File("empty.pdf");
    ASSERT_TRUE(QcReportExporter::ExportPdf(path, MakeBundle(0)).ok);

    const auto pdf = videoeye_test::VerifyPdfFile(path);
    ASSERT_TRUE(pdf.ok) << pdf.error;
    EXPECT_EQ(pdf.page_count, 1);
    EXPECT_NE(pdf.text.find("No issue found."), std::string::npos);
    EXPECT_NE(pdf.text.find("VideoEye QC Report"), std::string::npos);
}

// 中文标题 / 中文 issue / 中文 verdict 一个都不许变成 ?。
TEST(QcReportExporterTest, PdfKeepsChineseTitleIssueAndVerdict) {
    ScopedDir dir("pdf_cjk");
    const std::string path = dir.File("cjk.pdf");
    const auto result = QcReportExporter::ExportPdf(path, MakeTrickyBundle());
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.text_loss) << "中文不该被判成丢字（只有 BMP 外字符才算）";

    const auto pdf = videoeye_test::VerifyPdfFile(path);
    ASSERT_TRUE(pdf.ok) << pdf.error;
    for (const char* needle : {"广播级", "有条件通过", "GOP 过长", "响度超标", "缺少字幕轨"}) {
        EXPECT_NE(pdf.text.find(needle), std::string::npos)
            << "PDF 文本里缺少: " << needle << "\n实际文本: " << pdf.text;
    }
    EXPECT_EQ(pdf.text.find('?'), std::string::npos) << "中文不应被替换成 '?'";
    // 中文段走 Type0/Identity-H 字体（F3），只有 F1 的话根本画不出来
    EXPECT_GE(pdf.font_objects.size(), 3u);
}

// ============================================================
// 产物矩阵：五种格式统一做"导出后重新读取 / 结构验证"（阶段 1）
//
// 格式之间的必测项不同，但"能重新读回来"这条线是共通的：
//   JSON 重新解析、CSV 按 RFC 4180 重新解析、HTML 查关键字段、
//   PDF 查对象关系、TXT 查摘要与 issue 数量。
// ============================================================

enum class ArtifactFormat { Json, Csv, Html, Pdf, Txt };

const char* ArtifactName(ArtifactFormat format) {
    switch (format) {
        case ArtifactFormat::Json: return "json";
        case ArtifactFormat::Csv:  return "csv";
        case ArtifactFormat::Html: return "html";
        case ArtifactFormat::Pdf:  return "pdf";
        case ArtifactFormat::Txt:  return "txt";
    }
    return "unknown";
}

const char* ArtifactExtension(ArtifactFormat format) {
    switch (format) {
        case ArtifactFormat::Json: return "json";
        case ArtifactFormat::Csv:  return "csv";
        case ArtifactFormat::Html: return "html";
        case ArtifactFormat::Pdf:  return "pdf";
        case ArtifactFormat::Txt:  return "txt";
    }
    return "bin";
}

class ReportArtifactMatrix : public testing::TestWithParam<ArtifactFormat> {};

TEST_P(ReportArtifactMatrix, ExportedArtifactCanBeReadBack) {
    const ArtifactFormat format = GetParam();
    ScopedDir dir(std::string("matrix_") + ArtifactName(format));
    const std::string path =
        dir.File(std::string("report.") + ArtifactExtension(format));
    const QcExportBundle bundle = MakeTrickyBundle();
    const int issue_count = static_cast<int>(bundle.run.report.issues.size());
    ASSERT_EQ(issue_count, 4);

    bool ok = false;
    switch (format) {
        case ArtifactFormat::Json: ok = QcReportExporter::ExportJson(path, bundle); break;
        case ArtifactFormat::Csv:  ok = QcReportExporter::ExportCsv(path, bundle);  break;
        case ArtifactFormat::Html: ok = QcReportExporter::ExportHtml(path, bundle); break;
        case ArtifactFormat::Pdf:  ok = QcReportExporter::ExportPdf(path, bundle).ok; break;
        case ArtifactFormat::Txt:  ok = QcReportExporter::ExportText(path, bundle); break;
    }
    ASSERT_TRUE(ok) << "导出失败: " << path;

    // ---- 共通: 产物存在且非空 ----
    ASSERT_TRUE(std::filesystem::exists(path));
    const std::string content = ReadWholeFile(path);
    ASSERT_FALSE(content.empty()) << "产物是空文件: " << path;

    switch (format) {
        case ArtifactFormat::Json: {
            JsonValue root;
            std::string err;
            ASSERT_TRUE(JsonParse(content, root, &err)) << "JSON 无法重新解析: " << err;
            ASSERT_TRUE(root.IsObject());
            for (const char* key : {"schema", "schema_version", "profile", "file", "summary",
                                    "issues", "metrics", "streams", "rules"}) {
                EXPECT_TRUE(root.Has(key)) << "JSON 缺少顶层字段: " << key;
            }
            const JsonValue* issues = root.Find("issues");
            ASSERT_TRUE(issues != nullptr);
            ASSERT_TRUE(issues->IsArray());
            EXPECT_EQ(issues->Size(), static_cast<std::size_t>(issue_count));
            // NaN / Infinity 不是合法 JSON 字面量（有的库会写成 NaN，解析直接崩）
            EXPECT_EQ(content.find("NaN"), std::string::npos);
            EXPECT_EQ(content.find("Infinity"), std::string::npos);
            break;
        }
        case ArtifactFormat::Csv: {
            const auto rows = ParseCsvRecords(content);
            // 表头 + 每个 issue 一行。detail 里本来就带换行，导出端把它压成空格，
            // 于是"物理行数"必须正好等于"记录数" —— 一旦哪天忘了压，行数会虚增，
            // 下游按行读取的脚本就全错位了。
            EXPECT_EQ(std::count(content.begin(), content.end(), '\n'), issue_count + 1);
            ASSERT_EQ(rows.size(), static_cast<std::size_t>(issue_count) + 1);
            for (const auto& row : rows) ASSERT_EQ(row.size(), 12u);
            EXPECT_EQ(rows[1][10].find('\n'), std::string::npos)
                << "CSV 字段里不该残留换行: " << rows[1][10];
            EXPECT_EQ(rows[1][10], "实测 5.20 秒，超过 2.00 秒阈值 建议检查编码器的关键帧间隔");
            // 带引号与逗号的字段必须能被 RFC 4180 解析器原样还原
            EXPECT_EQ(rows[2][10], "含 \"引号\" 与,逗号");
            // 公式注入: 以 = 开头的字段必须被前置单引号中和
            EXPECT_EQ(rows[2][4].compare(0, 2, "'="), 0)
                << "公式注入未被处理: " << rows[2][4];
            // 中文没被破坏
            EXPECT_EQ(rows[1][4], "GOP 过长");
            break;
        }
        case ArtifactFormat::Html: {
            EXPECT_EQ(content.compare(0, 15, "<!DOCTYPE html>"), 0);
            EXPECT_NE(content.find("UTF-8"), std::string::npos);
            EXPECT_NE(content.find("video.gop.max_seconds"), std::string::npos);
            EXPECT_NE(content.find("有条件通过"), std::string::npos);
            EXPECT_NE(content.find("GOP 过长"), std::string::npos);
            break;
        }
        case ArtifactFormat::Pdf: {
            const auto pdf = videoeye_test::VerifyPdfFile(path);
            ASSERT_TRUE(pdf.ok) << pdf.error;
            EXPECT_GE(pdf.page_count, 1);
            EXPECT_EQ(pdf.content_objects.size(), static_cast<std::size_t>(pdf.page_count));
            EXPECT_EQ(pdf.stream_length_mismatch, 0);
            EXPECT_GE(pdf.text_lines, 10);
            EXPECT_NE(pdf.text.find("GOP 过长"), std::string::npos);
            break;
        }
        case ArtifactFormat::Txt: {
            EXPECT_NE(content.find("VideoEye QC 报告"), std::string::npos);
            // 摘要里的四个级别计数各 1
            EXPECT_NE(content.find("致命 1"), std::string::npos);
            EXPECT_NE(content.find("错误 1"), std::string::npos);
            EXPECT_NE(content.find("警告 1"), std::string::npos);
            EXPECT_NE(content.find("提示 1"), std::string::npos);
            // issue 逐条编号
            for (int i = 1; i <= issue_count; ++i) {
                EXPECT_NE(content.find("[" + std::to_string(i) + "]"), std::string::npos);
            }
            EXPECT_NE(content.find("有条件通过"), std::string::npos);
            break;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(AllFormats, ReportArtifactMatrix,
                         testing::Values(ArtifactFormat::Json, ArtifactFormat::Csv,
                                         ArtifactFormat::Html, ArtifactFormat::Pdf,
                                         ArtifactFormat::Txt),
                         [](const testing::TestParamInfo<ArtifactFormat>& info) {
                             return ArtifactName(info.param);
                         });

// 失败路径也是矩阵的一部分: 目录不可写时五种格式都不许报成功。
TEST(QcReportExporterTest, UnwritableDirectoryFailsForEveryFormat) {
    ScopedDir dir("unwritable");
    const std::string missing = (dir.path / "no_such_subdir" / "report").string();
    const QcExportBundle bundle = MakeTrickyBundle();

    EXPECT_FALSE(QcReportExporter::ExportJson(missing + ".json", bundle));
    EXPECT_FALSE(QcReportExporter::ExportCsv(missing + ".csv", bundle));
    EXPECT_FALSE(QcReportExporter::ExportHtml(missing + ".html", bundle));
    EXPECT_FALSE(QcReportExporter::ExportText(missing + ".txt", bundle));
    EXPECT_FALSE(QcReportExporter::ExportPdf(missing + ".pdf", bundle).ok);
}

}  // namespace
