#include "ReportExporter.h"
#include "Logger.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace videoeye {
namespace utils {

bool ReportExporter::ExportTextReport(
    const std::string& filename,
    const analyzer::StreamStats& stats,
    const std::string& video_file) {
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建报告文件: " + filename);
        return false;
    }
    
    file << "========================================" << std::endl;
    file << "  VideoEye 视频分析报告" << std::endl;
    file << "========================================" << std::endl;
    file << std::endl;
    
    if (!video_file.empty()) {
        file << "视频文件: " << video_file << std::endl;
        file << "生成时间: " << GenerateSummary(stats) << std::endl;
        file << std::endl;
    }
    
    file << "--- 基本统计 ---" << std::endl;
    file << "总包数: " << stats.total_packets << std::endl;
    file << "总字节数: " << (stats.total_bytes / 1024) << " KB" << std::endl;
    file << "视频包: " << stats.video_packets << std::endl;
    file << "音频包: " << stats.audio_packets << std::endl;
    file << std::endl;
    
    file << "--- 帧率分析 ---" << std::endl;
    file << "当前帧率: " << std::fixed << std::setprecision(2) << stats.current_fps << " fps" << std::endl;
    file << "平均帧率: " << std::fixed << std::setprecision(2) << stats.avg_fps << " fps" << std::endl;
    file << std::endl;
    
    file << "--- 码率分析 ---" << std::endl;
    file << "当前码率: " << FormatBitrate(stats.current_bitrate_bps) << std::endl;
    file << "平均码率: " << FormatBitrate(stats.avg_bitrate_bps) << std::endl;
    file << "峰值码率: " << FormatBitrate(stats.peak_bitrate_bps) << std::endl;
    file << std::endl;
    
    file << "--- GOP 分析 ---" << std::endl;
    file << "GOP 大小: " << stats.current_gop_size << " 帧" << std::endl;
    file << "最大 GOP: " << stats.max_gop_size << " 帧" << std::endl;
    file << "关键帧数: " << stats.key_frame_count << std::endl;
    file << std::endl;
    
    file << "--- 包大小统计 ---" << std::endl;
    file << "平均大小: " << stats.avg_packet_size << " bytes" << std::endl;
    file << "最大大小: " << stats.max_packet_size << " bytes" << std::endl;
    file << "最小大小: " << stats.min_packet_size << " bytes" << std::endl;
    file << std::endl;
    
    file << "--- 时间统计 ---" << std::endl;
    file << "持续时间: " << FormatTime(stats.duration_seconds) << std::endl;
    file << std::endl;
    
    file << "========================================" << std::endl;
    file << "  报告结束" << std::endl;
    file << "========================================" << std::endl;
    
    file.close();
    LOG_INFO("文本报告已导出: " + filename);
    return true;
}

bool ReportExporter::ExportCSV(
    const std::string& filename,
    const std::vector<analyzer::StreamStats>& stats_history) {
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建CSV文件: " + filename);
        return false;
    }
    
    // CSV 表头
    file << "Time(s),TotalPackets,TotalBytes,VideoPackets,AudioPackets,"
         << "CurrentFPS,AvgFPS,CurrentBitrate,AvgBitrate,PeakBitrate,"
         << "GOPSize,KeyFrames,AvgPacketSize" << std::endl;
    
    // 数据行
    for (const auto& stats : stats_history) {
        file << stats.duration_seconds << ","
             << stats.total_packets << ","
             << stats.total_bytes << ","
             << stats.video_packets << ","
             << stats.audio_packets << ","
             << stats.current_fps << ","
             << stats.avg_fps << ","
             << stats.current_bitrate_bps << ","
             << stats.avg_bitrate_bps << ","
             << stats.peak_bitrate_bps << ","
             << stats.current_gop_size << ","
             << stats.key_frame_count << ","
             << stats.avg_packet_size << std::endl;
    }
    
    file.close();
    LOG_INFO("CSV报告已导出: " + filename);
    return true;
}

bool ReportExporter::ExportJSON(
    const std::string& filename,
    const analyzer::StreamStats& stats,
    const std::string& video_file) {
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建JSON文件: " + filename);
        return false;
    }
    
    file << "{" << std::endl;
    file << "  \"video_file\": \"" << EscapeJSON(video_file) << "\"," << std::endl;
    file << "  \"statistics\": {" << std::endl;
    file << "    \"total_packets\": " << stats.total_packets << "," << std::endl;
    file << "    \"total_bytes\": " << stats.total_bytes << "," << std::endl;
    file << "    \"video_packets\": " << stats.video_packets << "," << std::endl;
    file << "    \"audio_packets\": " << stats.audio_packets << "," << std::endl;
    file << "    \"current_fps\": " << std::fixed << std::setprecision(2) << stats.current_fps << "," << std::endl;
    file << "    \"avg_fps\": " << std::fixed << std::setprecision(2) << stats.avg_fps << "," << std::endl;
    file << "    \"current_bitrate_bps\": " << stats.current_bitrate_bps << "," << std::endl;
    file << "    \"avg_bitrate_bps\": " << stats.avg_bitrate_bps << "," << std::endl;
    file << "    \"peak_bitrate_bps\": " << stats.peak_bitrate_bps << "," << std::endl;
    file << "    \"gop_size\": " << stats.current_gop_size << "," << std::endl;
    file << "    \"max_gop_size\": " << stats.max_gop_size << "," << std::endl;
    file << "    \"key_frame_count\": " << stats.key_frame_count << "," << std::endl;
    file << "    \"avg_packet_size\": " << stats.avg_packet_size << "," << std::endl;
    file << "    \"max_packet_size\": " << stats.max_packet_size << "," << std::endl;
    file << "    \"min_packet_size\": " << stats.min_packet_size << "," << std::endl;
    file << "    \"duration_seconds\": " << std::fixed << std::setprecision(2) << stats.duration_seconds << std::endl;
    file << "  }" << std::endl;
    file << "}" << std::endl;
    
    file.close();
    LOG_INFO("JSON报告已导出: " + filename);
    return true;
}

bool ReportExporter::ExportHTMLReport(
    const std::string& filename,
    const analyzer::StreamStats& stats,
    const std::vector<double>& fps_history,
    const std::vector<int>& bitrate_history,
    const std::string& video_file) {
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建HTML文件: " + filename);
        return false;
    }
    
    file << "<!DOCTYPE html>" << std::endl;
    file << "<html lang=\"zh-CN\">" << std::endl;
    file << "<head>" << std::endl;
    file << "  <meta charset=\"UTF-8\">" << std::endl;
    file << "  <title>VideoEye 分析报告</title>" << std::endl;
    file << "  <style>" << std::endl;
    file << "    body { font-family: Arial, sans-serif; margin: 20px; }" << std::endl;
    file << "    h1 { color: #333; }" << std::endl;
    file << "    table { border-collapse: collapse; width: 100%; margin: 20px 0; }" << std::endl;
    file << "    th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }" << std::endl;
    file << "    th { background-color: #4CAF50; color: white; }" << std::endl;
    file << "    tr:nth-child(even) { background-color: #f2f2f2; }" << std::endl;
    file << "    .chart { margin: 20px 0; padding: 20px; background: #f9f9f9; }" << std::endl;
    file << "  </style>" << std::endl;
    file << "</head>" << std::endl;
    file << "<body>" << std::endl;
    
    file << "  <h1>VideoEye 视频分析报告</h1>" << std::endl;
    if (!video_file.empty()) {
        file << "  <p><strong>视频文件:</strong> " << EscapeHTML(video_file) << "</p>" << std::endl;
    }
    
    file << "  <h2>统计摘要</h2>" << std::endl;
    file << "  <table>" << std::endl;
    file << "    <tr><th>指标</th><th>值</th></tr>" << std::endl;
    file << "    <tr><td>总包数</td><td>" << stats.total_packets << "</td></tr>" << std::endl;
    file << "    <tr><td>总字节数</td><td>" << (stats.total_bytes / 1024) << " KB</td></tr>" << std::endl;
    file << "    <tr><td>当前帧率</td><td>" << std::fixed << std::setprecision(2) << stats.current_fps << " fps</td></tr>" << std::endl;
    file << "    <tr><td>平均帧率</td><td>" << std::fixed << std::setprecision(2) << stats.avg_fps << " fps</td></tr>" << std::endl;
    file << "    <tr><td>当前码率</td><td>" << FormatBitrate(stats.current_bitrate_bps) << "</td></tr>" << std::endl;
    file << "    <tr><td>平均码率</td><td>" << FormatBitrate(stats.avg_bitrate_bps) << "</td></tr>" << std::endl;
    file << "    <tr><td>峰值码率</td><td>" << FormatBitrate(stats.peak_bitrate_bps) << "</td></tr>" << std::endl;
    file << "    <tr><td>GOP 大小</td><td>" << stats.current_gop_size << " 帧</td></tr>" << std::endl;
    file << "    <tr><td>关键帧数</td><td>" << stats.key_frame_count << "</td></tr>" << std::endl;
    file << "    <tr><td>持续时间</td><td>" << FormatTime(stats.duration_seconds) << "</td></tr>" << std::endl;
    file << "  </table>" << std::endl;
    
    // 简单的柱状图 (使用HTML)
    file << "  <h2>帧率趋势</h2>" << std::endl;
    file << "  <div class=\"chart\">" << std::endl;
    if (!fps_history.empty()) {
        double max_fps = *std::max_element(fps_history.begin(), fps_history.end());
        for (size_t i = 0; i < fps_history.size() && i < 60; ++i) {
            double height = (fps_history[i] / max_fps) * 100;
            file << "    <div style=\"display: inline-block; width: 10px; height: " 
                 << height << "px; background: #4CAF50; margin: 1px;\" "
                 << "title=\"" << fps_history[i] << " fps\"></div>" << std::endl;
        }
    }
    file << "  </div>" << std::endl;
    
    file << "  <footer style=\"margin-top: 40px; color: #666; font-size: 12px;\">" << std::endl;
    file << "    <p>Generated by VideoEye 2.0</p>" << std::endl;
    file << "  </footer>" << std::endl;
    
    file << "</body>" << std::endl;
    file << "</html>" << std::endl;
    
    file.close();
    LOG_INFO("HTML报告已导出: " + filename);
    return true;
}

// ===========================================================================
// 诊断报告 (model::QcReport) 导出
// ===========================================================================

bool ReportExporter::ExportQcReport(const std::string& filename, const model::QcReport& report) {
    const std::string lower = [&filename]() {
        std::string s;
        const size_t dot = filename.rfind('.');
        if (dot != std::string::npos) {
            s = filename.substr(dot);
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        }
        return s;
    }();

    if (lower == ".json") return ExportQcReportJSON(filename, report);
    if (lower == ".html" || lower == ".htm") return ExportQcReportHTML(filename, report);
    if (lower == ".csv") return ExportQcReportCSV(filename, report);
    return ExportQcReportText(filename, report);
}

bool ReportExporter::ExportQcReportJSON(const std::string& filename, const model::QcReport& report) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建诊断JSON文件: " + filename);
        return false;
    }

    file << "{\n";
    file << "  \"file_path\": \"" << EscapeJSON(report.file_path) << "\",\n";
    file << "  \"file_name\": \"" << EscapeJSON(report.file_name) << "\",\n";
    file << "  \"container_format\": \"" << EscapeJSON(report.container_format) << "\",\n";
    file << "  \"duration_seconds\": " << std::fixed << std::setprecision(3)
         << report.duration_seconds << ",\n";
    file << "  \"file_size_bytes\": " << report.file_size_bytes << ",\n";
    file << "  \"overall_bitrate_bps\": " << report.overall_bitrate_bps << ",\n";
    file << "  \"video_stream_count\": " << report.video_stream_count << ",\n";
    file << "  \"audio_stream_count\": " << report.audio_stream_count << ",\n";
    file << "  \"score\": " << std::fixed << std::setprecision(1) << report.score << ",\n";
    file << "  \"verdict\": \"" << EscapeJSON(report.verdict) << "\",\n";
    file << "  \"generated_at\": \"" << EscapeJSON(report.generated_at) << "\",\n";
    file << "  \"analysis_elapsed_ms\": " << std::fixed << std::setprecision(1)
         << report.analysis_elapsed_ms << ",\n";
    file << "  \"completed\": " << (report.completed ? "true" : "false") << ",\n";

    file << "  \"severity_counts\": {\n";
    file << "    \"critical\": " << report.CountBySeverity(model::IssueSeverity::Critical) << ",\n";
    file << "    \"error\": " << report.CountBySeverity(model::IssueSeverity::Error) << ",\n";
    file << "    \"warning\": " << report.CountBySeverity(model::IssueSeverity::Warning) << ",\n";
    file << "    \"info\": " << report.CountBySeverity(model::IssueSeverity::Info) << "\n";
    file << "  },\n";

    file << "  \"issues\": [\n";
    for (size_t i = 0; i < report.issues.size(); ++i) {
        const auto& issue = report.issues[i];
        file << "    {\n";
        file << "      \"rule_id\": \"" << EscapeJSON(issue.rule_id) << "\",\n";
        file << "      \"title\": \"" << EscapeJSON(issue.title) << "\",\n";
        file << "      \"severity\": \"" << EscapeJSON(issue.SeverityText()) << "\",\n";
        file << "      \"category\": \"" << EscapeJSON(issue.CategoryText()) << "\",\n";
        file << "      \"detail\": \"" << EscapeJSON(issue.detail) << "\",\n";
        file << "      \"suggestion\": \"" << EscapeJSON(issue.suggestion) << "\",\n";
        file << "      \"time_range\": \"" << EscapeJSON(issue.range.ToString()) << "\",\n";
        file << "      \"stream_index\": " << issue.stream_index << ",\n";
        file << "      \"metric_value\": " << std::fixed << std::setprecision(4)
             << issue.metric_value << ",\n";
        file << "      \"threshold\": " << std::fixed << std::setprecision(4)
             << issue.threshold << ",\n";
        file << "      \"occurrence_count\": " << issue.occurrence_count << "\n";
        file << "    }" << (i + 1 < report.issues.size() ? "," : "") << "\n";
    }
    file << "  ],\n";

    file << "  \"rules\": [\n";
    for (size_t i = 0; i < report.rules.size(); ++i) {
        const auto& rule = report.rules[i];
        file << "    {\n";
        file << "      \"id\": \"" << EscapeJSON(rule.id) << "\",\n";
        file << "      \"name\": \"" << EscapeJSON(rule.name) << "\",\n";
        file << "      \"category\": \"" << EscapeJSON(ToString(rule.category)) << "\",\n";
        file << "      \"severity\": \"" << EscapeJSON(ToString(rule.severity)) << "\",\n";
        file << "      \"op\": \"" << EscapeJSON(ToString(rule.op)) << "\",\n";
        file << "      \"threshold\": " << std::fixed << std::setprecision(4) << rule.threshold << ",\n";
        file << "      \"unit\": \"" << EscapeJSON(rule.unit) << "\",\n";
        file << "      \"enabled\": " << (rule.enabled ? "true" : "false") << "\n";
        file << "    }" << (i + 1 < report.rules.size() ? "," : "") << "\n";
    }
    file << "  ]\n";
    file << "}\n";

    file.close();
    LOG_INFO("诊断JSON报告已导出: " + filename);
    return true;
}

bool ReportExporter::ExportQcReportHTML(const std::string& filename, const model::QcReport& report) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建诊断HTML文件: " + filename);
        return false;
    }

    auto severity_class = [](model::IssueSeverity severity) {
        switch (severity) {
            case model::IssueSeverity::Critical: return "critical";
            case model::IssueSeverity::Error:    return "error";
            case model::IssueSeverity::Warning:  return "warning";
            case model::IssueSeverity::Info:     return "info";
        }
        return "info";
    };

    file << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n";
    file << "  <meta charset=\"UTF-8\">\n  <title>VideoEye 诊断报告</title>\n";
    file << "  <style>\n"
         << "    body { font-family: 'Microsoft YaHei', Arial, sans-serif; margin: 24px; color: #222; }\n"
         << "    h1 { font-size: 22px; }\n"
         << "    h2 { font-size: 18px; margin-top: 28px; border-left: 4px solid #4CAF50; padding-left: 8px; }\n"
         << "    table { border-collapse: collapse; width: 100%; margin-top: 10px; }\n"
         << "    th, td { border: 1px solid #ddd; padding: 8px; text-align: left; vertical-align: top; }\n"
         << "    th { background-color: #f5f5f5; }\n"
         << "    .score { font-size: 32px; font-weight: bold; }\n"
         << "    .pass { color: #2e7d32; } .warn { color: #ef6c00; } .fail { color: #c62828; }\n"
         << "    .critical td:first-child { color: #c62828; font-weight: bold; }\n"
         << "    .error td:first-child { color: #c62828; }\n"
         << "    .warning td:first-child { color: #ef6c00; }\n"
         << "    .info td:first-child { color: #1565c0; }\n"
         << "    .meta { color: #666; font-size: 13px; }\n"
         << "  </style>\n</head>\n<body>\n";

    file << "  <h1>VideoEye 诊断报告</h1>\n";
    file << "  <p class=\"meta\">文件: " << EscapeHTML(report.file_path)
         << " ｜ 生成时间: " << EscapeHTML(report.generated_at)
         << " ｜ 分析耗时: " << std::fixed << std::setprecision(0) << report.analysis_elapsed_ms
         << " ms" << (report.completed ? "" : " ｜ <b>分析被取消，结果不完整</b>") << "</p>\n";

    const std::string verdict_class =
        (report.score >= 90.0) ? "pass" : ((report.score >= 70.0) ? "warn" : "fail");
    file << "  <h2>总览</h2>\n";
    file << "  <p class=\"score " << verdict_class << "\">" << std::fixed << std::setprecision(1)
         << report.score << " / 100 — " << EscapeHTML(report.verdict) << "</p>\n";
    file << "  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
    file << "    <tr><td>容器格式</td><td>" << EscapeHTML(report.container_format) << "</td></tr>\n";
    file << "    <tr><td>时长</td><td>" << FormatTime(report.duration_seconds) << " ("
         << std::fixed << std::setprecision(3) << report.duration_seconds << " s)</td></tr>\n";
    file << "    <tr><td>文件大小</td><td>" << (report.file_size_bytes / 1024) << " KB</td></tr>\n";
    file << "    <tr><td>整体码率</td><td>" << FormatBitrate(static_cast<int>(report.overall_bitrate_bps)) << "</td></tr>\n";
    file << "    <tr><td>视频流 / 音频流</td><td>" << report.video_stream_count << " / "
         << report.audio_stream_count << "</td></tr>\n";
    file << "    <tr><td>问题数（致命/错误/警告/提示）</td><td>"
         << report.CountBySeverity(model::IssueSeverity::Critical) << " / "
         << report.CountBySeverity(model::IssueSeverity::Error) << " / "
         << report.CountBySeverity(model::IssueSeverity::Warning) << " / "
         << report.CountBySeverity(model::IssueSeverity::Info) << "</td></tr>\n";
    file << "  </table>\n";

    file << "  <h2>问题清单</h2>\n";
    if (report.issues.empty()) {
        file << "  <p>未发现问题。</p>\n";
    } else {
        file << "  <table>\n    <tr><th>严重度</th><th>类别</th><th>问题</th><th>位置</th>"
                "<th>说明</th><th>建议</th></tr>\n";
        for (const auto& issue : report.issues) {
            file << "    <tr class=\"" << severity_class(issue.severity) << "\">"
                 << "<td>" << EscapeHTML(issue.SeverityText()) << "</td>"
                 << "<td>" << EscapeHTML(issue.CategoryText()) << "</td>"
                 << "<td>" << EscapeHTML(issue.title) << "</td>"
                 << "<td>" << EscapeHTML(issue.range.ToString()) << "</td>"
                 << "<td>" << EscapeHTML(issue.detail) << "</td>"
                 << "<td>" << EscapeHTML(issue.suggestion) << "</td></tr>\n";
        }
        file << "  </table>\n";
    }

    file << "  <h2>规则快照</h2>\n";
    file << "  <table>\n    <tr><th>规则</th><th>类别</th><th>判定</th><th>阈值</th><th>启用</th></tr>\n";
    for (const auto& rule : report.rules) {
        file << "    <tr><td>" << EscapeHTML(rule.name) << " (" << EscapeHTML(rule.id) << ")</td>"
             << "<td>" << EscapeHTML(ToString(rule.category)) << "</td>"
             << "<td>" << EscapeHTML(ToString(rule.op)) << "</td>"
             << "<td>" << std::fixed << std::setprecision(2) << rule.threshold
             << " " << EscapeHTML(rule.unit) << "</td>"
             << "<td>" << (rule.enabled ? "是" : "否") << "</td></tr>\n";
    }
    file << "  </table>\n";

    file << "  <p class=\"meta\">Generated by VideoEye 2.0</p>\n</body>\n</html>\n";

    file.close();
    LOG_INFO("诊断HTML报告已导出: " + filename);
    return true;
}

bool ReportExporter::ExportQcReportCSV(const std::string& filename, const model::QcReport& report) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建诊断CSV文件: " + filename);
        return false;
    }

    // CSV 字段转义: 加引号 + 内部引号翻倍
    auto csv_field = [](const std::string& text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += "\"";
        return out;
    };

    file << "\xEF\xBB\xBF";  // UTF-8 BOM for Excel
    file << "严重度,类别,规则ID,问题,位置,流序号,实测值,阈值,出现次数,说明,建议\n";
    for (const auto& issue : report.issues) {
        file << csv_field(issue.SeverityText()) << ","
             << csv_field(issue.CategoryText()) << ","
             << csv_field(issue.rule_id) << ","
             << csv_field(issue.title) << ","
             << csv_field(issue.range.ToString()) << ","
             << issue.stream_index << ","
             << std::fixed << std::setprecision(4) << issue.metric_value << ","
             << std::fixed << std::setprecision(4) << issue.threshold << ","
             << issue.occurrence_count << ","
             << csv_field(issue.detail) << ","
             << csv_field(issue.suggestion) << "\n";
    }

    file.close();
    LOG_INFO("诊断CSV报告已导出: " + filename);
    return true;
}

bool ReportExporter::ExportQcReportText(const std::string& filename, const model::QcReport& report) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR("无法创建诊断文本报告: " + filename);
        return false;
    }

    file << "========================================\n";
    file << "  VideoEye 诊断报告\n";
    file << "========================================\n\n";
    file << "文件: " << report.file_path << "\n";
    file << "生成时间: " << report.generated_at << "\n";
    file << "评分: " << std::fixed << std::setprecision(1) << report.score << " / 100 ("
         << report.verdict << ")\n";
    if (!report.completed) file << "注意: 分析被取消，结果不完整\n";
    file << "\n--- 问题清单 ---\n";
    if (report.issues.empty()) {
        file << "未发现问题。\n";
    } else {
        for (size_t i = 0; i < report.issues.size(); ++i) {
            const auto& issue = report.issues[i];
            file << "[" << (i + 1) << "] " << issue.SeverityText() << " / " << issue.CategoryText()
                 << " / " << issue.title << "\n"
                 << "    位置: " << issue.range.ToString() << "\n"
                 << "    " << issue.detail << "\n"
                 << "    建议: " << issue.suggestion << "\n";
        }
    }

    file.close();
    LOG_INFO("诊断文本报告已导出: " + filename);
    return true;
}

std::string ReportExporter::GenerateSummary(const analyzer::StreamStats& stats) {
    std::ostringstream oss;
    oss << "总包数: " << stats.total_packets
        << ", 平均帧率: " << std::fixed << std::setprecision(2) << stats.avg_fps << " fps"
        << ", 平均码率: " << FormatBitrate(stats.avg_bitrate_bps)
        << ", 持续时间: " << FormatTime(stats.duration_seconds);
    return oss.str();
}

std::string ReportExporter::FormatTime(double seconds) {
    int hours = static_cast<int>(seconds) / 3600;
    int minutes = (static_cast<int>(seconds) % 3600) / 60;
    int secs = static_cast<int>(seconds) % 60;
    
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << secs;
    return oss.str();
}

std::string ReportExporter::FormatBitrate(int bps) {
    if (bps >= 1000000) {
        return std::to_string(bps / 1000000) + " Mbps";
    } else if (bps >= 1000) {
        return std::to_string(bps / 1000) + " Kbps";
    } else {
        return std::to_string(bps) + " bps";
    }
}

std::string ReportExporter::EscapeHTML(const std::string& text) {
    std::string result;
    for (char c : text) {
        switch (c) {
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '&': result += "&amp;"; break;
            case '\"': result += "&quot;"; break;
            default: result += c;
        }
    }
    return result;
}

std::string ReportExporter::EscapeJSON(const std::string& text) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase;
    for (unsigned char c : text) {
        switch (c) {
            case '\"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::setw(4) << std::setfill('0') 
                        << static_cast<int>(c) << std::setfill(' ');
                } else {
                    oss << static_cast<char>(c);
                }
                break;
        }
    }
    return oss.str();
}

} // namespace utils
} // namespace videoeye
