#include "core/reporting/QcReportExporter.h"

#include "core/reporting/QcReportExporterInternal.h"

#include <sstream>

namespace videoeye {
namespace reporting {

using detail::Fixed;
using detail::Int64Text;
using detail::kReportSchemaVersion;
using detail::WriteUtf8File;

namespace {

std::string Html(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '\"': out += "&quot;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

}  // namespace

// ===========================================================================
// HTML
// ===========================================================================

std::string QcReportExporter::BuildHtml(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    auto severity_class = [](model::IssueSeverity severity) {
        switch (severity) {
            case model::IssueSeverity::Critical: return "critical";
            case model::IssueSeverity::Error:    return "error";
            case model::IssueSeverity::Warning:  return "warning";
            case model::IssueSeverity::Info:     return "info";
        }
        return "info";
    };

    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
        << "  <meta charset=\"UTF-8\">\n  <title>VideoEye QC 报告 - "
        << Html(report.file_name) << "</title>\n"
        << "  <style>\n"
        << "    body { font-family: 'Microsoft YaHei', Arial, sans-serif; margin: 24px; color: #23272e; }\n"
        << "    h1 { font-size: 22px; margin-bottom: 4px; }\n"
        << "    h2 { font-size: 17px; margin-top: 26px; border-left: 4px solid #2f6fed; padding-left: 8px; }\n"
        << "    table { border-collapse: collapse; width: 100%; margin-top: 10px; font-size: 13px; }\n"
        << "    th, td { border: 1px solid #d8dee6; padding: 6px 8px; text-align: left; vertical-align: top; }\n"
        << "    th { background: #f2f4f7; }\n"
        << "    .score { font-size: 30px; font-weight: bold; }\n"
        << "    .pass { color: #1b7f3b; } .warn { color: #b26a00; } .fail { color: #b3261e; }\n"
        << "    .critical td:first-child { color: #b3261e; font-weight: bold; }\n"
        << "    .error td:first-child { color: #b3261e; }\n"
        << "    .warning td:first-child { color: #b26a00; }\n"
        << "    .info td:first-child { color: #1565c0; }\n"
        << "    .meta { color: #6b7280; font-size: 12px; }\n"
        << "  </style>\n</head>\n<body>\n";

    const std::string verdict_class =
        (report.score >= 90.0) ? "pass" : ((report.score >= 70.0) ? "warn" : "fail");

    out << "  <h1>VideoEye QC 报告</h1>\n";
    out << "  <p class=\"meta\">模板: " << Html(bundle.profile_name) << " ("
        << Html(bundle.profile_id) << ") ｜ 生成时间: " << Html(report.generated_at)
        << " ｜ 分析耗时: " << Fixed(run.elapsed_ms, 0) << " ms"
        << (report.completed ? (report.partial ? " ｜ <b>抽样完成·非全量</b>" : "")
                             : " ｜ <b>分析未跑完</b>") << "</p>\n";

    out << "  <h2>总体结论</h2>\n";
    out << "  <p class=\"score " << verdict_class << "\">" << Fixed(report.score, 1)
        << " / 100 — " << Html(report.verdict) << "</p>\n";
    out << "  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
    out << "    <tr><td>文件</td><td>" << Html(report.file_path) << "</td></tr>\n";
    out << "    <tr><td>容器格式</td><td>" << Html(report.container_format) << "</td></tr>\n";
    out << "    <tr><td>时长</td><td>" << Fixed(report.duration_seconds, 3) << " s</td></tr>\n";
    out << "    <tr><td>文件大小</td><td>" << Int64Text(report.file_size_bytes) << " 字节</td></tr>\n";
    out << "    <tr><td>整体码率</td><td>" << Fixed(report.overall_bitrate_bps / 1000.0, 1)
        << " kbps</td></tr>\n";
    out << "    <tr><td>视频流 / 音频流</td><td>" << report.video_stream_count << " / "
        << report.audio_stream_count << "</td></tr>\n";
    out << "    <tr><td>致命 / 错误 / 警告 / 提示</td><td>"
        << report.CountBySeverity(model::IssueSeverity::Critical) << " / "
        << report.CountBySeverity(model::IssueSeverity::Error) << " / "
        << report.CountBySeverity(model::IssueSeverity::Warning) << " / "
        << report.CountBySeverity(model::IssueSeverity::Info) << "</td></tr>\n";
    if (!run.error.empty()) out << "    <tr><td>错误信息</td><td>" << Html(run.error) << "</td></tr>\n";
    out << "  </table>\n";

    out << "  <h2>问题清单</h2>\n";
    if (report.issues.empty()) {
        out << "  <p>未发现问题。</p>\n";
    } else {
        out << "  <table>\n    <tr><th>严重度</th><th>类别</th><th>规则</th><th>问题</th>"
               "<th>位置</th><th>说明</th><th>建议</th></tr>\n";
        for (const auto& issue : report.issues) {
            out << "    <tr class=\"" << severity_class(issue.severity) << "\">"
                << "<td>" << Html(issue.SeverityText()) << "</td>"
                << "<td>" << Html(issue.CategoryText()) << "</td>"
                << "<td>" << Html(issue.rule_id) << "</td>"
                << "<td>" << Html(issue.title) << "</td>"
                << "<td>" << Html(issue.range.ToString()) << "</td>"
                << "<td>" << Html(issue.detail) << "</td>"
                << "<td>" << Html(issue.suggestion) << "</td></tr>\n";
        }
        out << "  </table>\n";
    }

    out << "  <h2>流列表</h2>\n";
    out << "  <table>\n    <tr><th>#</th><th>类型</th><th>编码</th><th>Profile</th>"
           "<th>分辨率 / 采样率</th><th>码率</th><th>帧数 / 包数</th></tr>\n";
    for (const auto& stream : run.analysis.streams) {
        const char* type = stream.IsVideo() ? "视频" : (stream.IsAudio() ? "音频" : "其他");
        const std::string resolution = stream.IsVideo()
            ? (std::to_string(stream.width) + "x" + std::to_string(stream.height))
            : (std::to_string(stream.sample_rate) + " Hz / " +
               std::to_string(stream.channels) + " 声道");
        out << "    <tr><td>" << stream.index << "</td><td>" << type << "</td><td>"
            << Html(stream.codec_name) << "</td><td>" << Html(stream.profile_name) << "</td><td>"
            << Html(resolution) << "</td><td>"
            << Fixed(stream.bitrate_bps / 1000.0, 1) << " kbps</td><td>"
            << stream.frame_count << " / " << stream.packet_count << "</td></tr>\n";
    }
    out << "  </table>\n";

    if (run.analysis.audio_qc.analyzed) {
        out << "  <h2>响度与音频 QC</h2>\n  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
        out << "    <tr><td>积分响度</td><td>" << Fixed(run.analysis.audio_qc.integrated_lufs, 2)
            << " LUFS</td></tr>\n";
        out << "    <tr><td>真峰值</td><td>" << Fixed(run.analysis.audio_qc.true_peak_dbtp, 2)
            << " dBTP</td></tr>\n";
        out << "    <tr><td>响度范围 LRA</td><td>"
            << Fixed(run.analysis.audio_qc.loudness_range_lu, 2) << " LU</td></tr>\n";
        out << "    <tr><td>采样峰值</td><td>" << Fixed(run.analysis.audio_qc.sample_peak_dbfs, 2)
            << " dBFS</td></tr>\n";
        out << "    <tr><td>静音占比</td><td>"
            << Fixed(run.analysis.audio_qc.silence_ratio * 100.0, 2) << " %</td></tr>\n";
        out << "    <tr><td>削波样本</td><td>" << run.analysis.audio_qc.clipping_sample_count
            << "</td></tr>\n";
        out << "  </table>\n";
    }

    if (bundle.has_comparison) {
        out << "  <h2>文件对比</h2>\n";
        out << "  <p class=\"meta\">原文件: " << Html(bundle.comparison.left_path)
            << " ｜ 对比文件: " << Html(bundle.comparison.right_path) << " ｜ 差异项: "
            << bundle.comparison.DifferentCount() << "</p>\n";
        out << "  <table>\n    <tr><th>分组</th><th>字段</th><th>原文件</th><th>对比文件</th>"
               "<th>差异</th></tr>\n";
        for (const auto& row : bundle.comparison.rows) {
            if (row.diff == qc::QcFieldDiff::Unavailable) continue;
            out << "    <tr><td>" << Html(row.group) << "</td><td>" << Html(row.field)
                << "</td><td>" << Html(row.left) << " " << Html(row.unit) << "</td><td>"
                << Html(row.right) << " " << Html(row.unit) << "</td><td>"
                << Html(qc::ToString(row.diff)) << "</td></tr>\n";
        }
        out << "  </table>\n";
    }

    out << "  <h2>规则快照</h2>\n";
    out << "  <table>\n    <tr><th>规则</th><th>类别</th><th>阈值</th><th>启用</th></tr>\n";
    for (const auto& rule : report.rules) {
        out << "    <tr><td>" << Html(rule.name) << " (" << Html(rule.id) << ")</td><td>"
            << Html(model::ToString(rule.category)) << "</td><td>" << Fixed(rule.threshold, 2)
            << " " << Html(rule.unit) << "</td><td>" << (rule.enabled ? "是" : "否")
            << "</td></tr>\n";
    }
    out << "  </table>\n";

    out << "  <p class=\"meta\">Generated by VideoEye 2.0 ｜ schema version "
        << kReportSchemaVersion << "</p>\n</body>\n</html>\n";
    return out.str();
}

bool QcReportExporter::ExportHtml(const std::string& path, const QcExportBundle& bundle) {
    return WriteUtf8File(path, BuildHtml(bundle));
}

std::string QcReportExporter::BuildComparisonHtml(const qc::QcComparison& comparison) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n  <meta charset=\"UTF-8\">\n"
        << "  <title>VideoEye 文件对比</title>\n  <style>\n"
        << "    body { font-family: 'Microsoft YaHei', Arial, sans-serif; margin: 24px; }\n"
        << "    table { border-collapse: collapse; width: 100%; margin-top: 10px; font-size: 13px; }\n"
        << "    th, td { border: 1px solid #d8dee6; padding: 6px 8px; text-align: left; }\n"
        << "    th { background: #f2f4f7; }\n"
        << "    .different td { background: #fff5f5; }\n"
        << "    .meta { color: #6b7280; font-size: 12px; }\n"
        << "  </style>\n</head>\n<body>\n";
    out << "  <h1>VideoEye 文件对比</h1>\n";
    out << "  <p class=\"meta\">原文件: " << Html(comparison.left_path) << "<br>对比文件: "
        << Html(comparison.right_path) << "<br>评分 " << Fixed(comparison.left_score, 1) << " → "
        << Fixed(comparison.right_score, 1) << " ｜ 差异项 " << comparison.DifferentCount()
        << " / " << comparison.AvailableRowCount() << "</p>\n";
    out << "  <table>\n    <tr><th>分组</th><th>字段</th><th>原文件</th><th>对比文件</th>"
           "<th>单位</th><th>判定</th><th>差值</th></tr>\n";
    for (const auto& row : comparison.rows) {
        const bool different = row.diff == qc::QcFieldDiff::Different;
        out << "    <tr" << (different ? " class=\"different\"" : "") << "><td>" << Html(row.group)
            << "</td><td>" << Html(row.field) << "</td><td>" << Html(row.left) << "</td><td>"
            << Html(row.right) << "</td><td>" << Html(row.unit) << "</td><td>"
            << Html(qc::ToString(row.diff)) << "</td><td>"
            << (row.numeric ? Fixed(row.delta, 3) : std::string()) << "</td></tr>\n";
    }
    out << "  </table>\n</body>\n</html>\n";
    return out.str();
}

bool QcReportExporter::ExportComparisonHtml(const std::string& path,
                                           const qc::QcComparison& comparison) {
    return WriteUtf8File(path, BuildComparisonHtml(comparison));
}

bool QcReportExporter::ExportBatchSummaryHtml(const std::string& path,
                                             const QcBatchSummaryInput& input) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n  <meta charset=\"UTF-8\">\n"
        << "  <title>VideoEye 批量 QC 汇总</title>\n  <style>\n"
        << "    body { font-family: 'Microsoft YaHei', Arial, sans-serif; margin: 24px; }\n"
        << "    table { border-collapse: collapse; width: 100%; margin-top: 10px; font-size: 13px; }\n"
        << "    th, td { border: 1px solid #d8dee6; padding: 6px 8px; text-align: left; }\n"
        << "    th { background: #f2f4f7; }\n"
        << "    .bad td:nth-child(5) { color: #b3261e; font-weight: bold; }\n"
        << "    .meta { color: #6b7280; font-size: 12px; }\n"
        << "  </style>\n</head>\n<body>\n";
    out << "  <h1>VideoEye 批量 QC 汇总</h1>\n";
    out << "  <p class=\"meta\">目录: " << Html(input.root) << " ｜ 模板: "
        << Html(input.profile_name) << " (" << Html(input.profile_id) << ") ｜ 完成 "
        << input.succeeded << " / 失败 " << input.failed << " / 超时 " << input.timed_out
        << " / 取消 " << input.cancelled
        << " / 跳过 " << input.skipped << " ｜ 耗时 " << Fixed(input.elapsed_ms, 0)
        << " ms" << (input.completed ? "" : " ｜ <b>未完成（被取消）</b>") << "</p>\n";
    out << "  <table>\n    <tr><th>文件</th><th>状态</th><th>评分</th><th>结论</th>"
           "<th>致命</th><th>错误</th><th>警告</th><th>提示</th><th>耗时(ms)</th><th>输出</th></tr>\n";
    for (const auto& row : input.rows) {
        const bool bad = row.critical_count > 0 || row.error_count > 0;
        out << "    <tr" << (bad ? " class=\"bad\"" : "") << "><td>" << Html(row.path)
            << "</td><td>" << Html(row.status) << "</td><td>" << Fixed(row.score, 1)
            << "</td><td>" << Html(row.verdict) << "</td><td>" << row.critical_count
            << "</td><td>" << row.error_count << "</td><td>" << row.warning_count
            << "</td><td>" << row.info_count << "</td><td>" << Fixed(row.elapsed_ms, 0)
            << "</td><td>" << Html(row.output_path) << "</td></tr>\n";
    }
    out << "  </table>\n</body>\n</html>\n";
    return WriteUtf8File(path, out.str());
}

}  // namespace reporting
}  // namespace videoeye