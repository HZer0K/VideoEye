#include <gtest/gtest.h>

// 五格式交叉一致性：同一份 QcExportBundle 导出成 JSON / CSV / HTML / TXT / PDF，
// 再各自解析回结构化事实，逐项比对条目、级别、计数与五个完整性标志。
//
// 为什么不能直接复用 test_qc_report_exporter.cpp 的产物矩阵：那里的 5 个
// TEST_P 实例每个只导出一种格式、按格式自查关键词，没有任何跨格式比较语句；
// 而 PdfBodyIsTheSameTextAsTheTxtReport 只钉死了 PDF/TXT 正文同源。同一份结论
// 换格式丢掉 run.ok / run.timed_out 这种事，两边都不会报错。
//
// 覆盖守护（防「没事实实时也一路绿」）：每种格式都必须真的解析出完整性块，
// Clean 场景必须解析出非空条目序列，7 场景 × 5 格式全部计入比对次数。

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/model/QcReport.h"
#include "core/qc/QcAnalyzeRequest.h"
#include "core/reporting/QcReportExporter.h"
#include "core/reporting/ReportIntegrityFacts.h"
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
using videoeye::reporting::detail::IntegrityFacts;

namespace {

// ---------------------------------------------------------------------------
// 场景与独立真值
// ---------------------------------------------------------------------------
enum class Scenario {
    Clean,                // 3 条命中，正常完成
    NoIssues,             // 真没问题
    Failed,               // 打开失败
    Cancelled,            // 用户取消
    TimedOut,             // 后台超时（timed_out=true）
    Sampled,              // 抽样扫描，仍算完整结论
    PartialAndIncomplete, // partial && !completed 同时成立（派生标签会吞掉一个）
};

const char* ScenarioName(Scenario s) {
    switch (s) {
    case Scenario::Clean:
        return "clean";
    case Scenario::NoIssues:
        return "no_issues";
    case Scenario::Failed:
        return "failed";
    case Scenario::Cancelled:
        return "cancelled";
    case Scenario::TimedOut:
        return "timed_out";
    case Scenario::Sampled:
        return "sampled";
    case Scenario::PartialAndIncomplete:
        return "partial_incomplete";
    }
    return "unknown";
}

struct Signature {
    std::vector<std::string> rule_ids;
    std::vector<std::string> severities; // 规范英文码，与 CSV / JSON 的 severity 一致
    int critical = 0;
    int error = 0;
    int warning = 0;
    int info = 0;
    IntegrityFacts integrity;
};

struct ScenarioData {
    std::string name;
    QcExportBundle bundle;
    Signature expected;
};

DiagnosticIssue MakeIssue(const std::string& rule_id, IssueSeverity severity, const std::string& title) {
    DiagnosticIssue issue;
    issue.rule_id = rule_id;
    issue.severity = severity;
    issue.category = IssueCategory::Video;
    issue.title = title;
    issue.detail = "实测 " + title;
    issue.suggestion = "建议 " + title;
    issue.occurrence_count = 1;
    return issue;
}

ScenarioData MakeScenario(Scenario scenario) {
    ScenarioData data;
    data.name = ScenarioName(scenario);
    QcRunResult& run = data.bundle.run;
    run.ok = true;
    run.elapsed_ms = 1234.0;
    run.report.file_name = "sample.mp4";
    run.report.file_path = "/tmp/sample.mp4";
    run.report.container_format = "mp4";
    run.report.duration_seconds = 12.5;
    run.report.score = 42.0;
    run.report.verdict = "不通过";
    run.report.generated_at = "2026-11-08 21:00:00";
    data.bundle.profile_id = "broadcast";
    data.bundle.profile_name = "广播级";

    Signature& sig = data.expected;
    sig.integrity = IntegrityFacts{true, true, false, false, ""};

    switch (scenario) {
    case Scenario::Clean:
        run.report.issues.push_back(MakeIssue("video.gop.max_seconds", IssueSeverity::Critical, "GOP 过长"));
        run.report.issues.push_back(MakeIssue("audio.loudness.range", IssueSeverity::Error, "响度超标"));
        run.report.issues.push_back(MakeIssue("container.missing_subtitle", IssueSeverity::Warning, "缺字幕"));
        sig.rule_ids = {"video.gop.max_seconds", "audio.loudness.range", "container.missing_subtitle"};
        sig.severities = {"critical", "error", "warning"};
        sig.critical = 1;
        sig.error = 1;
        sig.warning = 1;
        break;
    case Scenario::NoIssues:
        break;
    case Scenario::Failed:
        run.ok = false;
        run.report.completed = false;
        run.error = "打开失败";
        sig.integrity = IntegrityFacts{false, false, false, false, "打开失败"};
        break;
    case Scenario::Cancelled:
        run.ok = false;
        run.report.completed = false;
        run.error = "分析被取消";
        sig.integrity = IntegrityFacts{false, false, false, false, "分析被取消"};
        break;
    case Scenario::TimedOut:
        run.ok = false;
        run.report.completed = false;
        run.timed_out = true;
        run.error = "分析超时（后台线程无响应）";
        sig.integrity = IntegrityFacts{false, false, false, true, "分析超时（后台线程无响应）"};
        break;
    case Scenario::Sampled:
        run.report.partial = true;
        run.report.issues.push_back(MakeIssue("video.gop.max_seconds", IssueSeverity::Critical, "GOP 过长"));
        run.report.issues.push_back(MakeIssue("container.missing_subtitle", IssueSeverity::Info, "缺字幕"));
        sig.rule_ids = {"video.gop.max_seconds", "container.missing_subtitle"};
        sig.severities = {"critical", "info"};
        sig.critical = 1;
        sig.info = 1;
        sig.integrity = IntegrityFacts{true, true, true, false, ""};
        break;
    case Scenario::PartialAndIncomplete:
        run.ok = false;
        run.report.completed = false;
        run.report.partial = true;
        run.error = "扫描窗口耗尽";
        run.report.issues.push_back(MakeIssue("timing.dts.regression", IssueSeverity::Warning, "DTS 回退"));
        sig.rule_ids = {"timing.dts.regression"};
        sig.severities = {"warning"};
        sig.warning = 1;
        sig.integrity = IntegrityFacts{false, false, true, false, "扫描窗口耗尽"};
        break;
    }
    return data;
}

std::vector<Scenario> AllScenarios() {
    return {Scenario::Clean,
            Scenario::NoIssues,
            Scenario::Failed,
            Scenario::Cancelled,
            Scenario::TimedOut,
            Scenario::Sampled,
            Scenario::PartialAndIncomplete};
}

// ---------------------------------------------------------------------------
// 产物
// ---------------------------------------------------------------------------
enum class Format { Json, Csv, Html, Text, Pdf };

const char* FormatName(Format format) {
    switch (format) {
    case Format::Json:
        return "json";
    case Format::Csv:
        return "csv";
    case Format::Html:
        return "html";
    case Format::Text:
        return "txt";
    case Format::Pdf:
        return "pdf";
    }
    return "unknown";
}

std::vector<Format> AllFormats() {
    return {Format::Json, Format::Csv, Format::Html, Format::Text, Format::Pdf};
}

struct Artifacts {
    std::string json;
    std::string csv;
    std::string html;
    std::string txt;
    std::string pdf_path;
    std::string pdf_text;
    videoeye_test::PdfVerifyResult pdf_verify;
};

std::string ReadWhole(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool ExportAll(const std::filesystem::path& dir, const QcExportBundle& bundle, Artifacts* out, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto file = [&dir](const char* ext) { return (dir / ("report." + std::string(ext))).string(); };
    const std::string json_path = file("json");
    const std::string csv_path = file("csv");
    const std::string html_path = file("html");
    const std::string txt_path = file("txt");
    const std::string pdf_path = file("pdf");

    if (!QcReportExporter::ExportJson(json_path, bundle) || !QcReportExporter::ExportCsv(csv_path, bundle) ||
        !QcReportExporter::ExportHtml(html_path, bundle) || !QcReportExporter::ExportText(txt_path, bundle) ||
        !QcReportExporter::ExportPdf(pdf_path, bundle).ok) {
        *error = "导出失败: " + dir.string();
        return false;
    }
    out->json = ReadWhole(json_path);
    out->csv = ReadWhole(csv_path);
    out->html = ReadWhole(html_path);
    out->txt = ReadWhole(txt_path);
    out->pdf_path = pdf_path;
    out->pdf_verify = videoeye_test::VerifyPdfFile(pdf_path);
    if (!out->pdf_verify.ok) {
        *error = "PDF 结构校验失败: " + out->pdf_verify.error;
        return false;
    }
    out->pdf_text = out->pdf_verify.text;
    if (out->json.empty() || out->csv.empty() || out->html.empty() || out->txt.empty() || out->pdf_text.empty()) {
        *error = "有产物为空";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 解析小工具（纯 std，不依赖产品导出器）
// ---------------------------------------------------------------------------
std::string Compact(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            continue;
        out += c;
    }
    return out;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// 最小 RFC 4180 行切分：够还原导出器写出的 CsvField（双引号 + 内部双引号翻倍）。
std::vector<std::string> SplitCsvRow(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            cells.push_back(cell);
            cell.clear();
        } else {
            cell += c;
        }
    }
    cells.push_back(cell);
    return cells;
}

std::string HtmlUnescape(const std::string& text) {
    std::string out = text;
    const std::pair<const char*, const char*> pairs[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"}};
    for (const auto& pair : pairs) {
        const std::string from = pair.first;
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), pair.second);
            pos += std::strlen(pair.second);
        }
    }
    return out;
}

std::vector<std::string> ExtractHtmlCells(const std::string& row) {
    std::vector<std::string> cells;
    size_t pos = 0;
    while ((pos = row.find("<td>", pos)) != std::string::npos) {
        const size_t begin = pos + 4;
        const size_t end = row.find("</td>", begin);
        if (end == std::string::npos)
            break;
        cells.push_back(HtmlUnescape(row.substr(begin, end - begin)));
        pos = end + 5;
    }
    return cells;
}

const char* SeverityCodeFromChinese(const std::string& text) {
    if (text == "致命")
        return "critical";
    if (text == "错误")
        return "error";
    if (text == "警告")
        return "warning";
    if (text == "提示")
        return "info";
    return "";
}

// 从「去空白」文本里读一个 是/否 标志。中文占 3 字节。
bool ReadYesNo(const std::string& compact, const std::string& key, bool* out, std::string* error) {
    const size_t pos = compact.find(key + "=");
    if (pos == std::string::npos) {
        *error = "缺完整性标志 " + key;
        return false;
    }
    const std::string value = compact.substr(pos + key.size() + 1, 3);
    if (value == "是") {
        *out = true;
        return true;
    }
    if (value == "否") {
        *out = false;
        return true;
    }
    *error = "完整性标志 " + key + " 的值不是 是/否: " + value;
    return false;
}

bool ReadErrorReason(const std::string& compact, std::string* out, std::string* error) {
    const std::string marker = "错误原因=[";
    const size_t pos = compact.find(marker);
    if (pos == std::string::npos) {
        *error = "缺完整性标志 错误原因";
        return false;
    }
    const size_t begin = pos + marker.size();
    const size_t end = compact.find(']', begin);
    if (end == std::string::npos) {
        *error = "错误原因 没有结束界定符";
        return false;
    }
    *out = compact.substr(begin, end - begin);
    return true;
}

// TXT / HTML / PDF 共用：完整性块是一行固定键名文本，PDF 折行后去空白即可复原。
bool ParseIntegrityFromCompact(const std::string& compact, IntegrityFacts* out, std::vector<std::string>* errors) {
    const size_t pos = compact.find("完整性:");
    if (pos == std::string::npos) {
        errors->push_back("正文里没有完整性块");
        return false;
    }
    const std::string window = compact.substr(pos, 512);
    std::string error;
    bool ok = true;
    ok &= ReadYesNo(window, "分析成功", &out->analysis_ok, &error);
    if (!error.empty()) {
        errors->push_back(error);
        error.clear();
    }
    ok &= ReadYesNo(window, "结论完整", &out->conclusion_complete, &error);
    if (!error.empty()) {
        errors->push_back(error);
        error.clear();
    }
    ok &= ReadYesNo(window, "抽样扫描", &out->sampled, &error);
    if (!error.empty()) {
        errors->push_back(error);
        error.clear();
    }
    ok &= ReadYesNo(window, "超时放弃", &out->timed_out, &error);
    if (!error.empty()) {
        errors->push_back(error);
        error.clear();
    }
    ok &= ReadErrorReason(window, &out->error, &error);
    if (!error.empty())
        errors->push_back(error);
    return ok;
}

bool ParseCsvIntegrity(const std::string& content, IntegrityFacts* out, std::vector<std::string>* errors) {
    bool found = false;
    for (const std::string& line : SplitLines(content)) {
        if (line.empty() || line[0] != '#')
            continue;
        const auto cells = SplitCsvRow(line.substr(1));
        if (cells.empty() || cells[0] != "结论")
            continue;
        found = true;
        for (size_t i = 1; i + 1 < cells.size(); i += 2) {
            const std::string& key = cells[i];
            const std::string& value = cells[i + 1];
            bool* slot = nullptr;
            if (key == "分析成功")
                slot = &out->analysis_ok;
            else if (key == "结论完整")
                slot = &out->conclusion_complete;
            else if (key == "抽样扫描")
                slot = &out->sampled;
            else if (key == "超时放弃")
                slot = &out->timed_out;
            else if (key == "错误原因")
                out->error = value;
            if (slot != nullptr) {
                if (value == "是")
                    *slot = true;
                else if (value == "否")
                    *slot = false;
                else
                    errors->push_back("CSV 结论/" + key + " 不是 是/否: " + value);
            }
        }
    }
    if (!found)
        errors->push_back("CSV 没有 #结论 元数据段");
    return found && errors->empty();
}

bool ParseJsonIntegrity(const std::string& content, IntegrityFacts* out, std::vector<std::string>* errors) {
    JsonValue root;
    std::string parse_error;
    if (!JsonParse(content, root, &parse_error)) {
        errors->push_back("JSON 无法解析: " + parse_error);
        return false;
    }
    const JsonValue* summary = root.Find("summary");
    const JsonValue* node = summary != nullptr ? summary->Find("integrity") : nullptr;
    if (node == nullptr || !node->IsObject()) {
        errors->push_back("JSON summary.integrity 缺失");
        return false;
    }
    const char* keys[] = {"analysis_ok", "conclusion_complete", "sampled", "timed_out"};
    bool* slots[] = {&out->analysis_ok, &out->conclusion_complete, &out->sampled, &out->timed_out};
    for (int i = 0; i < 4; ++i) {
        const JsonValue* value = node->Find(keys[i]);
        if (value == nullptr || !value->IsBool()) {
            errors->push_back(std::string("JSON integrity.") + keys[i] + " 缺失或不是布尔");
            continue;
        }
        *slots[i] = value->BoolValueOr();
    }
    const JsonValue* error = node->Find("error");
    if (error == nullptr || !error->IsString())
        errors->push_back("JSON integrity.error 缺失（空串也是事实，必须恒落）");
    else
        out->error = error->StringValueOr();
    return errors->empty();
}

// ---------------------------------------------------------------------------
// 逐格式解析成同一份事实
// ---------------------------------------------------------------------------
struct ParsedFacts {
    std::vector<std::string> rule_ids;
    std::vector<std::string> severities;
    int critical = -1;
    int error = -1;
    int warning = -1;
    int info = -1;
    IntegrityFacts integrity;
    bool integrity_found = false;
    std::vector<std::string> errors;
};

bool ParseCountsQuad(const std::string& text, ParsedFacts* out) {
    // 文本形如 "致命 1 / 错误 1 / 警告 1 / 提示 0"，或紧凑后的 "致命1/错误1/警告1/提示0"。
    const std::string keys[] = {"致命", "错误", "警告", "提示"};
    int* slots[] = {&out->critical, &out->error, &out->warning, &out->info};
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        const size_t pos = text.find(keys[i]);
        if (pos == std::string::npos) {
            ok = false;
            continue;
        }
        size_t begin = pos + keys[i].size();
        while (begin < text.size() && text[begin] == ' ')
            ++begin;
        size_t end = begin;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
            ++end;
        if (end == begin) {
            ok = false;
            continue;
        }
        *slots[i] = std::stoi(text.substr(begin, end - begin));
    }
    return ok;
}

ParsedFacts ParseJsonFacts(const std::string& content) {
    ParsedFacts out;
    JsonValue root;
    std::string parse_error;
    if (!JsonParse(content, root, &parse_error)) {
        out.errors.push_back("JSON 无法解析: " + parse_error);
        return out;
    }
    const JsonValue* issues = root.Find("issues");
    if (issues == nullptr || !issues->IsArray()) {
        out.errors.push_back("JSON issues 缺失");
    } else {
        for (const JsonValue& issue : issues->Items()) {
            const JsonValue* rule = issue.Find("rule_id");
            const JsonValue* sev = issue.Find("severity");
            if (rule == nullptr || sev == nullptr) {
                out.errors.push_back("JSON issue 缺 rule_id/severity");
                continue;
            }
            out.rule_ids.push_back(rule->StringValueOr());
            out.severities.push_back(sev->StringValueOr());
        }
    }
    const JsonValue* summary = root.Find("summary");
    const JsonValue* counts = summary != nullptr ? summary->Find("issue_counts") : nullptr;
    if (counts == nullptr) {
        out.errors.push_back("JSON summary.issue_counts 缺失");
    } else {
        const char* keys[] = {"critical", "error", "warning", "info"};
        int* slots[] = {&out.critical, &out.error, &out.warning, &out.info};
        for (int i = 0; i < 4; ++i) {
            const JsonValue* value = counts->Find(keys[i]);
            if (value == nullptr || !value->IsNumber())
                out.errors.push_back(std::string("JSON issue_counts.") + keys[i] + " 缺失");
            else
                *slots[i] = value->IntValueOr(-1);
        }
    }
    out.integrity_found = ParseJsonIntegrity(content, &out.integrity, &out.errors);
    return out;
}

ParsedFacts ParseCsvFacts(const std::string& content) {
    ParsedFacts out;
    std::string content_no_bom = content;
    if (content_no_bom.size() >= 3 && static_cast<unsigned char>(content_no_bom[0]) == 0xEF)
        content_no_bom.erase(0, 3);
    bool in_data = false;
    for (const std::string& line : SplitLines(content_no_bom)) {
        if (line.empty())
            continue;
        if (line[0] == '#') {
            const auto cells = SplitCsvRow(line.substr(1));
            if (!cells.empty() && cells[0] == "统计") {
                for (size_t i = 1; i + 1 < cells.size(); i += 2) {
                    const std::string& key = cells[i];
                    const std::string& value = cells[i + 1];
                    if (key == "致命")
                        out.critical = std::atoi(value.c_str());
                    else if (key == "错误")
                        out.error = std::atoi(value.c_str());
                    else if (key == "警告")
                        out.warning = std::atoi(value.c_str());
                    else if (key == "提示")
                        out.info = std::atoi(value.c_str());
                }
            }
            continue;
        }
        const auto cells = SplitCsvRow(line);
        if (!in_data) {
            if (cells.size() >= 2 && cells[0] == "文件" && cells[1] == "规则ID")
                in_data = true;
            continue;
        }
        if (cells.size() < 3) {
            out.errors.push_back("CSV 数据行列数不足 12");
            continue;
        }
        out.rule_ids.push_back(cells[1]);
        out.severities.push_back(cells[2]);
    }
    if (!in_data)
        out.errors.push_back("CSV 没有数据表头");
    out.integrity_found = ParseCsvIntegrity(content_no_bom, &out.integrity, &out.errors);
    return out;
}

ParsedFacts ParseHtmlFacts(const std::string& content) {
    ParsedFacts out;
    const std::string compact = Compact(content);
    for (const std::string& line : SplitLines(content)) {
        if (line.find("<tr class=\"") != std::string::npos && line.find("<td>") != std::string::npos) {
            const auto cells = ExtractHtmlCells(line);
            if (cells.size() == 7) {
                const std::string code = SeverityCodeFromChinese(cells[0]);
                if (code.empty())
                    out.errors.push_back("HTML 问题行的严重度无法识别: " + cells[0]);
                out.severities.push_back(code);
                out.rule_ids.push_back(cells[2]);
            }
        }
        if (line.find("致命 / 错误 / 警告 / 提示") != std::string::npos) {
            const auto cells = ExtractHtmlCells(line);
            // 值行是 "1 / 1 / 1 / 0"，没有中文键名，按 "/" 切成四个数。
            std::vector<int> values;
            if (cells.size() >= 2) {
                std::istringstream values_in(cells[1]);
                std::string token;
                while (std::getline(values_in, token, '/')) {
                    const std::string trimmed = Compact(token);
                    if (!trimmed.empty() && std::all_of(trimmed.begin(), trimmed.end(),
                                                        [](unsigned char c) { return std::isdigit(c) != 0; }))
                        values.push_back(std::stoi(trimmed));
                }
            }
            if (values.size() == 4) {
                out.critical = values[0];
                out.error = values[1];
                out.warning = values[2];
                out.info = values[3];
            } else {
                out.errors.push_back("HTML 级别计数行无法解析: " +
                                     (cells.size() >= 2 ? cells[1] : std::string("<缺列>")));
            }
        }
    }
    out.integrity_found = ParseIntegrityFromCompact(compact, &out.integrity, &out.errors);
    return out;
}

ParsedFacts ParseTextFacts(const std::string& content) {
    ParsedFacts out;
    const std::string compact = Compact(content);
    for (const std::string& line : SplitLines(content)) {
        if (line.find("致命 ") == 0 && line.find(" / 错误 ") != std::string::npos) {
            if (!ParseCountsQuad(line, &out))
                out.errors.push_back("TXT 级别计数行无法解析");
        }
        if (line.size() > 3 && line[0] == '[' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            const size_t close = line.find(']');
            const size_t paren = line.rfind('(');
            const size_t paren_end = line.rfind(')');
            if (close == std::string::npos || paren == std::string::npos || paren_end <= paren) {
                out.errors.push_back("TXT 问题行缺 rule_id: " + line);
                continue;
            }
            const size_t slash = line.find(" / ", close + 1);
            const std::string sev_text = line.substr(close + 2, slash == std::string::npos ? 0 : slash - close - 2);
            const std::string code = SeverityCodeFromChinese(sev_text);
            if (code.empty())
                out.errors.push_back("TXT 问题行的严重度无法识别: " + sev_text);
            out.severities.push_back(code);
            out.rule_ids.push_back(line.substr(paren + 1, paren_end - paren - 1));
        }
    }
    out.integrity_found = ParseIntegrityFromCompact(compact, &out.integrity, &out.errors);
    return out;
}

// PDF 正文 ≡ TXT 正文（去空白逐字全等由既有用例守护），这里在紧凑文本里
// 逐项验证完整性块、条目顺序与级别计数。期望串由测试自己拼，不调产品函数，
// 免得「产品把键写漏了，期望串也跟着漏」。
ParsedFacts ParsePdfFacts(const std::string& pdf_text, const Signature& expected) {
    ParsedFacts out;
    const std::string compact = Compact(pdf_text);
    out.integrity_found = ParseIntegrityFromCompact(compact, &out.integrity, &out.errors);

    const std::string expected_line =
        std::string("完整性:") + "分析成功=" + (expected.integrity.analysis_ok ? "是" : "否") +
        "｜结论完整=" + (expected.integrity.conclusion_complete ? "是" : "否") +
        "｜抽样扫描=" + (expected.integrity.sampled ? "是" : "否") +
        "｜超时放弃=" + (expected.integrity.timed_out ? "是" : "否") + "｜错误原因=[" + expected.integrity.error + "]";
    if (compact.find(expected_line) == std::string::npos)
        out.errors.push_back("PDF 正文里的完整性块与独立真值不一致（期望 " + expected_line + "）");

    size_t cursor = 0;
    for (size_t i = 0; i < expected.rule_ids.size(); ++i) {
        const std::string marker = "[" + std::to_string(i + 1) + "]";
        const size_t pos = compact.find(marker, cursor);
        if (pos == std::string::npos) {
            out.errors.push_back("PDF 缺第 " + std::to_string(i + 1) + " 条问题");
            continue;
        }
        const size_t end = compact.find(')', pos);
        const size_t paren = compact.rfind('(', end == std::string::npos ? compact.size() : end);
        if (end == std::string::npos || paren == std::string::npos || paren <= pos ||
            compact.substr(paren + 1, end - paren - 1) != expected.rule_ids[i]) {
            out.errors.push_back("PDF 第 " + std::to_string(i + 1) + " 条问题的 rule_id 不符");
        } else {
            const std::string sev_marker = marker + (expected.severities[i] == "critical"  ? "致命/"
                                                     : expected.severities[i] == "error"   ? "错误/"
                                                     : expected.severities[i] == "warning" ? "警告/"
                                                                                           : "提示/");
            if (compact.find(sev_marker, pos) != pos)
                out.errors.push_back("PDF 第 " + std::to_string(i + 1) + " 条问题的严重度不符");
            out.rule_ids.push_back(expected.rule_ids[i]);
            out.severities.push_back(expected.severities[i]);
        }
        cursor = pos + marker.size();
    }
    const std::string expected_counts = "致命" + std::to_string(expected.critical) + "/错误" +
                                        std::to_string(expected.error) + "/警告" + std::to_string(expected.warning) +
                                        "/提示" + std::to_string(expected.info);
    if (compact.find(expected_counts) == std::string::npos)
        out.errors.push_back("PDF 级别计数与独立真值不一致（期望 " + expected_counts + "）");
    else {
        out.critical = expected.critical;
        out.error = expected.error;
        out.warning = expected.warning;
        out.info = expected.info;
    }
    return out;
}

ParsedFacts ParseFormat(Format format, const Artifacts& artifacts, const Signature& expected) {
    switch (format) {
    case Format::Json:
        return ParseJsonFacts(artifacts.json);
    case Format::Csv:
        return ParseCsvFacts(artifacts.csv);
    case Format::Html:
        return ParseHtmlFacts(artifacts.html);
    case Format::Text:
        return ParseTextFacts(artifacts.txt);
    case Format::Pdf:
        return ParsePdfFacts(artifacts.pdf_text, expected);
    }
    return ParsedFacts{};
}

// ---------------------------------------------------------------------------
// 比对（反向守护也走这里，保证「判据坏了看不出来」会被发现）
// ---------------------------------------------------------------------------
std::vector<std::string> CompareFacts(const ParsedFacts& parsed, const Signature& expected) {
    std::vector<std::string> diffs;
    for (const std::string& error : parsed.errors)
        diffs.push_back("解析错误: " + error);
    if (!parsed.integrity_found)
        diffs.push_back("没解析到完整性块（空事实不得算一致）");
    if (parsed.rule_ids != expected.rule_ids)
        diffs.push_back("rule_ids 序列不一致");
    if (parsed.severities != expected.severities)
        diffs.push_back("severities 序列不一致");
    if (parsed.critical != expected.critical || parsed.error != expected.error || parsed.warning != expected.warning ||
        parsed.info != expected.info)
        diffs.push_back("级别计数不一致");
    const IntegrityFacts& a = parsed.integrity;
    const IntegrityFacts& b = expected.integrity;
    if (a.analysis_ok != b.analysis_ok)
        diffs.push_back("分析成功 不一致");
    if (a.conclusion_complete != b.conclusion_complete)
        diffs.push_back("结论完整 不一致");
    if (a.sampled != b.sampled)
        diffs.push_back("抽样扫描 不一致");
    if (a.timed_out != b.timed_out)
        diffs.push_back("超时放弃 不一致");
    if (a.error != b.error)
        diffs.push_back("错误原因 不一致");
    return diffs;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines)
        out += "\n  - " + line;
    return out;
}

std::filesystem::path WorkRoot() {
    return std::filesystem::temp_directory_path() / "videoeye_report_consistency";
}

} // namespace

// ---------------------------------------------------------------------------
// 1) 主用例：7 种结论态 × 5 种格式，全部解析回同一份事实
// ---------------------------------------------------------------------------
TEST(ReportFormatConsistency, AllFiveFormatsCarryTheSameFactsForEveryScenario) {
    std::error_code ec;
    std::filesystem::remove_all(WorkRoot(), ec);
    size_t comparisons = 0;
    size_t parsed_with_integrity = 0;
    std::vector<std::string> failures;

    for (Scenario scenario : AllScenarios()) {
        const ScenarioData data = MakeScenario(scenario);
        const std::filesystem::path dir = WorkRoot() / data.name;
        Artifacts artifacts;
        std::string error;
        ASSERT_TRUE(ExportAll(dir, data.bundle, &artifacts, &error)) << error;

        for (Format format : AllFormats()) {
            const ParsedFacts parsed = ParseFormat(format, artifacts, data.expected);
            const auto diffs = CompareFacts(parsed, data.expected);
            ++comparisons;
            if (parsed.integrity_found)
                ++parsed_with_integrity;
            if (!diffs.empty())
                failures.push_back(data.name + "/" + FormatName(format) + ":" + Join(diffs));
            // 覆盖守护：Clean 场景必须真的解析出 3 条条目，不能靠空事实一路绿。
            if (scenario == Scenario::Clean && parsed.rule_ids.size() != 3)
                failures.push_back(data.name + "/" + FormatName(format) + ": Clean 场景没解析出 3 条条目");
        }
    }

    EXPECT_TRUE(failures.empty()) << Join(failures);
    EXPECT_EQ(comparisons, 35u) << "场景 × 格式 的比对次数必须钉死";
    EXPECT_EQ(parsed_with_integrity, comparisons) << "每份产物都必须真的解析出完整性块";
    std::filesystem::remove_all(WorkRoot(), ec);
}

// ---------------------------------------------------------------------------
// 2) 空报告专项：五种「没命中」的结论态在每种格式里都必须可两两区分
//    （修复前 JSON / HTML / TXT / PDF 无法区分「超时」与「被取消」，
//     而且 partial && !completed 会被派生标签吞掉一个）
// ---------------------------------------------------------------------------
TEST(ReportFormatConsistency, EmptyReportsStayDistinguishableAcrossAllFormats) {
    const std::vector<Scenario> states = {Scenario::NoIssues, Scenario::Failed,  Scenario::Cancelled,
                                          Scenario::TimedOut, Scenario::Sampled, Scenario::PartialAndIncomplete};
    std::error_code ec;
    std::filesystem::remove_all(WorkRoot() / "distinguish", ec);
    size_t distinguishable_pairs = 0;
    std::vector<std::string> failures;

    for (Scenario scenario : states) {
        const ScenarioData data = MakeScenario(scenario);
        const std::filesystem::path dir = WorkRoot() / "distinguish" / data.name;
        Artifacts artifacts;
        std::string error;
        ASSERT_TRUE(ExportAll(dir, data.bundle, &artifacts, &error)) << error;
        for (Format format : AllFormats()) {
            const ParsedFacts parsed = ParseFormat(format, artifacts, data.expected);
            for (const std::string& diff : CompareFacts(parsed, data.expected))
                failures.push_back(data.name + "/" + FormatName(format) + ": " + diff);
            for (Scenario other : states) {
                if (other <= scenario)
                    continue;
                const Signature other_sig = MakeScenario(other).expected;
                if (parsed.integrity.analysis_ok == other_sig.integrity.analysis_ok &&
                    parsed.integrity.conclusion_complete == other_sig.integrity.conclusion_complete &&
                    parsed.integrity.sampled == other_sig.integrity.sampled &&
                    parsed.integrity.timed_out == other_sig.integrity.timed_out &&
                    parsed.integrity.error == other_sig.integrity.error) {
                    failures.push_back(std::string(FormatName(format)) + ": " + data.name + " 与 " +
                                       ScenarioName(other) + " 的完整性标志完全相同，无法区分");
                } else {
                    ++distinguishable_pairs;
                }
            }
        }
    }

    EXPECT_TRUE(failures.empty()) << Join(failures);
    EXPECT_EQ(distinguishable_pairs, 75u) << "6 态两两组合 15 对 × 5 格式，每对都必须可分";
    std::filesystem::remove_all(WorkRoot() / "distinguish", ec);
}

// ---------------------------------------------------------------------------
// 3) 反向守护：判据本身「坏了看不出来」最危险。空事实、缺完整性块、
//    条目被换序 / 少一条 / 级别计数被改，都必须精确报错。
// ---------------------------------------------------------------------------
TEST(ReportFormatConsistency, GuardsRejectMissingFactsAndMutatedConclusions) {
    const ScenarioData data = MakeScenario(Scenario::Clean);
    ParsedFacts empty;
    EXPECT_FALSE(CompareFacts(empty, data.expected).empty()) << "空事实必须判红";

    ParsedFacts good;
    good.rule_ids = data.expected.rule_ids;
    good.severities = data.expected.severities;
    good.critical = data.expected.critical;
    good.error = data.expected.error;
    good.warning = data.expected.warning;
    good.info = data.expected.info;
    good.integrity = data.expected.integrity;
    good.integrity_found = true;
    EXPECT_TRUE(CompareFacts(good, data.expected).empty()) << "完整事实必须通过";

    ParsedFacts no_integrity = good;
    no_integrity.integrity_found = false;
    EXPECT_FALSE(CompareFacts(no_integrity, data.expected).empty()) << "缺完整性块必须判红";

    ParsedFacts reordered = good;
    std::swap(reordered.rule_ids[0], reordered.rule_ids[2]);
    EXPECT_FALSE(CompareFacts(reordered, data.expected).empty()) << "条目换序必须判红";

    ParsedFacts dropped = good;
    dropped.rule_ids.pop_back();
    dropped.severities.pop_back();
    EXPECT_FALSE(CompareFacts(dropped, data.expected).empty()) << "少一条条目必须判红";

    ParsedFacts wrong_count = good;
    wrong_count.critical += 1;
    EXPECT_FALSE(CompareFacts(wrong_count, data.expected).empty()) << "级别计数被改必须判红";

    ParsedFacts wrong_timeout = good;
    wrong_timeout.integrity.timed_out = true;
    EXPECT_FALSE(CompareFacts(wrong_timeout, data.expected).empty()) << "超时标志被改必须判红";

    ParsedFacts wrong_error = good;
    wrong_error.integrity.error = "分析被取消";
    EXPECT_FALSE(CompareFacts(wrong_error, data.expected).empty()) << "错误原因被改必须判红";
}

// ---------------------------------------------------------------------------
// 4) 错误原因净化：全角竖线是完整性行的分隔符，必须换成半角；方括号换全角，
//    换行压空格，保证 "[...]" 仍能界定整段原因。旧写法把 U+FF5C 写成
//    c == '｜'（多字节字符常量），逐字节比较恒为假 —— 这条用例会精确判红。
// ---------------------------------------------------------------------------
TEST(ReportFormatConsistency, SanitizeErrorKeepsBracketDelimitersIntact) {
    using videoeye::reporting::detail::SanitizeIntegrityError;
    EXPECT_EQ(std::string("a|b［c］ d"), SanitizeIntegrityError("a｜b[c]\nd"));
    // 反向守护：只命中完整的 EF BD 9C 序列，普通中文 UTF-8 不能被误伤。
    EXPECT_EQ(std::string("中文原因保留"), SanitizeIntegrityError("中文原因保留"));
    EXPECT_EQ(std::string("x|y"), SanitizeIntegrityError("x|y"));
}
