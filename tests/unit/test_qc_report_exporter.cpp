#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/domain/model/QcReport.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/reporting/QcReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/PdfVerifier.h"

using videoeye::JsonParse;
using videoeye::JsonValue;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::qc::QcRunResult;
using videoeye::reporting::QcExportBundle;
using videoeye::reporting::QcReportExporter;
using videoeye::reporting::QcReportOutputPath;

namespace {
DiagnosticIssue MakeIssue(const std::string& rule_id, IssueSeverity sev, IssueCategory cat, const std::string& title) {
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
    if (issue_count > 0)
        run.report.issues.push_back(
            MakeIssue("video.gop.max_seconds", IssueSeverity::Critical, IssueCategory::Gop, "GOP 过长"));
    if (issue_count > 1)
        run.report.issues.push_back(
            MakeIssue("audio.loudness.range", IssueSeverity::Error, IssueCategory::Audio, "响度超标"));
    if (issue_count > 2)
        run.report.issues.push_back(
            MakeIssue("container.missing_subtitle", IssueSeverity::Warning, IssueCategory::Container, "缺字幕"));
    // 3 个以上: 多页 PDF / 大批量用例才需要, 编号进标题便于定位
    for (int i = 3; i < issue_count; ++i) {
        run.report.issues.push_back(MakeIssue("video.extra.rule_" + std::to_string(i), IssueSeverity::Info,
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
               ("videoeye_report_" + tag + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }
    ~ScopedDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string File(const std::string& name) const {
        return (path / name).string();
    }
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
    if (content.compare(0, 3, "\xEF\xBB\xBF") == 0)
        i = 3; // UTF-8 BOM
    for (; i < content.size(); ++i) {
        const char c = content[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < content.size() && content[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else
                    in_quotes = false;
            } else {
                field += c; // 引号内的换行/逗号原样保留
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

// ---- CSV 元数据头（行首为 '#' 的行）----
//
// 行语法见 QcReportExporterCsv.cpp 顶部注释。这里按**物理行**切分是有意的：
// 元数据行的值在导出端被压过换行，物理行数必须等于记录数 —— 一旦哪天忘了压，
// 下面这些按行读取的辅助函数就会把一行拆成两行，断言随即失败（正是要守的东西）。

std::vector<std::string> SplitCsvPhysicalLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (i == 0 && content.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            i = 2; // 跳过 UTF-8 BOM
            continue;
        }
        const char c = content[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

// 只留表头 + 数据行：模拟"用 comment='#' 读表"的下游脚本
std::string DropCsvMetaLines(const std::string& content) {
    std::string out;
    for (const std::string& line : SplitCsvPhysicalLines(content)) {
        if (!line.empty() && line.front() == '#')
            continue;
        out += line;
        out += '\n';
    }
    return out;
}

std::vector<std::vector<std::string>> ParseCsvDataRecords(const std::string& content) {
    return ParseCsvRecords(DropCsvMetaLines(content));
}

// 元数据头解析结果：单值事实按"节 -> 键 -> 值"收，覆盖表单独收。
struct CsvMetaFacts {
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::vector<std::string> coverage_columns;
    std::vector<std::vector<std::string>> coverage_rows;
    std::size_t meta_line_count = 0;

    // 找不到时返回空串（而不是让用例抛异常）：缺字段正是要被断言出来的那种失败
    std::string Value(const std::string& section, const std::string& key) const {
        const auto section_it = sections.find(section);
        if (section_it == sections.end())
            return std::string();
        const auto value_it = section_it->second.find(key);
        return value_it == section_it->second.end() ? std::string() : value_it->second;
    }
};

CsvMetaFacts ParseCsvMetaFacts(const std::string& content) {
    CsvMetaFacts facts;
    for (const std::string& line : SplitCsvPhysicalLines(content)) {
        if (line.empty() || line.front() != '#')
            continue;
        ++facts.meta_line_count;
        // 元数据行保证一行一条记录，取第一条即可
        const auto records = ParseCsvRecords(line);
        if (records.empty())
            continue;
        const auto& fields = records.front();
        if (fields.empty())
            continue;
        const std::string section = fields.front().substr(1); // 去掉行首的 '#'
        if (section == "覆盖") {
            if (fields.size() > 1 && fields[1] == "维度")
                facts.coverage_columns = fields; // 列名行
            else
                facts.coverage_rows.push_back(fields); // 一个维度一行，列与列名行对齐
            continue;
        }
        for (std::size_t i = 1; i + 1 < fields.size(); i += 2)
            facts.sections[section][fields[i]] = fields[i + 1];
    }
    return facts;
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
    injected.title = "=cmd|'/c calc'!A0"; // 公式注入载荷
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
    const std::string path = std::filesystem::temp_directory_path().string() + "/videoeye_qc_export_csv_test.csv";
    ASSERT_TRUE(QcReportExporter::ExportCsv(path, MakeBundle(3)));

    const std::string content = ReadWholeFile(path);
    // 表头 + 3 个 issue（'#' 开头的元数据行不属于数据表）
    EXPECT_EQ(ParseCsvDataRecords(content).size(), 4u);
    EXPECT_NE(content.find("video.gop.max_seconds"), std::string::npos);
    EXPECT_NE(content.find("audio.loudness.range"), std::string::npos);
    EXPECT_NE(content.find("container.missing_subtitle"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(QcReportExporterTest, JsonSchemaIsStable) {
    const std::string path = std::filesystem::temp_directory_path().string() + "/videoeye_qc_export_json_test.json";
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
    const std::string path = std::filesystem::temp_directory_path().string() + "/videoeye_qc_export_empty_test.csv";
    ASSERT_TRUE(QcReportExporter::ExportCsv(path, MakeBundle(0)));
    const std::string content = ReadWholeFile(path);
    EXPECT_EQ(ParseCsvDataRecords(content).size(), 1u); // 只剩表头

    // 空表本身证明不了"跑过"：元数据头必须把结论与计数一起落下。
    const CsvMetaFacts facts = ParseCsvMetaFacts(content);
    EXPECT_EQ(facts.Value("统计", "问题数"), "0");
    EXPECT_EQ(facts.Value("结论", "分析成功"), "是");
    EXPECT_EQ(facts.Value("结论", "结论完整"), "是");
    // 规则条数也要在：0 条启用 与 "12 条全跑过且没命中" 在空表上长得一样
    EXPECT_EQ(facts.Value("统计", "规则"), "0");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// CSV 最容易被忽略的一种失效：数据区只有表头时，"真没问题"与"没跑完/被取消/超时"
// 肉眼和脚本都分不出来。下面这条同时钉住两件事：
//   1) 两种情况下数据区**确实完全一样**（所以只靠数据区的判据根本不存在）；
//   2) 元数据头必须把两者区分开，且写清原因。
TEST(QcReportExporterTest, CsvMetaHeaderSeparatesNoIssuesFromNotRun) {
    ScopedDir dir("csv_meta");
    QcExportBundle clean = MakeBundle(0);
    QcExportBundle interrupted = MakeBundle(0);
    interrupted.run.ok = false;
    interrupted.run.timed_out = true;
    interrupted.run.error = "分析超时（后台线程无响应）";
    interrupted.run.report.completed = false;

    const std::string clean_path = dir.File("clean.csv");
    const std::string interrupted_path = dir.File("interrupted.csv");
    ASSERT_TRUE(QcReportExporter::ExportCsv(clean_path, clean));
    ASSERT_TRUE(QcReportExporter::ExportCsv(interrupted_path, interrupted));

    const std::string clean_content = ReadWholeFile(clean_path);
    const std::string interrupted_content = ReadWholeFile(interrupted_path);
    ASSERT_FALSE(clean_content.empty());
    EXPECT_EQ(DropCsvMetaLines(clean_content), DropCsvMetaLines(interrupted_content))
        << "两种情形的数据区本来就该一样 —— 区分只能靠元数据头";

    const CsvMetaFacts ok_facts = ParseCsvMetaFacts(clean_content);
    const CsvMetaFacts bad_facts = ParseCsvMetaFacts(interrupted_content);
    EXPECT_EQ(ok_facts.Value("结论", "分析成功"), "是");
    EXPECT_EQ(ok_facts.Value("结论", "超时放弃"), "否");
    EXPECT_TRUE(ok_facts.Value("结论", "错误原因").empty());

    EXPECT_EQ(bad_facts.Value("结论", "分析成功"), "否");
    EXPECT_EQ(bad_facts.Value("结论", "结论完整"), "否");
    EXPECT_EQ(bad_facts.Value("结论", "超时放弃"), "是");
    EXPECT_EQ(bad_facts.Value("结论", "错误原因"), "分析超时（后台线程无响应）");
    // 两份都是 0 个问题 —— 所以"问题数"不能当区分依据，完整性标志才是
    EXPECT_EQ(ok_facts.Value("统计", "问题数"), "0");
    EXPECT_EQ(bad_facts.Value("统计", "问题数"), "0");
}

// 逐维度覆盖的用例放在 MakeEnrichedBundle() 之后（需要那份带维度状态的 fixture）。

TEST(QcReportExporterTest, OutputPathUsesExtension) {
    const std::string dir = std::filesystem::temp_directory_path().string();
    auto expected = [&](const std::string& ext) { return (std::filesystem::path(dir) / ("clip" + ext)).string(); };
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

    const std::string path = std::filesystem::temp_directory_path().string() + "/videoeye_qc_summary_test.csv";
    ASSERT_TRUE(QcReportExporter::ExportBatchSummaryCsv(path, input));

    std::ifstream in(path);
    int lines = 0;
    std::string line;
    while (std::getline(in, line))
        ++lines;
    EXPECT_EQ(lines, 5); // 表头 + 4 行

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
    EXPECT_GE(pdf.font_objects.size(), 3u); // F1 / F2 / F3 都得在
    EXPECT_EQ(pdf.stream_length_mismatch, 0);
    EXPECT_GT(pdf.text_runs, 0);
    // 每行画完必须有换行操作符：少了它整页会首尾相接画在同一条基线上，
    // 阅读器里只能看到开头那一小段 —— 而文件本身照样"导出成功"。
    EXPECT_GE(pdf.text_lines, 10) << "文本只排了 " << pdf.text_lines << " 行, 疑似全部叠在一起";
}

TEST(QcReportExporterTest, PdfMultiPageIsStructurallyValid) {
    ScopedDir dir("pdf_multi");
    const std::string path = dir.File("report.pdf");
    // 每页 50 行上限；正文来自纯文本报告，固定头部 14 行 + 每个 issue 4 行，
    // 14 个 issue 就是 70 行，必然溢出到第二页。
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
    EXPECT_NE(pdf.text.find("未发现问题。"), std::string::npos);
    EXPECT_NE(pdf.text.find("VideoEye QC 报告"), std::string::npos);
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
    case ArtifactFormat::Json:
        return "json";
    case ArtifactFormat::Csv:
        return "csv";
    case ArtifactFormat::Html:
        return "html";
    case ArtifactFormat::Pdf:
        return "pdf";
    case ArtifactFormat::Txt:
        return "txt";
    }
    return "unknown";
}

const char* ArtifactExtension(ArtifactFormat format) {
    switch (format) {
    case ArtifactFormat::Json:
        return "json";
    case ArtifactFormat::Csv:
        return "csv";
    case ArtifactFormat::Html:
        return "html";
    case ArtifactFormat::Pdf:
        return "pdf";
    case ArtifactFormat::Txt:
        return "txt";
    }
    return "bin";
}

class ReportArtifactMatrix : public testing::TestWithParam<ArtifactFormat> {};

TEST_P(ReportArtifactMatrix, ExportedArtifactCanBeReadBack) {
    const ArtifactFormat format = GetParam();
    ScopedDir dir(std::string("matrix_") + ArtifactName(format));
    const std::string path = dir.File(std::string("report.") + ArtifactExtension(format));
    const QcExportBundle bundle = MakeTrickyBundle();
    const int issue_count = static_cast<int>(bundle.run.report.issues.size());
    ASSERT_EQ(issue_count, 4);

    bool ok = false;
    switch (format) {
    case ArtifactFormat::Json:
        ok = QcReportExporter::ExportJson(path, bundle);
        break;
    case ArtifactFormat::Csv:
        ok = QcReportExporter::ExportCsv(path, bundle);
        break;
    case ArtifactFormat::Html:
        ok = QcReportExporter::ExportHtml(path, bundle);
        break;
    case ArtifactFormat::Pdf:
        ok = QcReportExporter::ExportPdf(path, bundle).ok;
        break;
    case ArtifactFormat::Txt:
        ok = QcReportExporter::ExportText(path, bundle);
        break;
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
        for (const char* key :
             {"schema", "schema_version", "profile", "file", "summary", "issues", "metrics", "streams", "rules"}) {
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
        // 表头 + 每个 issue 一行。detail 里本来就带换行，导出端把它压成空格，
        // 于是"物理行数"必须正好等于"元数据行数 + 记录数" —— 一旦哪天忘了压，行数会虚增，
        // 下游按行读取的脚本就全错位了。
        const CsvMetaFacts meta = ParseCsvMetaFacts(content);
        EXPECT_GT(meta.meta_line_count, 0u) << "CSV 必须带元数据头，否则空表无法自证";
        const auto rows = ParseCsvDataRecords(content);
        EXPECT_EQ(static_cast<long>(std::count(content.begin(), content.end(), '\n')),
                  static_cast<long>(issue_count) + 1 + static_cast<long>(meta.meta_line_count));
        ASSERT_EQ(rows.size(), static_cast<std::size_t>(issue_count) + 1);
        for (const auto& row : rows)
            ASSERT_EQ(row.size(), 12u);
        EXPECT_EQ(rows[1][10].find('\n'), std::string::npos) << "CSV 字段里不该残留换行: " << rows[1][10];
        EXPECT_EQ(rows[1][10], "实测 5.20 秒，超过 2.00 秒阈值 建议检查编码器的关键帧间隔");
        // 带引号与逗号的字段必须能被 RFC 4180 解析器原样还原
        EXPECT_EQ(rows[2][10], "含 \"引号\" 与,逗号");
        // 公式注入: 以 = 开头的字段必须被前置单引号中和
        EXPECT_EQ(rows[2][4].compare(0, 2, "'="), 0) << "公式注入未被处理: " << rows[2][4];
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
                         testing::Values(ArtifactFormat::Json, ArtifactFormat::Csv, ArtifactFormat::Html,
                                         ArtifactFormat::Pdf, ArtifactFormat::Txt),
                         [](const testing::TestParamInfo<ArtifactFormat>& info) { return ArtifactName(info.param); });

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

// ==========================================================================
// MPEG-TS 传输层证据（可追溯性）
//
// 锁的是这条：规则只在**命中**时才有 detail，未命中什么都不留。于是报告里
// "连续计数错误 = 0" 和 "压根没扫传输层" 会长得一模一样，争议时无法回答
// "扫了没有"。下面三个用例分别锁住：扫到了要落数字、是 TS 但没扫出结论要显式
// 告警、不是 TS 不该有这一节。
// ==========================================================================

namespace {

// 取成员并在缺失时把键名报出来（否则后面一串 EXPECT 只会看到莫名的默认值）。
const JsonValue* Member(const JsonValue* obj, const std::string& key) {
    const JsonValue* found = (obj != nullptr) ? obj->Find(key) : nullptr;
    EXPECT_TRUE(found != nullptr) << "缺字段: " << key;
    return found;
}
double Num(const JsonValue* obj, const std::string& key) {
    const JsonValue* v = Member(obj, key);
    return (v != nullptr) ? v->NumberValueOr(-1.0) : -1.0;
}
bool Flag(const JsonValue* obj, const std::string& key) {
    const JsonValue* v = Member(obj, key);
    return (v != nullptr) && v->BoolValueOr(false);
}

// 造一份填好传输层数字的结果。applicable/analyzed 分开控制，好把三种情形钉死。
QcExportBundle MakeTsBundle(bool applicable, bool analyzed) {
    QcExportBundle bundle = MakeBundle(0);
    bundle.run.report.file_name = "clip.ts";
    bundle.run.report.file_path = "/tmp/clip.ts";
    bundle.run.report.container_format = "mpegts";
    auto& ts = bundle.run.analysis.ts_transport;
    ts.applicable = applicable;
    ts.analyzed = analyzed;
    if (analyzed) {
        ts.packet_size = 188;
        ts.scanned_packets = 50000;
        ts.estimated_packets = 50000;
        ts.scan_complete = true;
        ts.programs = 1;
        ts.streams = 2;
        ts.services = 1;
        ts.has_pat = true;
        ts.has_pmt = true;
        ts.has_sdt = true;
        ts.sync_loss = 0;
        ts.continuity_errors = 10;
        ts.scrambled_packets = 0;
        ts.pcr_max_gap_ms = 40.0;
    } else {
        ts.error_message = "文件打不开";
    }
    return bundle;
}

} // namespace

TEST(QcReportExporterTest, TsTransportFactsLandInJsonEvenWithoutAnyIssue) {
    const QcExportBundle bundle = MakeTsBundle(true, true);

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* metrics = root.Find("metrics");
    ASSERT_TRUE(metrics != nullptr);
    const JsonValue* ts = metrics->Find("ts_transport");
    ASSERT_TRUE(ts != nullptr) << "扫过的 TS 必须在报告里留下传输层数字";

    EXPECT_TRUE(Flag(ts, "analyzed"));
    EXPECT_EQ(Num(ts, "packet_size"), 188.0);
    EXPECT_EQ(Num(ts, "scanned_packets"), 50000.0);
    EXPECT_EQ(Num(ts, "estimated_packets"), 50000.0);
    EXPECT_TRUE(Flag(ts, "scan_complete"));
    EXPECT_EQ(Num(ts, "continuity_errors"), 10.0);
    // 错误率 10/50000 = 0.02%
    EXPECT_NEAR(Num(ts, "continuity_error_percent"), 0.02, 1e-9);
    EXPECT_EQ(Num(ts, "pcr_max_gap_ms"), 40.0);
    EXPECT_TRUE(Flag(ts, "has_pat"));

    // 关键：这份报告里一个问题都没有（MakeBundle(0)），但数字照样在。
    EXPECT_TRUE(bundle.run.report.issues.empty());
}

TEST(QcReportExporterTest, TsWithoutTransportResultIsCalledOutNotSilentlySkipped) {
    const QcExportBundle bundle = MakeTsBundle(true, false);

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* ts = root.Find("metrics")->Find("ts_transport");
    ASSERT_TRUE(ts != nullptr) << "是 TS 但没扫出结论，必须显式出现，不能和'不是 TS'混为一谈";
    EXPECT_FALSE(Flag(ts, "analyzed"));
    ASSERT_TRUE(ts->Find("error_message") != nullptr);
    EXPECT_EQ(ts->Find("error_message")->StringValueOr(), "文件打不开");

    const std::string text = QcReportExporter::BuildText(bundle);
    EXPECT_NE(text.find("MPEG-TS 传输层"), std::string::npos);
    EXPECT_NE(text.find("没有证据"), std::string::npos) << "必须说清未命中≠没问题";

    const std::string html = QcReportExporter::BuildHtml(bundle);
    EXPECT_NE(html.find("MPEG-TS 传输层"), std::string::npos);
}

TEST(QcReportExporterTest, NonTsReportHasNoTsTransportSection) {
    // applicable=false（默认）：MP4 之类压根不该有传输层一节，否则每份报告都多一段噪音。
    const QcExportBundle bundle = MakeBundle(1);
    EXPECT_FALSE(bundle.run.analysis.ts_transport.applicable);

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* metrics = root.Find("metrics");
    ASSERT_TRUE(metrics != nullptr);
    EXPECT_TRUE(metrics->Find("ts_transport") == nullptr);
    EXPECT_EQ(QcReportExporter::BuildText(bundle).find("MPEG-TS 传输层"), std::string::npos);
    EXPECT_EQ(QcReportExporter::BuildHtml(bundle).find("MPEG-TS 传输层"), std::string::npos);
}

TEST(QcReportExporterTest, TsTransportSectionAppearsInTextAndHtml) {
    const QcExportBundle bundle = MakeTsBundle(true, true);
    const std::string text = QcReportExporter::BuildText(bundle);
    EXPECT_NE(text.find("MPEG-TS 传输层"), std::string::npos);
    EXPECT_NE(text.find("扫描范围"), std::string::npos);
    EXPECT_NE(text.find("已扫全文件"), std::string::npos);

    const std::string html = QcReportExporter::BuildHtml(bundle);
    EXPECT_NE(html.find("MPEG-TS 传输层"), std::string::npos);
    EXPECT_NE(html.find("连续计数错误"), std::string::npos);
}

// ==========================================================================
// 阶段 1/4/5 新增数据在报告里的落地：证据与覆盖、参数变化时间线、逐轨音频 QC
//
// 锁的是"这些新结论真的出现在三种格式里"，以及两条容易写错的边界：
//   * 无覆盖信息时必须**缺省** coverage_ratio，落 0 会被读成"覆盖率 0%"
//   * 空数组也要落，否则"没有变化"与"没追踪"在报告里长得一样
// ==========================================================================

namespace {

namespace model = videoeye::model;

std::string Str(const JsonValue* obj, const std::string& key) {
    const JsonValue* v = obj != nullptr ? obj->Find(key) : nullptr;
    return v != nullptr ? v->StringValueOr() : std::string();
}

// 一份带"证据 / 参数变化 / 多音轨"三块数据的结果
QcExportBundle MakeEnrichedBundle() {
    QcExportBundle bundle = MakeBundle(1);
    model::AnalysisResult& analysis = bundle.run.analysis;

    // 维度 1：跳过（未执行）
    analysis.SetDimension(model::MakeSkippedDimension(model::AnalysisDimension::TsTransport, "不是 MPEG-TS 容器"));

    // 维度 2：部分完成 + 有覆盖率
    model::DimensionStatus partial;
    partial.dimension = model::AnalysisDimension::FrameStructure;
    partial.executed = true;
    partial.capability = model::AnalysisCapability::Partial;
    partial.scope_unit = "包";
    partial.units_scanned = 5000;
    partial.units_total = 10000;
    partial.has_coverage = true;
    partial.coverage_ratio = 0.5;
    partial.failure_reason = "已到扫描窗口上限";
    analysis.SetDimension(partial);

    // 维度 3：总量未知 —— coverage_ratio 必须缺省
    model::DimensionStatus unknown_total;
    unknown_total.dimension = model::AnalysisDimension::VideoBitstream;
    unknown_total.executed = true;
    unknown_total.capability = model::AnalysisCapability::Supported;
    unknown_total.scope_unit = "轨";
    unknown_total.units_scanned = 1;
    unknown_total.has_coverage = false;
    analysis.SetDimension(unknown_total);

    // 参数变化：一次同 id 替换
    model::ParameterTimeline timeline;
    timeline.analyzed = true;
    timeline.stream_index = 0;
    timeline.codec_name = "h264";
    timeline.packets_scanned = 15320;
    model::ParameterChangeEvent change;
    change.stream_index = 0;
    change.kind = model::ParameterSetKind::Sps;
    change.parameter_set_id = 0;
    change.has_time = true;
    change.time_seconds = 755.24;
    change.packet_index = 15320;
    change.file_offset = 0x1A2B3C;
    change.source = "SPS(id=0)";
    change.old_config = "1280x720 / 8 bit / 4:2:0";
    change.new_config = "1920x1080 / 8 bit / 4:2:0";
    change.changes = "1280x720 -> 1920x1080";
    change.replaced_same_id = true;
    timeline.changes.push_back(change);
    analysis.parameter_timelines.push_back(timeline);

    // 帧级显示顺序：带重排与 GOP 分段，够两端各自渲染出一整节
    model::FrameOrderTimeline frame_order;
    frame_order.stream_index = 0;
    frame_order.codec = "h264";
    frame_order.order_source = "poc";
    frame_order.available = true;
    frame_order.frames_observed = 300;
    frame_order.i_count = 3;
    frame_order.p_count = 120;
    frame_order.b_count = 177;
    frame_order.idr_count = 3;
    frame_order.irap_count = 3;
    frame_order.reorder_events = 60;
    frame_order.reorder_depth = 2;
    frame_order.gop_count = 3;
    frame_order.min_gop_frames = 90;
    frame_order.max_gop_frames = 110;
    frame_order.avg_gop_frames = 100.0;
    analysis.frame_order_timelines.push_back(frame_order);

    // 逐轨音频：两条轨，一条满覆盖一条半覆盖
    model::AudioQcResult track0;
    track0.analyzed = true;
    track0.integrated_lufs = -23.0;
    track0.true_peak_dbtp = -1.0;
    track0.metadata.sample_rate = 48000;
    track0.metadata.channels = 2;
    track0.metadata.channel_layout = "立体声 (2.0)";
    track0.coverage_ratio = 1.0;
    model::AudioQcResult track1 = track0;
    track1.integrated_lufs = -18.5;
    track1.coverage_ratio = 0.4;
    analysis.audio_qc_streams.push_back(track0);
    analysis.audio_qc_streams.push_back(track1);
    analysis.audio_qc_stream_indices.push_back(1);
    analysis.audio_qc_stream_indices.push_back(2);
    analysis.audio_qc_tracks_analyzed = 2;
    analysis.audio_qc_tracks_total = 3;

    // 主音轨响度：点亮「响度（主音轨）」，让这份 fixture 覆盖全部章节
    analysis.audio_qc.analyzed = true;
    analysis.audio_qc.integrated_lufs = -23.0;
    analysis.audio_qc.true_peak_dbtp = -1.0;
    analysis.audio_qc.loudness_range_lu = 5.0;

    // 流列表：一条视频 + 一张封面 + 一条音频。
    // 封面必须显示成"封面"而不是"视频" —— 它在码流里是一轨 MJPEG，但用户眼里不是画面。
    model::StreamDigest video_stream;
    video_stream.index = 0;
    video_stream.media_type = 0; // AVMEDIA_TYPE_VIDEO
    video_stream.codec_name = "h264";
    video_stream.profile_name = "High";
    video_stream.width = 1920;
    video_stream.height = 1080;
    video_stream.bitrate_bps = 5200000;
    video_stream.frame_count = 300;
    video_stream.packet_count = 300;
    analysis.streams.push_back(video_stream);

    model::StreamDigest cover_stream = video_stream;
    cover_stream.index = 1;
    cover_stream.attached_picture = true;
    cover_stream.codec_name = "mjpeg";
    cover_stream.profile_name.clear();
    cover_stream.width = 640;
    cover_stream.height = 640;
    cover_stream.bitrate_bps = 0;
    cover_stream.frame_count = 1;
    cover_stream.packet_count = 1;
    analysis.streams.push_back(cover_stream);

    model::StreamDigest audio_stream;
    audio_stream.index = 2;
    audio_stream.media_type = 1; // AVMEDIA_TYPE_AUDIO
    audio_stream.codec_name = "aac";
    audio_stream.profile_name = "LC";
    audio_stream.sample_rate = 48000;
    audio_stream.channels = 2;
    audio_stream.bitrate_bps = 128000;
    audio_stream.packet_count = 560;
    analysis.streams.push_back(audio_stream);

    // 规则快照：故意带一条停用的 —— 报告要能说清"启用 3 / 4"，
    // 否则读者会把"停用了响度规则"读成"响度没问题"。
    const struct {
        const char* id;
        const char* name;
        IssueCategory category;
        double threshold;
        const char* unit;
        bool enabled;
    } rule_specs[] = {
        {"video.gop.max_seconds", "GOP 最长时长", IssueCategory::Gop, 4.0, "s", true},
        {"audio.loudness.range", "响度范围上限", IssueCategory::Audio, 20.0, "LU", true},
        {"container.missing_subtitle", "缺字幕", IssueCategory::Container, 0.0, "", false},
        {"video.bitrate.min", "视频码率下限", IssueCategory::Video, 2000.0, "kbps", true},
    };
    for (const auto& spec : rule_specs) {
        model::QcRule rule;
        rule.id = spec.id;
        rule.name = spec.name;
        rule.category = spec.category;
        rule.threshold = spec.threshold;
        rule.unit = spec.unit;
        rule.enabled = spec.enabled;
        bundle.run.report.rules.push_back(rule);
    }

    // 对比 与 MPEG-TS 传输层：这两节两端的标题不同（"对比" / "文件对比"），
    // 正好把对照表本身的映射也验一遍。
    bundle.has_comparison = true;
    bundle.comparison.left_path = "/tmp/left.ts";
    bundle.comparison.right_path = "/tmp/right.ts";

    analysis.ts_transport.applicable = true;
    analysis.ts_transport.analyzed = true;
    analysis.ts_transport.packet_size = 188;
    analysis.ts_transport.scanned_packets = 120000;
    analysis.ts_transport.estimated_packets = 120000;
    analysis.ts_transport.scan_complete = true;
    analysis.ts_transport.programs = 1;
    analysis.ts_transport.streams = 2;
    analysis.ts_transport.services = 1;
    analysis.ts_transport.has_pat = true;
    analysis.ts_transport.has_pmt = true;
    analysis.ts_transport.has_sdt = false;
    analysis.ts_transport.continuity_errors = 7;
    analysis.ts_transport.pcr_max_gap_ms = 38.0;
    return bundle;
}

} // namespace

TEST(QcReportExporterTest, DimensionStatusTableLandsInJson) {
    const QcExportBundle bundle = MakeEnrichedBundle();

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* dimensions = root.Find("dimensions");
    ASSERT_TRUE(dimensions != nullptr) << "阶段 5 的「证据与覆盖」必须是顶层章节";
    ASSERT_EQ(dimensions->Size(), 3u);

    // 跳过的那一行：executed=false 且带原因 —— 这是"没证据"而非"没问题"
    const JsonValue& skipped = dimensions->Items()[0];
    EXPECT_EQ(Str(&skipped, "dimension"), "ts_transport");
    EXPECT_FALSE(Flag(&skipped, "executed"));
    EXPECT_EQ(Str(&skipped, "capability"), "unavailable");
    EXPECT_FALSE(Flag(&skipped, "conclusive"));
    EXPECT_EQ(Str(&skipped, "failure_reason"), "不是 MPEG-TS 容器");

    // 部分完成：覆盖率 50%，conclusive 必须 false
    const JsonValue& partial = dimensions->Items()[1];
    EXPECT_EQ(Str(&partial, "dimension"), "frame_structure");
    EXPECT_TRUE(Flag(&partial, "executed"));
    EXPECT_EQ(Str(&partial, "capability"), "partial");
    EXPECT_FALSE(Flag(&partial, "conclusive")) << "Partial 不能给出可用的通过结论";
    EXPECT_NEAR(Num(&partial, "coverage_ratio"), 0.5, 1e-9);
    EXPECT_EQ(Num(&partial, "units_scanned"), 5000.0);
    EXPECT_EQ(Num(&partial, "units_total"), 10000.0);
}

// 边界：has_coverage=false 时绝不能落 coverage_ratio —— 0 会被读成"覆盖率为 0"
TEST(QcReportExporterTest, MissingCoverageOmitsCoverageField) {
    const QcExportBundle bundle = MakeEnrichedBundle();

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* dimensions = root.Find("dimensions");
    ASSERT_TRUE(dimensions != nullptr);
    const JsonValue& unknown_total = dimensions->Items()[2];
    EXPECT_EQ(Str(&unknown_total, "dimension"), "video_bitstream");
    EXPECT_TRUE(unknown_total.Find("coverage_ratio") == nullptr) << "总量未知时不能落覆盖率，否则会被读成 0%";
    EXPECT_EQ(Num(&unknown_total, "units_scanned"), 1.0);
}

TEST(QcReportExporterTest, ParameterTimelineLandsInJsonWithLocator) {
    const QcExportBundle bundle = MakeEnrichedBundle();

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* timelines = root.Find("parameter_timelines");
    ASSERT_TRUE(timelines != nullptr);
    ASSERT_EQ(timelines->Size(), 1u);

    const JsonValue& first = timelines->Items()[0];
    EXPECT_EQ(Num(&first, "stream_index"), 0.0);
    EXPECT_EQ(Str(&first, "codec"), "h264");
    EXPECT_EQ(Num(&first, "packets_scanned"), 15320.0);

    const JsonValue* changes = first.Find("changes");
    ASSERT_TRUE(changes != nullptr);
    ASSERT_EQ(changes->Size(), 1u);
    const JsonValue& change = changes->Items()[0];
    EXPECT_EQ(Str(&change, "kind"), "sps");
    EXPECT_TRUE(Flag(&change, "replaced_same_id")) << "同 id 换内容是最危险的一类，必须可识别";
    EXPECT_EQ(Str(&change, "old_config"), "1280x720 / 8 bit / 4:2:0");
    EXPECT_EQ(Str(&change, "new_config"), "1920x1080 / 8 bit / 4:2:0");
    // 可定位三件套：时间 / 包号 / 文件偏移
    EXPECT_NEAR(Num(&change, "time_seconds"), 755.24, 1e-9);
    EXPECT_EQ(Num(&change, "packet_index"), 15320.0);
    EXPECT_EQ(Num(&change, "file_offset"), static_cast<double>(0x1A2B3C));
    // 文本形态也落一份，报告读者不必自己拼
    EXPECT_NE(Str(&change, "text").find("0x1A2B3C"), std::string::npos);
}

// 空数组也要落：报告要能区分"没有配置变化"与"没做参数追踪"，
// 后者由 dimensions 里 parameter_timeline 那一行的 executed 说明。
TEST(QcReportExporterTest, EmptyParameterTimelinesStillExportedAsArray) {
    const QcExportBundle bundle = MakeBundle(0);

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* timelines = root.Find("parameter_timelines");
    ASSERT_TRUE(timelines != nullptr) << "没有数据也要落空数组";
    EXPECT_EQ(timelines->Size(), 0u);
    const JsonValue* audio = root.Find("audio_qc_streams");
    ASSERT_TRUE(audio != nullptr);
    EXPECT_EQ(audio->Size(), 0u);
}

TEST(QcReportExporterTest, PerTrackAudioQcLandsInJson) {
    const QcExportBundle bundle = MakeEnrichedBundle();

    JsonValue root;
    std::string err;
    ASSERT_TRUE(JsonParse(QcReportExporter::BuildJson(bundle), root, &err)) << err;
    const JsonValue* tracks = root.Find("audio_qc_streams");
    ASSERT_TRUE(tracks != nullptr);
    ASSERT_EQ(tracks->Size(), 2u);

    const JsonValue& t0 = tracks->Items()[0];
    EXPECT_EQ(Num(&t0, "stream_index"), 1.0) << "流号来自并行索引表，不能错位";
    EXPECT_NEAR(Num(&t0, "integrated_lufs"), -23.0, 1e-9);
    EXPECT_NEAR(Num(&t0, "coverage_ratio"), 1.0, 1e-9);

    const JsonValue& t1 = tracks->Items()[1];
    EXPECT_EQ(Num(&t1, "stream_index"), 2.0);
    EXPECT_NEAR(Num(&t1, "integrated_lufs"), -18.5, 1e-9);
    // 逐轨覆盖率是这块的核心：轨数对上了不代表每条轨都跑满
    EXPECT_NEAR(Num(&t1, "coverage_ratio"), 0.4, 1e-9);
}

// 三种格式都要有：JSON 有字段、TEXT/HTML 有人能读的段落
TEST(QcReportExporterTest, NewSectionsAppearInTextAndHtml) {
    const QcExportBundle bundle = MakeEnrichedBundle();

    const std::string text = QcReportExporter::BuildText(bundle);
    EXPECT_NE(text.find("证据与覆盖"), std::string::npos);
    EXPECT_NE(text.find("参数变化时间线"), std::string::npos);
    EXPECT_NE(text.find("同一参数集 id 的内容被替换"), std::string::npos)
        << "危险变化必须在文本报告里显式提示，不能只留一个 true";
    EXPECT_NE(text.find("音频 QC（逐轨）"), std::string::npos);
    EXPECT_NE(text.find("2 / 3 条音轨参与分析"), std::string::npos);

    const std::string html = QcReportExporter::BuildHtml(bundle);
    EXPECT_NE(html.find("证据与覆盖"), std::string::npos);
    EXPECT_NE(html.find("参数变化时间线"), std::string::npos);
    EXPECT_NE(html.find("音频 QC（逐轨）"), std::string::npos);
}

// 逐维度覆盖在 CSV 里的落法：与 TXT / HTML 的「证据与覆盖」同一份口径，换成列而已。
// 没有它，空表读者只能猜"黑帧检测是不是压根没跑"。
TEST(QcReportExporterTest, CsvCoverageRowsMirrorDimensionStatus) {
    ScopedDir dir("csv_coverage");
    const QcExportBundle bundle = MakeEnrichedBundle();
    const std::string path = dir.File("report.csv");
    ASSERT_TRUE(QcReportExporter::ExportCsv(path, bundle));

    const CsvMetaFacts facts = ParseCsvMetaFacts(ReadWholeFile(path));
    ASSERT_FALSE(facts.coverage_columns.empty()) << "有维度状态时必须给出覆盖表的列名行";
    ASSERT_GT(facts.coverage_columns.size(), 10u);
    EXPECT_EQ(facts.coverage_columns[1], "维度");
    EXPECT_EQ(facts.coverage_columns[8], "覆盖率(%)");

    // 行序 = dimensions 的顺序，不重排：读者按报告里同一顺序对照
    ASSERT_EQ(facts.coverage_rows.size(), 3u);

    const auto& skipped = facts.coverage_rows[0];
    EXPECT_EQ(skipped[1], "MPEG-TS 传输层");
    EXPECT_EQ(skipped[2], "未执行");
    // 占位符必须原样是 '-'：它曾被当成"以 - 开头的字段"加公式前缀，读回来是 "'-"
    EXPECT_EQ(skipped[5], "-");
    EXPECT_NE(skipped[10].find("不是 MPEG-TS"), std::string::npos) << "未执行必须写清原因";

    const auto& partial = facts.coverage_rows[1];
    EXPECT_EQ(partial[1], "帧级结构");
    EXPECT_EQ(partial[2], "已执行");
    EXPECT_EQ(partial[6], "5000");
    EXPECT_EQ(partial[7], "10000");
    EXPECT_EQ(partial[8], "50.0");
    EXPECT_NE(partial[10].find("扫描窗口上限"), std::string::npos);

    const auto& unknown_total = facts.coverage_rows[2];
    EXPECT_EQ(unknown_total[1], "视频码流参数");
    EXPECT_EQ(unknown_total[7], "-") << "总量未知不能落 0（0 会被读成覆盖 0%）";
    EXPECT_EQ(unknown_total[8], "-");
}

// ============================================================
// PDF 与纯文本报告必须同源
//
// 背景（2026-10-30）：PDF 导出器一直自己另排一套正文（英文抬头 + 问题清单 +
// 主轨响度三项），于是「证据与覆盖」「参数变化时间线」「帧级显示顺序」
// 「音频 QC（逐轨）」「MPEG-TS 传输层」这五节在 PDF 里一个字都没有。同一份 QC
// 结论换个格式导出就丢掉全部覆盖说明 —— 而 PDF 恰恰是最常用于交付与存档的
// 格式，读者只能看到"问题清单里没问题"，无法区分"真没问题"和"这个维度根本
// 没跑"。这正是 DimensionStatus 那套结构要消灭的虚假结论，换个格式又长回来了。
//
// 现在 PDF 的正文直接复用 BuildText，下面这条用例把"同源"钉死：谁再在 PDF 侧
// 另起一套，它立刻红。
//
// 判据为什么选"去掉空白后逐字全等"而不是抽查关键词：关键词抽查漏得掉整节
// （本轮就是这么漏的）；去空白能容忍两端各自的分隔线与折行，却对"少一段正文"
// 零容忍，顺带把"折行不丢字符"也一起守住。
// ============================================================

std::string DropWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        out.push_back(c);
    }
    return out;
}

TEST(QcReportExporterTest, PdfBodyIsTheSameTextAsTheTxtReport) {
    ScopedDir dir("pdf_parity");
    const std::string path = dir.File("parity.pdf");
    const QcExportBundle bundle = MakeEnrichedBundle();
    const auto result = QcReportExporter::ExportPdf(path, bundle);
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.text_loss) << "中文不该被判成丢字（只有 BMP 外字符才算）";

    const auto pdf = videoeye_test::VerifyPdfFile(path);
    ASSERT_TRUE(pdf.ok) << pdf.error;

    // 逐节点名，失败时能直接看出缺的是哪一节（比"全等失败"好定位）。
    for (const char* section : {"证据与覆盖", "参数变化时间线", "音频 QC（逐轨）"}) {
        EXPECT_NE(pdf.text.find(section), std::string::npos)
            << "PDF 里缺这一节: " << section << " —— 正文应与纯文本报告同源";
    }

    const std::string expected = DropWhitespace(QcReportExporter::BuildText(bundle));
    const std::string actual = DropWhitespace(pdf.text);
    ASSERT_FALSE(expected.empty());

    std::size_t i = 0;
    while (i < expected.size() && i < actual.size() && expected[i] == actual[i])
        ++i;
    const std::size_t from = (i > 24) ? (i - 24) : 0;
    ASSERT_EQ(i, expected.size()) << "PDF 正文在偏移 " << i
                                  << " 处与纯文本报告不同\n  文本: " << expected.substr(from, 40)
                                  << "\n  PDF : " << actual.substr(from, 40);
    ASSERT_EQ(i, actual.size()) << "PDF 比纯文本报告多出内容: " << actual.substr(from, 40);
}

// 折行守护：文本报告里有整段很长的行（TS 统计、参数集 Summary、issue 的 detail），
// 而 MiniPdfWriter 只按 \n 切段、不做自动换行。PDF 侧必须自己折 —— 不折就会一路
// 画出 /MediaBox 右边界被裁掉，而文件仍然"结构有效"、导出仍然返回成功。
//
// 为什么不用 pdf.text 判：VerifyPdfFile 还原文本时不插行分隔符，折没折在 text 上
// 看不出来。改用"同一份内容、标题长短不同的两版对照"：长版必须多占行。
TEST(QcReportExporterTest, PdfWrapsOverlongLinesInsteadOfDrawingPastTheMargin) {
    ScopedDir dir("pdf_wrap");
    QcExportBundle short_bundle = MakeBundle(1);
    QcExportBundle long_bundle = MakeBundle(1);
    // 每行上限是 88 个半角单位，追加 180 个字符至少要折成 3 行。
    long_bundle.run.report.issues[0].title += std::string(180, 'x');
    short_bundle.run.report.issues[0].title += "y";

    const std::string short_path = dir.File("short.pdf");
    const std::string long_path = dir.File("long.pdf");
    ASSERT_TRUE(QcReportExporter::ExportPdf(short_path, short_bundle).ok);
    ASSERT_TRUE(QcReportExporter::ExportPdf(long_path, long_bundle).ok);

    const auto short_pdf = videoeye_test::VerifyPdfFile(short_path);
    const auto long_pdf = videoeye_test::VerifyPdfFile(long_path);
    ASSERT_TRUE(short_pdf.ok) << short_pdf.error;
    ASSERT_TRUE(long_pdf.ok) << long_pdf.error;
    // 两版都必须落在单页里，行数差异才只由折行引起（分页会给每页多算一行）。
    ASSERT_EQ(short_pdf.page_count, 1);
    ASSERT_EQ(long_pdf.page_count, 1);
    EXPECT_GE(long_pdf.text_lines, short_pdf.text_lines + 2)
        << "长标题没有折行：短版 " << short_pdf.text_lines << " 行、长版 " << long_pdf.text_lines
        << " 行。超宽的行会画出页面右边界被裁掉，而 PDF 自身仍然结构有效。";
}

// ============================================================
// 章节一致性：纯文本（= PDF 正文）与 HTML 必须落同样多的章节
//
// 这两个格式各自维护一份"该输出哪些章节"的清单，谁多一节谁少一节都不会编译报错，
// 于是「流列表」「规则快照」在 HTML / JSON 里存在了两三个版本，纯文本与 PDF 里
// 一个字都没有 —— PDF 恰恰是交付与存档最常用的那一版。
//
// 所以这里不用关键词抽查（抽查天生会漏掉整节，本轮就是这么漏的），改成：
//   * 先把两格式实际输出的章节名全抓出来；
//   * 再跟下面这张对照表核：表外的章节 = 有人在某一端加了章节却没同步 → 直接红；
//   * 表内每一对：一端有一端没有 → 红。
// 新增章节必须同步这张表和两端实现，代价很小，收益是以后不可能再静默漂移。
// ============================================================

std::set<std::string> CollectTextSections(const std::string& text) {
    std::set<std::string> titles;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // 形如 "--- 汇总 ---"：整行只有分节标题。
        // 结尾那个空格是可选的（历史上有两节写成了 "--- 音频 QC（逐轨）---"），
        // 不能因为少个空格就整行认不出来 —— 那等于让一致性检查静默漏掉一节。
        if (line.size() > 8 && line.compare(0, 4, "--- ") == 0 && line.compare(line.size() - 3, 3, "---") == 0) {
            std::string title = line.substr(4, line.size() - 7);
            while (!title.empty() && (title.back() == ' ' || title.back() == '\t'))
                title.pop_back();
            titles.insert(title);
        }
    }
    return titles;
}

// 按**出现顺序**抓（顺序有语义，见 HtmlSectionsAppearInTheDocumentedOrder）。
std::vector<std::string> CollectHtmlSectionsInOrder(const std::string& html) {
    std::vector<std::string> titles;
    const std::string open = "<h2>";
    const std::string close = "</h2>";
    std::size_t pos = 0;
    while ((pos = html.find(open, pos)) != std::string::npos) {
        pos += open.size();
        const std::size_t end = html.find(close, pos);
        if (end == std::string::npos)
            break;
        titles.push_back(html.substr(pos, end - pos));
        pos = end + close.size();
    }
    return titles;
}

std::set<std::string> CollectHtmlSections(const std::string& html) {
    std::set<std::string> titles;
    for (const std::string& title : CollectHtmlSectionsInOrder(html))
        titles.insert(title);
    return titles;
}

struct SectionPair {
    const char* text_title;
    const char* html_title;
};

// 名字允许不同（HTML 是表格标题、纯文本是分节线），所以显式列对照。
const SectionPair kSectionPairs[] = {
    {"汇总", "总体结论"},
    {"问题清单", "问题清单"},
    {"证据与覆盖", "证据与覆盖"},
    {"参数变化时间线", "参数变化时间线"},
    {"帧级显示顺序", "帧级显示顺序"},
    {"流列表", "流列表"},
    {"音频 QC（逐轨）", "音频 QC（逐轨）"},
    {"响度（主音轨）", "响度与音频 QC（主音轨）"},
    {"MPEG-TS 传输层", "MPEG-TS 传输层"},
    {"对比", "文件对比"},
    {"规则快照", "规则快照"},
};

// 章节在还不够，内容也得对：流列表要把封面标成"封面"（它在码流里是一轨 MJPEG，
// 但用户眼里不是画面，写成"视频"等于凭空多一路画面）；规则快照要能说清
// "启用 3 / 4"，否则读者会把"停用了响度规则"读成"响度没问题"。
TEST(QcReportExporterTest, StreamListAndRuleSnapshotCarryTheRightContent) {
    const std::string text = QcReportExporter::BuildText(MakeEnrichedBundle());

    for (const char* needle : {"--- 流列表 ---", "#0 视频 h264（High） ｜ 1920x1080", "#1 封面 mjpeg ｜ 640x640",
                               "#2 音频 aac（LC） ｜ 48000 Hz / 2 声道 ｜ 128.0 kbps", "--- 规则快照 ---",
                               "启用 3 / 4 条", "GOP 最长时长（video.gop.max_seconds）", "已停用"}) {
        EXPECT_NE(text.find(needle), std::string::npos) << "纯文本（以及复用它的 PDF）里找不到: " << needle;
    }
    // 封面那一行绝不能出现"视频"字样
    const std::size_t cover = text.find("#1 ");
    ASSERT_NE(cover, std::string::npos);
    const std::string cover_line = text.substr(cover, text.find('\n', cover) - cover);
    EXPECT_EQ(cover_line.find("视频"), std::string::npos) << "封面被写成视频轨了: " << cover_line;
}

TEST(QcReportExporterTest, TextAndHtmlEmitTheSameSetOfSections) {
    const QcExportBundle bundle = MakeEnrichedBundle();
    const std::string text = QcReportExporter::BuildText(bundle);
    const std::string html = QcReportExporter::BuildHtml(bundle);

    const std::set<std::string> text_titles = CollectTextSections(text);
    const std::set<std::string> html_titles = CollectHtmlSections(html);
    ASSERT_FALSE(html_titles.empty()) << "HTML 一个 h2 章节都没有，抓章节名的辅助函数可能失效了";

    const std::vector<SectionPair> pairs(std::begin(kSectionPairs), std::end(kSectionPairs));
    auto known_text = [&pairs](const std::string& title) {
        for (const SectionPair& p : pairs) {
            if (p.text_title == title)
                return true;
        }
        return false;
    };
    auto known_html = [&pairs](const std::string& title) {
        for (const SectionPair& p : pairs) {
            if (p.html_title == title)
                return true;
        }
        return false;
    };

    // 表外章节：某一端加了新章节却没同步这张表与另一端。
    for (const std::string& title : text_titles)
        EXPECT_TRUE(known_text(title)) << "纯文本出现了对照表外的章节「" << title
                                       << "」—— 请同步本表的 html_title 与 QcReportExporterHtml.cpp";
    for (const std::string& title : html_titles)
        EXPECT_TRUE(known_html(title)) << "HTML 出现了对照表外的章节「" << title
                                       << "」—— 请同步本表的 text_title 与 QcReportExporterText.cpp";

    // 表内每一对：一端有一端没有就是内容缺失（PDF 复用纯文本，所以这也守着 PDF）。
    int matched = 0;
    for (const SectionPair& p : pairs) {
        const bool in_text = text_titles.count(p.text_title) > 0;
        const bool in_html = html_titles.count(p.html_title) > 0;
        if (in_text && in_html)
            ++matched;
        else if (in_html)
            ADD_FAILURE() << "HTML 有「" << p.html_title << "」但纯文本（以及复用它的 PDF）没有「" << p.text_title
                          << "」—— 两种格式的结论不再等价";
        else if (in_text)
            ADD_FAILURE() << "纯文本有「" << p.text_title << "」但 HTML 没有「" << p.html_title << "」";
    }

    // 覆盖守护：fixture 缩水或章节抓取失效时，上面所有循环都会变成空转（全绿）。
    // 这份 fixture 应该点亮对照表里的**全部**章节，少一节就说明这一轮覆盖退化了。
    EXPECT_EQ(matched, static_cast<int>(pairs.size()))
        << "两端同时命中的章节只有 " << matched << " / " << pairs.size()
        << " 节 —— fixture 少喂了数据，或某一端不再渲染某一节。一致性检查正在退化成空操作";
}

// 章节**齐全**还不够，**顺序**也有语义：
//   * 「证据与覆盖」必须在「流列表」之前 —— 读者要先知道哪些维度真跑了、覆盖多少，
//     才知道流列表里那些数字能推到什么结论；
//   * 「规则快照」在最后 —— 先给结论，依据殿后。
//
// BuildHtml 现在就是一串 Append* 调用（章节各自独立成函数之后，挪一行就换顺序、且不会
// 有任何编译错误），所以顺序必须由用例钉死在 kSectionPairs 上。
TEST(QcReportExporterTest, HtmlSectionsAppearInTheDocumentedOrder) {
    const std::string html = QcReportExporter::BuildHtml(MakeEnrichedBundle());
    const std::vector<std::string> actual = CollectHtmlSectionsInOrder(html);

    std::vector<std::string> expected;
    for (const SectionPair& p : kSectionPairs)
        expected.push_back(p.html_title);

    // 覆盖守护：抓不到任何 h2 时下面的逐项比对会退化成空转。
    ASSERT_FALSE(actual.empty()) << "一个 h2 都没抓到 —— 抓取辅助函数或 HTML 结构变了";
    EXPECT_EQ(actual, expected) << "HTML 章节顺序与 kSectionPairs 不一致（BuildHtml 的调用顺序被改过？）";
}

} // namespace
