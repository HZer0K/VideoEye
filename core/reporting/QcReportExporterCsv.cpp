#include "core/reporting/QcReportExporter.h"

#include "core/reporting/QcReportExporterInternal.h"

#include <sstream>

namespace videoeye {
namespace reporting {

using detail::Fixed;
using detail::SingleLine;
using detail::WriteUtf8File;

namespace {

// CSV 转义（RFC 4180）：字段整体用双引号包住，内部的双引号翻倍。
//
// 另外对以 = + - @ 开头的字段前置一个单引号：Excel / LibreOffice / Google Sheets
// 会把这样的单元格当成公式执行（=cmd|'/c ...、@SUM(...) 等），导出的报告会被
// 打开它的表格软件执行的载荷，前置 ' 之后它只是普通文本。
std::string CsvField(const std::string& text) {
    std::string out;
    if (!text.empty()) {
        const char first = text.front();
        if (first == '=' || first == '+' || first == '-' || first == '@')
            out += '\'';
    }
    out += '"';
    for (char c : text) {
        if (c == '"') out += "\"\"";
        else out += c;   // 逗号/换行/CR 本来就落在引号里面，属于 RFC 4180 的合法转义
    }
    out += '"';
    return out;
}

}  // namespace

// ===========================================================================
// CSV —— 每个 issue 一行
// ===========================================================================

bool QcReportExporter::ExportCsv(const std::string& path, const QcExportBundle& bundle) {
    const model::QcReport& report = bundle.run.report;

    std::ostringstream body;
    body << "\xEF\xBB\xBF";  // UTF-8 BOM，Excel 直接双击打开才不会乱码
    body << "文件,规则ID,严重度,类别,问题,位置,流序号,实测值,阈值,出现次数,说明,建议\n";
    for (const auto& issue : report.issues) {
        body << CsvField(report.file_name) << ","
             << CsvField(issue.rule_id) << ","
             << CsvField(SeverityCode(issue.severity)) << ","
             << CsvField(CategoryCode(issue.category)) << ","
             << CsvField(issue.title) << ","
             << CsvField(issue.range.ToString()) << ","
             << issue.stream_index << ","
             << Fixed(issue.metric_value, 4) << ","
             << Fixed(issue.threshold, 4) << ","
             << issue.occurrence_count << ","
             << CsvField(SingleLine(issue.detail)) << ","
             << CsvField(SingleLine(issue.suggestion)) << "\n";
    }
    return WriteUtf8File(path, body.str());
}

bool QcReportExporter::ExportComparisonCsv(const std::string& path,
                                          const qc::QcComparison& comparison) {
    std::ostringstream body;
    body << "\xEF\xBB\xBF";
    body << "分组,字段,原文件,对比文件,单位,是否一致,差值\n";
    for (const auto& row : comparison.rows) {
        const char* diff_text = "same";
        switch (row.diff) {
            case qc::QcFieldDiff::Same:        diff_text = "same";        break;
            case qc::QcFieldDiff::Different:   diff_text = "different";   break;
            case qc::QcFieldDiff::OnlyLeft:    diff_text = "only_left";   break;
            case qc::QcFieldDiff::OnlyRight:   diff_text = "only_right";  break;
            case qc::QcFieldDiff::Unavailable: diff_text = "unavailable"; break;
        }
        body << CsvField(row.group) << "," << CsvField(row.field) << "," << CsvField(row.left)
             << "," << CsvField(row.right) << "," << CsvField(row.unit) << "," << diff_text << ",";
        if (row.numeric) body << Fixed(row.delta, 4);
        body << "\n";
    }
    return WriteUtf8File(path, body.str());
}

bool QcReportExporter::ExportBatchSummaryCsv(const std::string& path,
                                            const QcBatchSummaryInput& input) {
    std::ostringstream body;
    body << "\xEF\xBB\xBF";
    body << "文件,状态,评分,结论,致命,错误,警告,提示,耗时(ms),输出路径,错误\n";
    for (const auto& row : input.rows) {
        body << CsvField(row.path) << "," << CsvField(row.status) << ","
             << Fixed(row.score, 1) << "," << CsvField(row.verdict) << ","
             << row.critical_count << "," << row.error_count << "," << row.warning_count << ","
             << row.info_count << "," << Fixed(row.elapsed_ms, 0) << ","
             << CsvField(row.output_path) << "," << CsvField(SingleLine(row.error)) << "\n";
    }
    return WriteUtf8File(path, body.str());
}

}  // namespace reporting
}  // namespace videoeye