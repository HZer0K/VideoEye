#include <gtest/gtest.h>

// 报告分派一致性：Auto 系列按扩展名选格式，落盘内容必须与扩展名一致。
//
// 历史缺陷（切片 4 调研发现、本切片修复）：
//   * ExportComparisonAuto 的 .pdf 落到 ExportComparisonText；
//   * ExportBatchSummaryAuto 的 .txt 落到 Csv、.pdf 落到 Html；
//   * 三个 Auto 认不出扩展名时静默退回 JSON，写出「叫 .xyz 的 JSON 文件」；
//   * ExportReport(裸 QcReport) 把 run.ok 绑成 report.completed，让
//     「分析成功 / 结论完整」两个独立完整性维度永远同值，还伪造 "分析未完成"。
//
// 这里的识别器**不调任何产品函数**：按 BOM / 魔数 / 结构标记自己判内容格式，
// 再与扩展名比对。每个家族还带一条独有的特征串，防止「格式对了但正文串到
// 别的家族」（例如 .pdf 汇总误调单文件的 ExportPdf 会产出合法 PDF，却全是
// 另一份报告的内容 —— 只看魔数根本发现不了）。

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/domain/model/QcReport.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/qc/QcComparator.h"
#include "core/qc/QcReportFormat.h"
#include "core/reporting/QcReportExporter.h"
#include "infrastructure/serialization/Json.h"
#include "tests/support/PdfVerifier.h"

using videoeye::JsonParse;
using videoeye::JsonValue;
using videoeye::model::DiagnosticIssue;
using videoeye::model::IssueCategory;
using videoeye::model::IssueSeverity;
using videoeye::model::QcReport;
using videoeye::qc::QcCompareRow;
using videoeye::qc::QcComparison;
using videoeye::qc::QcFieldDiff;
using videoeye::qc::QcRunResult;
using videoeye::reporting::QcBatchSummaryInput;
using videoeye::reporting::QcBatchSummaryRow;
using videoeye::reporting::QcExportBundle;
using videoeye::reporting::QcReportExporter;

namespace {

// ---------------------------------------------------------------------------
// 脚手架
// ---------------------------------------------------------------------------

struct ScopedDir {
    std::filesystem::path path;
    explicit ScopedDir(const std::string& tag)
        : path(std::filesystem::temp_directory_path() /
               ("videoeye_dispatch_" + tag + "_" +
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

std::string ReadText(const std::string& path) {
    std::string out;
    videoeye_test::ReadWholeFile(path, out);
    return out;
}

// PDF 折行 / HTML 缩进 / JSON 空白都不影响「正文事实」比较。
std::string Compact(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out.push_back(c);
    }
    return out;
}

// 从 CSV 的 '#节,"键","值",...' 元数据行里直读一个值（不调产品解析器）。
// CsvField 把键和值都包了双引号，所以按这个固定形状找即可；值内不含引号
// （完整性标志只有 是/否/空串/一句错误原因），找不到一律返回 <missing>。
std::string MetaValue(const std::string& csv, const std::string& key) {
    const std::string needle = "\"" + key + "\",\"";
    const std::size_t pos = csv.find(needle);
    if (pos == std::string::npos)
        return "<missing>";
    const std::size_t begin = pos + needle.size();
    const std::size_t end = csv.find('"', begin);
    if (end == std::string::npos)
        return "<missing>";
    return csv.substr(begin, end - begin);
}

// ---------------------------------------------------------------------------
// 独立的内容格式识别器（只看字节，不看路径，也不调产品代码）
// ---------------------------------------------------------------------------

enum class Actual { Unknown, Json, Csv, Html, Text, Pdf };

const char* ActualName(Actual value) {
    switch (value) {
    case Actual::Unknown:
        return "unknown";
    case Actual::Json:
        return "json";
    case Actual::Csv:
        return "csv";
    case Actual::Html:
        return "html";
    case Actual::Text:
        return "txt";
    case Actual::Pdf:
        return "pdf";
    }
    return "unknown";
}

bool HasUtf8Bom(const std::string& bytes) {
    return bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
           static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF;
}

int NonEmptyLines(const std::string& bytes) {
    std::istringstream in(bytes);
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") != std::string::npos)
            ++count;
    }
    return count;
}

Actual DetectFormat(const std::string& bytes) {
    if (bytes.empty())
        return Actual::Unknown;
    if (bytes.rfind("%PDF-", 0) == 0)
        return Actual::Pdf;
    // 三个 CSV 实现都写 UTF-8 BOM（Excel 兼容），这是与纯文本最稳的分界。
    if (HasUtf8Bom(bytes))
        return Actual::Csv;
    const std::size_t first = bytes.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return Actual::Unknown;
    if (bytes[first] == '{')
        return Actual::Json;
    if (bytes.find("<!DOCTYPE html") != std::string::npos || bytes.find("<html") != std::string::npos)
        return Actual::Html;
    if (NonEmptyLines(bytes) < 3)
        return Actual::Unknown;
    return Actual::Text;
}

// 单份产物的完整校验：格式 / 结构 / 本家族特征串 / 不得串进别的家族正文。
// 返回空 = 通过；否则每条都是可直接读的失败原因。
std::vector<std::string> CheckArtifact(const std::string& path, Actual expected, const std::string& marker,
                                       const std::string& foreign_marker, const std::string& schema) {
    std::vector<std::string> errors;
    std::string bytes;
    if (!videoeye_test::ReadWholeFile(path, bytes)) {
        errors.push_back("产物文件不存在: " + path);
        return errors;
    }
    if (bytes.empty()) {
        errors.push_back("产物文件为空");
        return errors;
    }

    const Actual actual = DetectFormat(bytes);
    if (actual != expected)
        errors.push_back(std::string("实际内容格式 ") + ActualName(actual) + " 与期望 " + ActualName(expected) +
                         " 不符");

    std::string text = bytes;
    switch (expected) {
    case Actual::Json: {
        JsonValue root;
        std::string parse_error;
        if (!JsonParse(bytes, root, &parse_error))
            errors.push_back("JSON 无法解析: " + parse_error);
        else {
            const JsonValue* node = root.Find("schema");
            if (node == nullptr || !node->IsString() || node->StringValueOr() != schema)
                errors.push_back("JSON schema 不是本家族的 " + schema);
        }
        break;
    }
    case Actual::Csv: {
        std::istringstream in(bytes);
        std::string line;
        int comma_lines = 0;
        while (std::getline(in, line)) {
            if (line.find(',') != std::string::npos)
                ++comma_lines;
        }
        if (comma_lines < 2)
            errors.push_back("CSV 出现空表或无数据行（只有表头）");
        break;
    }
    case Actual::Html:
        if (bytes.find("</html>") == std::string::npos)
            errors.push_back("HTML 缺收尾标签，可能是截断或串了别的正文");
        break;
    case Actual::Text:
        if (NonEmptyLines(bytes) < 3)
            errors.push_back("TXT 正文不足 3 行");
        break;
    case Actual::Pdf: {
        const videoeye_test::PdfVerifyResult pdf = videoeye_test::VerifyPdfFile(path);
        if (!pdf.ok)
            errors.push_back("PDF 结构校验失败: " + pdf.error);
        if (pdf.text.empty())
            errors.push_back("PDF 没画出任何文字");
        text = pdf.text;
        break;
    }
    case Actual::Unknown:
        errors.push_back("期望格式不能是 Unknown");
        break;
    }

    if (text.find(marker) == std::string::npos)
        errors.push_back("缺本家族特征串 " + marker + "（可能落成了空壳或别的报告）");
    if (text.find(foreign_marker) != std::string::npos)
        errors.push_back("串进了别的家族正文 " + foreign_marker);
    return errors;
}

// ---------------------------------------------------------------------------
// 三个家族的数据与独立真值（特征串各家独有，互不出现）
// ---------------------------------------------------------------------------

enum class Family { Single, Comparison, Batch };

constexpr const char* kSingleMarker = "dispatch_single.mp4";
constexpr const char* kComparisonMarker = "dispatch_comparison_left.mp4";
constexpr const char* kBatchMarker = "dispatch_batch_row.mp4";

const char* MarkerOf(Family family) {
    switch (family) {
    case Family::Single:
        return kSingleMarker;
    case Family::Comparison:
        return kComparisonMarker;
    case Family::Batch:
        return kBatchMarker;
    }
    return "";
}

const char* ForeignMarkerOf(Family family) {
    switch (family) {
    case Family::Single:
        return kBatchMarker;
    case Family::Comparison:
        return kSingleMarker;
    case Family::Batch:
        return kComparisonMarker;
    }
    return "";
}

const char* SchemaOf(Family family) {
    switch (family) {
    case Family::Single:
        return "videoeye.qc-report";
    case Family::Comparison:
        return "videoeye.qc-comparison";
    case Family::Batch:
        return "videoeye.qc-batch-summary";
    }
    return "";
}

DiagnosticIssue MakeIssue(const std::string& rule_id) {
    DiagnosticIssue issue;
    issue.rule_id = rule_id;
    issue.severity = IssueSeverity::Warning;
    issue.category = IssueCategory::Container;
    issue.title = "分派守护问题";
    issue.detail = "实测 " + rule_id;
    issue.suggestion = "检查导出分派";
    issue.occurrence_count = 1;
    return issue;
}

QcReport MakeReport(bool completed, bool partial) {
    QcReport report;
    report.file_name = kSingleMarker;
    report.file_path = std::string("/media/") + kSingleMarker;
    report.container_format = "mp4";
    report.duration_seconds = 12.5;
    report.score = 71.0;
    report.verdict = "通过";
    report.generated_at = "2026-11-08 21:00:00";
    report.completed = completed;
    report.partial = partial;
    report.issues.push_back(MakeIssue("dispatch.single.rule_a"));
    report.issues.push_back(MakeIssue("dispatch.single.rule_b"));
    return report;
}

QcExportBundle MakeBundle() {
    QcExportBundle bundle;
    bundle.profile_id = "broadcast";
    bundle.profile_name = "广播级";
    bundle.run.ok = true;
    bundle.run.report = MakeReport(true, false);
    bundle.has_comparison = false;
    return bundle;
}

QcComparison MakeComparison() {
    QcComparison comparison;
    comparison.left_path = std::string("/media/") + kComparisonMarker;
    comparison.right_path = "/media/right.mp4";
    comparison.left_score = 70.0;
    comparison.right_score = 82.0;
    comparison.left_verdict = "通过";
    comparison.right_verdict = "通过";
    for (int i = 0; i < 2; ++i) {
        QcCompareRow row;
        row.group = "容器";
        row.field = "dispatch.compare.field_" + std::to_string(i);
        row.left = comparison.left_path;
        row.right = "h264";
        row.unit = "s";
        row.diff = QcFieldDiff::Different;
        row.numeric = true;
        row.left_value = 12.0;
        row.right_value = 13.0;
        row.delta = 1.0;
        comparison.rows.push_back(row);
    }
    return comparison;
}

QcBatchSummaryInput MakeSummary() {
    QcBatchSummaryInput input;
    input.root = "/media/batch";
    input.profile_id = "broadcast";
    input.profile_name = "广播级";
    input.succeeded = 2;
    input.failed = 1;
    input.timed_out = 1;
    input.cancelled = 0;
    input.skipped = 0;
    input.elapsed_ms = 1234.0;
    input.completed = false;
    for (int i = 0; i < 2; ++i) {
        QcBatchSummaryRow row;
        row.path = std::string("/media/") + kBatchMarker + "." + std::to_string(i);
        row.status = "succeeded";
        row.verdict = "通过";
        row.score = 80.0 + i;
        row.elapsed_ms = 100.0 + i;
        row.warning_count = i;
        row.output_path = "/out/report.json";
        input.rows.push_back(row);
    }
    return input;
}

bool DispatchAuto(Family family, const std::string& path) {
    switch (family) {
    case Family::Single:
        return QcReportExporter::ExportAuto(path, MakeBundle());
    case Family::Comparison:
        return QcReportExporter::ExportComparisonAuto(path, MakeComparison());
    case Family::Batch:
        return QcReportExporter::ExportBatchSummaryAuto(path, MakeSummary());
    }
    return false;
}

struct DispatchCase {
    Family family;
    Actual expected;
    const char* extension;
};

std::vector<DispatchCase> AllCases() {
    const Family families[] = {Family::Single, Family::Comparison, Family::Batch};
    const struct {
        Actual format;
        const char* extension;
    } formats[] = {{Actual::Json, ".json"},
                   {Actual::Csv, ".csv"},
                   {Actual::Html, ".html"},
                   {Actual::Text, ".txt"},
                   {Actual::Pdf, ".pdf"}};
    std::vector<DispatchCase> cases;
    for (const Family family : families) {
        for (const auto& format : formats)
            cases.push_back({family, format.format, format.extension});
    }
    return cases;
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

TEST(ReportDispatchTest, AutoDispatchMatchesExtensionForAllThreeFamilies) {
    ScopedDir dir("matrix");
    const std::vector<DispatchCase> cases = AllCases();
    int checked = 0;
    int per_family[3] = {0, 0, 0};
    int per_format[5] = {0, 0, 0, 0, 0};

    for (const DispatchCase& item : cases) {
        const std::string path = dir.File(std::string("dispatch_") + std::to_string(checked) + item.extension);
        ASSERT_TRUE(DispatchAuto(item.family, path)) << "分派返回失败: " << path;
        const std::vector<std::string> errors = CheckArtifact(path, item.expected, MarkerOf(item.family),
                                                              ForeignMarkerOf(item.family), SchemaOf(item.family));
        for (const std::string& error : errors)
            ADD_FAILURE() << path << ": " << error;
        ++checked;
        per_family[static_cast<int>(item.family)] += 1;
        per_format[static_cast<int>(item.expected) - 1] += 1;
    }

    // 覆盖守护：别让「筛到什么都没测」伪装成通过。
    EXPECT_EQ(checked, 15);
    for (const int count : per_family)
        EXPECT_EQ(count, 5);
    for (const int count : per_format)
        EXPECT_EQ(count, 3);
}

TEST(ReportDispatchTest, AutoDispatchRejectsUnknownExtensionsWithoutWriting) {
    ScopedDir dir("unknown");
    const Family families[] = {Family::Single, Family::Comparison, Family::Batch};
    const char* names[] = {"report.xyz", "report_no_extension"};
    int checked = 0;
    for (const Family family : families) {
        for (const char* name : names) {
            const std::string path = dir.File(std::string(name) + "_" + std::to_string(checked));
            EXPECT_FALSE(DispatchAuto(family, path)) << "未知扩展名不该被静默当成 JSON: " << path;
            EXPECT_FALSE(std::filesystem::exists(path)) << "拒绝后不该留下半成品文件: " << path;
            ++checked;
        }
    }
    EXPECT_EQ(checked, 6);
}

TEST(ReportDispatchTest, ExplicitFormatExportStillAllowsArbitraryFilename) {
    // 修 Auto 不能把「显式指定格式」这条合法路径一起封死：调用方自己决定了
    // 格式，路径叫什么由它负责，与按扩展名分派是两件事。
    ScopedDir dir("explicit");
    const std::string path = dir.File("hand_picked.xyz");
    ASSERT_TRUE(QcReportExporter::Export(path, MakeBundle(), videoeye::qc::QcReportFormat::Html));
    EXPECT_EQ(DetectFormat(ReadText(path)), Actual::Html);
    EXPECT_NE(ReadText(path).find(kSingleMarker), std::string::npos);
}

TEST(ReportDispatchTest, DetectorItselfHasDiscriminatingPower) {
    // 识别器自证：五种标准样本各自认对，错标内容会被认出原格式。
    struct Sample {
        Actual expected;
        std::string content;
    };
    const std::vector<Sample> samples = {
        {Actual::Pdf, "%PDF-1.4\n1 0 obj\n<<>>\nendobj\n%%EOF\n"},
        {Actual::Csv, std::string("\xEF\xBB\xBF") + "a,b\n1,2\n"},
        {Actual::Json, "{\"schema\":\"videoeye.qc-report\"}\n"},
        {Actual::Html, "<!DOCTYPE html>\n<html><body>x</body></html>\n"},
        {Actual::Text, "VideoEye 纯文本\n第二行\n第三行\n"},
        {Actual::Unknown, ""},
        {Actual::Unknown, "  \n\t\n"},
    };
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::string content = samples[i].content;
        EXPECT_EQ(DetectFormat(content), samples[i].expected) << "标准样本 " << i << " 识别错误";
    }
    // 把 PDF 正文写成 .txt 时，矩阵检查必须能看出「实际是 pdf」。
    EXPECT_EQ(DetectFormat(samples[0].content), Actual::Pdf);
    // 把 JSON 正文写成 .html 时同理。
    EXPECT_EQ(DetectFormat(samples[2].content), Actual::Json);
}

TEST(ReportDispatchTest, ArtifactGuardRejectsEmptyWrongFamilyAndMissingFiles) {
    ScopedDir dir("guard");
    const std::string marker = kSingleMarker;
    const std::string foreign = kBatchMarker;
    const std::string schema = SchemaOf(Family::Single);

    const std::string empty_path = dir.File("empty.json");
    { std::ofstream(empty_path, std::ios::binary) << ""; }
    EXPECT_FALSE(CheckArtifact(empty_path, Actual::Json, marker, foreign, schema).empty());

    const std::string no_marker = dir.File("no_marker.json");
    { std::ofstream(no_marker, std::ios::binary) << "{\"schema\":\"videoeye.qc-report\"}\n"; }
    EXPECT_FALSE(CheckArtifact(no_marker, Actual::Json, marker, foreign, schema).empty());

    const std::string wrong_family = dir.File("wrong_family.json");
    { std::ofstream(wrong_family, std::ios::binary) << "{\"schema\":\"videoeye.qc-report\"}\n" << marker << foreign; }
    EXPECT_FALSE(CheckArtifact(wrong_family, Actual::Json, marker, foreign, schema).empty());

    EXPECT_FALSE(CheckArtifact(dir.File("does_not_exist.json"), Actual::Json, marker, foreign, schema).empty());

    // 内容格式与期望不符也要判红（把纯文本塞进 .pdf 路径）。
    const std::string mislabeled = dir.File("mislabeled.pdf");
    { std::ofstream(mislabeled, std::ios::binary) << "VideoEye 纯文本\n第二行\n第三行\n" << marker; }
    EXPECT_FALSE(CheckArtifact(mislabeled, Actual::Pdf, marker, foreign, schema).empty());

    // 正例：真的产物必须一次通过，否则上面的「全判红」没有意义。
    const std::string good = dir.File("good.json");
    ASSERT_TRUE(QcReportExporter::ExportAuto(good, MakeBundle()));
    EXPECT_TRUE(CheckArtifact(good, Actual::Json, marker, foreign, schema).empty());
}

TEST(ReportDispatchTest, ConvenienceEntryKeepsIntegrityDimensionsIndependent) {
    // 裸 QcReport 没有运行上下文：分析成功恒为「是」，结论完整/抽样按报告自己的
    // 语义落，超时与错误原因保持空 —— 绝不把两个维度绑成一个、也不伪造错误文案。
    ScopedDir dir("convenience");
    const QcReport report = MakeReport(false, true); // 未完成 + 抽样
    ASSERT_FALSE(report.completed);
    ASSERT_TRUE(report.partial);

    const std::string json_path = dir.File("convenience.json");
    ASSERT_TRUE(QcReportExporter::ExportReport(json_path, report));
    JsonValue root;
    std::string parse_error;
    ASSERT_TRUE(JsonParse(ReadText(json_path), root, &parse_error)) << parse_error;
    const JsonValue* summary = root.Find("summary");
    ASSERT_NE(summary, nullptr);
    const JsonValue* integrity = summary->Find("integrity");
    ASSERT_NE(integrity, nullptr);
    ASSERT_NE(integrity->Find("analysis_ok"), nullptr);
    EXPECT_TRUE(integrity->Find("analysis_ok")->BoolValueOr()) << "分析成功不该被 report.completed 拖成否";
    ASSERT_NE(integrity->Find("conclusion_complete"), nullptr);
    EXPECT_FALSE(integrity->Find("conclusion_complete")->BoolValueOr());
    ASSERT_NE(integrity->Find("sampled"), nullptr);
    EXPECT_TRUE(integrity->Find("sampled")->BoolValueOr());
    ASSERT_NE(integrity->Find("timed_out"), nullptr);
    EXPECT_FALSE(integrity->Find("timed_out")->BoolValueOr());
    ASSERT_NE(integrity->Find("error"), nullptr);
    EXPECT_EQ(integrity->Find("error")->StringValueOr(), "");

    const std::string csv_path = dir.File("convenience.csv");
    ASSERT_TRUE(QcReportExporter::ExportReport(csv_path, report));
    const std::string csv = ReadText(csv_path);
    EXPECT_EQ(MetaValue(csv, "分析成功"), "是") << "分析成功不该被 report.completed 拖成否";
    EXPECT_EQ(MetaValue(csv, "结论完整"), "否");
    EXPECT_EQ(MetaValue(csv, "抽样扫描"), "是");
    EXPECT_EQ(MetaValue(csv, "超时放弃"), "否");
    EXPECT_EQ(MetaValue(csv, "错误原因"), "") << "空串也是事实，必须恒落";
    EXPECT_EQ(csv.find("分析未完成"), std::string::npos) << "不该凭空造错误原因";

    const std::string txt_path = dir.File("convenience.txt");
    ASSERT_TRUE(QcReportExporter::ExportReport(txt_path, report));
    const std::string txt = ReadText(txt_path);
    EXPECT_NE(txt.find("完整性: 分析成功=是"), std::string::npos);
    EXPECT_NE(txt.find("结论完整=否"), std::string::npos);
    EXPECT_NE(txt.find("错误原因=[]"), std::string::npos);
    EXPECT_EQ(txt.find("错误: 分析未完成"), std::string::npos);
}

TEST(ReportDispatchTest, ConvenienceEntryWithRunResultKeepsAllFiveFacts) {
    ScopedDir dir("runentry");
    QcRunResult run;
    run.ok = false;
    run.timed_out = true;
    run.error = "后台超时放弃";
    run.profile_id = "broadcast";
    run.report = MakeReport(false, true);

    const std::string json_path = dir.File("run.json");
    ASSERT_TRUE(QcReportExporter::ExportReport(json_path, run));
    JsonValue root;
    std::string parse_error;
    ASSERT_TRUE(JsonParse(ReadText(json_path), root, &parse_error)) << parse_error;
    const JsonValue* summary = root.Find("summary");
    ASSERT_NE(summary, nullptr);
    const JsonValue* integrity = summary->Find("integrity");
    ASSERT_NE(integrity, nullptr);
    EXPECT_FALSE(integrity->Find("analysis_ok")->BoolValueOr());
    EXPECT_FALSE(integrity->Find("conclusion_complete")->BoolValueOr());
    EXPECT_TRUE(integrity->Find("sampled")->BoolValueOr());
    EXPECT_TRUE(integrity->Find("timed_out")->BoolValueOr());
    EXPECT_EQ(integrity->Find("error")->StringValueOr(), "后台超时放弃");

    const std::string csv_path = dir.File("run.csv");
    ASSERT_TRUE(QcReportExporter::ExportReport(csv_path, run));
    const std::string csv = ReadText(csv_path);
    EXPECT_EQ(MetaValue(csv, "分析成功"), "否");
    EXPECT_EQ(MetaValue(csv, "结论完整"), "否");
    EXPECT_EQ(MetaValue(csv, "抽样扫描"), "是");
    EXPECT_EQ(MetaValue(csv, "超时放弃"), "是");
    EXPECT_EQ(MetaValue(csv, "错误原因"), "后台超时放弃");
}

TEST(ReportDispatchTest, BatchSummaryTextAndPdfShareTheSameBody) {
    // 新补的 TXT / PDF 必须与汇总本身同源：PDF 不能另排一套只写表头的正文。
    ScopedDir dir("summary_body");
    const QcBatchSummaryInput input = MakeSummary();
    const std::string body = QcReportExporter::BuildBatchSummaryText(input);
    ASSERT_NE(body.find(kBatchMarker), std::string::npos);
    ASSERT_NE(body.find("完成: 2 / 失败: 1"), std::string::npos);

    const std::string pdf_path = dir.File("summary.pdf");
    ASSERT_TRUE(QcReportExporter::ExportBatchSummaryPdf(pdf_path, input).ok);
    const videoeye_test::PdfVerifyResult pdf = videoeye_test::VerifyPdfFile(pdf_path);
    ASSERT_TRUE(pdf.ok) << pdf.error;
    EXPECT_EQ(Compact(pdf.text), Compact(body)) << "PDF 正文与 TXT 正文漂移";

    const std::string txt_path = dir.File("summary.txt");
    ASSERT_TRUE(QcReportExporter::ExportBatchSummaryText(txt_path, input));
    EXPECT_EQ(Compact(ReadText(txt_path)), Compact(body));
}

} // namespace
