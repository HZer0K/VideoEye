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
//
// ❗ 单引号必须落在**引号之内**。写在引号外面（先上 ' 再上 "）会让字段的第一个
// 字符变成 '，RFC 4180 解析器据此判定"这个字段没有加引号"，于是把后面那对引号
// 当成普通字符原样保留 —— 实测占位符 "-" 会被读成 "'-"，值里带引号时更乱。
std::string CsvField(const std::string& text) {
    std::string out = "\"";
    if (!text.empty()) {
        const char first = text.front();
        if (first == '=' || first == '+' || first == '-' || first == '@')
            out += '\'';
    }
    for (char c : text) {
        if (c == '"')
            out += "\"\"";
        else
            out += c; // 逗号/换行/CR 本来就落在引号里面，属于 RFC 4180 的合法转义
    }
    out += '"';
    return out;
}

} // namespace

// ===========================================================================
// CSV —— 元数据头（# 行）+ 每个 issue 一行
//
// 为什么必须留一段元数据头：这张表只有"命中的规则"才有行，于是**空表**既可能是
// "真没问题"，也可能是"打开失败 / 被取消 / 超时 / 只跑了半个文件"。数据区在这几种
// 情况下一模一样（只剩表头），换个格式（TXT / HTML / JSON）却都能说清楚 —— 同一份
// QC 结论换个导出格式就丢掉全部覆盖说明，正是「证据与覆盖」那套结构要消灭的假象。
//
// 约定（下游只需记这一条）：**行首为 '#' 的行是元数据，不属于数据表。**
//   * pandas：read_csv(..., comment='#') 直接拿到纯表；
//   * 自己写解析器：跳过首字符为 '#' 的行即可（元数据行的值都压过换行，物理行 = 记录行）；
//   * Excel 双击：多出几行以 '#' 开头的说明，不影响下面那张表。
//
// 行语法两种：
//   '#<节>,<键>,<值>,<键>,<值>,...'   单值事实（报告 / 结论 / 统计）
//   '#覆盖,维度,执行,能力,...'        覆盖表的列名行，紧随其后每个维度一行、列对齐
// ===========================================================================

namespace {

// 把"#节 + 若干键值对"拼成一行。值一律走 CsvField（含公式注入中和与引号转义），
// 只有已经格式化好的数字例外 —— 否则负数会被前置单引号，"'-18.5" 读回来就不是数了。
struct MetaRow {
    std::ostringstream& out;
    MetaRow(std::ostringstream& stream, const char* section) : out(stream) {
        out << '#' << section;
    }
    MetaRow& Text(const char* key, const std::string& value) {
        out << ',' << CsvField(key) << ',' << CsvField(SingleLine(value));
        return *this;
    }
    MetaRow& Number(const char* key, const std::string& value) {
        out << ',' << CsvField(key) << ',' << value;
        return *this;
    }
    MetaRow& Flag(const char* key, bool value) {
        return Text(key, value ? "是" : "否");
    }
};

void AppendMetaHeader(std::ostringstream& body, const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    // 这一行是给"双击打开"的人看的说明，不含半角逗号，整行落在一个单元格里。
    body << "#VideoEye QC 报告（CSV）｜ 以 # 开头的行是元数据，读表时用 comment='#' 跳过即可；"
            "表头之后每行一个问题\n";

    MetaRow facts(body, "报告");
    facts.Text("文件", report.file_name)
        .Text("路径", report.file_path)
        .Text("模板",
              bundle.profile_name.empty() ? bundle.profile_id : bundle.profile_name + " (" + bundle.profile_id + ")")
        .Text("生成时间", report.generated_at)
        .Number("分析耗时(ms)", Fixed(run.elapsed_ms, 0));
    body << "\n";

    // 五个完整性标志分开落，不合成一个"完整性=完整/抽样/未完成"的派生值：
    // 合成之后"打开失败"与"用户取消"会挤进同一个词，而两者的处置方式完全不同。
    MetaRow verdict(body, "结论");
    verdict.Number("评分", Fixed(report.score, 1))
        .Text("结论", report.verdict)
        .Flag("分析成功", run.ok)           // 打开 + 分析是否正常返回
        .Flag("结论完整", report.completed) // 是否覆盖完整扫描（取消 / 失败为否）
        .Flag("抽样扫描", report.partial)   // 命中包数上限，只看了部分数据
        .Flag("超时放弃", run.timed_out)
        .Text("错误原因", run.error);
    body << "\n";

    MetaRow counts(body, "统计");
    counts.Number("问题数", std::to_string(report.issues.size()))
        .Number("致命", std::to_string(report.CountBySeverity(model::IssueSeverity::Critical)))
        .Number("错误", std::to_string(report.CountBySeverity(model::IssueSeverity::Error)))
        .Number("警告", std::to_string(report.CountBySeverity(model::IssueSeverity::Warning)))
        .Number("提示", std::to_string(report.CountBySeverity(model::IssueSeverity::Info)))
        .Number("流数", std::to_string(run.analysis.streams.size()))
        .Number("时长(s)", Fixed(report.duration_seconds, 3))
        .Number("大小(字节)", std::to_string(report.file_size_bytes));
    // 规则快照的正文不进 CSV（一张表塞不下），但"几条规则在跑"必须留下：
    // 0 条启用 与 "12 条全跑过且都没命中" 在空表上长得一样。
    int enabled_rules = 0;
    for (const model::QcRule& rule : report.rules) {
        if (rule.enabled)
            ++enabled_rules;
    }
    counts.Number("规则", std::to_string(report.rules.size())).Number("启用规则", std::to_string(enabled_rules));
    body << "\n";
}

// 逐维度覆盖：与 TXT / HTML 的「证据与覆盖」同一份口径，只是换成列。
//
// 没有这一节，空表读者只能猜"黑帧检测是不是压根没跑"。列名行单独给一行，
// 维度多的时候下游不必记列序。
void AppendMetaCoverage(std::ostringstream& body, const model::AnalysisResult& analysis) {
    if (analysis.dimensions.empty())
        return;
    body << "#覆盖";
    for (const char* column :
         {"维度", "执行", "能力", "结论可用", "处理对象", "已扫", "总量", "覆盖率(%)", "失败", "限制"})
        body << ',' << CsvField(column);
    body << "\n";
    for (const model::DimensionStatus& status : analysis.dimensions) {
        std::string remark = status.failure_reason;
        for (const std::string& note : status.notes) {
            if (!remark.empty())
                remark += "；";
            remark += note;
        }
        body << "#覆盖," << CsvField(model::DimensionName(status.dimension)) << ','
             << CsvField(status.executed ? "已执行" : "未执行") << ','
             << CsvField(model::CapabilityToString(status.capability)) << ','
             << CsvField(status.conclusive ? "是" : "否")
             << ','
             // 占位符 "-" 不走 CsvField：它会被当成"以 - 开头的字段"加公式前缀，
             // 读回来变成 "'-"，与"没有这一列"看起来一样却查不到。
             << (status.scope_unit.empty() ? std::string("-") : CsvField(status.scope_unit)) << ','
             << status.units_scanned
             << ','
             // 总量未知（裸流算不出总帧数）落 "-"，不能落 0 —— 0 会被读成"覆盖 0%"
             << (status.has_coverage ? std::to_string(status.units_total) : std::string("-")) << ','
             << (status.has_coverage ? Fixed(status.CoveragePercent(), 1) : std::string("-")) << ',' << status.failures
             << ',' << CsvField(SingleLine(remark)) << "\n";
    }
}

} // namespace

bool QcReportExporter::ExportCsv(const std::string& path, const QcExportBundle& bundle) {
    const model::QcReport& report = bundle.run.report;

    std::ostringstream body;
    body << "\xEF\xBB\xBF"; // UTF-8 BOM，Excel 直接双击打开才不会乱码
    AppendMetaHeader(body, bundle);
    AppendMetaCoverage(body, bundle.run.analysis);
    body << "文件,规则ID,严重度,类别,问题,位置,流序号,实测值,阈值,出现次数,说明,建议\n";
    for (const auto& issue : report.issues) {
        body << CsvField(report.file_name) << "," << CsvField(issue.rule_id) << ","
             << CsvField(SeverityCode(issue.severity)) << "," << CsvField(CategoryCode(issue.category)) << ","
             << CsvField(issue.title) << "," << CsvField(issue.range.ToString()) << "," << issue.stream_index << ","
             << Fixed(issue.metric_value, 4) << "," << Fixed(issue.threshold, 4) << "," << issue.occurrence_count << ","
             << CsvField(SingleLine(issue.detail)) << "," << CsvField(SingleLine(issue.suggestion)) << "\n";
    }
    return WriteUtf8File(path, body.str());
}

bool QcReportExporter::ExportComparisonCsv(const std::string& path, const qc::QcComparison& comparison) {
    std::ostringstream body;
    body << "\xEF\xBB\xBF";
    body << "分组,字段,原文件,对比文件,单位,是否一致,差值\n";
    for (const auto& row : comparison.rows) {
        const char* diff_text = "same";
        switch (row.diff) {
        case qc::QcFieldDiff::Same:
            diff_text = "same";
            break;
        case qc::QcFieldDiff::Different:
            diff_text = "different";
            break;
        case qc::QcFieldDiff::OnlyLeft:
            diff_text = "only_left";
            break;
        case qc::QcFieldDiff::OnlyRight:
            diff_text = "only_right";
            break;
        case qc::QcFieldDiff::Unavailable:
            diff_text = "unavailable";
            break;
        }
        body << CsvField(row.group) << "," << CsvField(row.field) << "," << CsvField(row.left) << ","
             << CsvField(row.right) << "," << CsvField(row.unit) << "," << diff_text << ",";
        if (row.numeric)
            body << Fixed(row.delta, 4);
        body << "\n";
    }
    return WriteUtf8File(path, body.str());
}

bool QcReportExporter::ExportBatchSummaryCsv(const std::string& path, const QcBatchSummaryInput& input) {
    std::ostringstream body;
    body << "\xEF\xBB\xBF";
    body << "文件,状态,评分,结论,致命,错误,警告,提示,耗时(ms),输出路径,错误\n";
    for (const auto& row : input.rows) {
        body << CsvField(row.path) << "," << CsvField(row.status) << "," << Fixed(row.score, 1) << ","
             << CsvField(row.verdict) << "," << row.critical_count << "," << row.error_count << "," << row.warning_count
             << "," << row.info_count << "," << Fixed(row.elapsed_ms, 0) << "," << CsvField(row.output_path) << ","
             << CsvField(SingleLine(row.error)) << "\n";
    }
    return WriteUtf8File(path, body.str());
}

} // namespace reporting
} // namespace videoeye
