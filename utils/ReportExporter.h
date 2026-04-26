#pragma once

#include <string>
#include <vector>

#include "core/analyzer/StreamAnalyzer.h"
#include "core/analyzer/FrameAnalyzer.h"
#include "core/analyzer/FaceDetector.h"

namespace videoeye {
namespace utils {

// 分析报告导出器
class ReportExporter {
public:
    ReportExporter() = default;
    ~ReportExporter() = default;
    
    // 导出为文本报告
    static bool ExportTextReport(
        const std::string& filename,
        const analyzer::StreamStats& stats,
        const std::string& video_file = "");
    
    // 导出为CSV格式 (用于Excel)
    static bool ExportCSV(
        const std::string& filename,
        const std::vector<analyzer::StreamStats>& stats_history);
    
    // 导出为JSON格式
    static bool ExportJSON(
        const std::string& filename,
        const analyzer::StreamStats& stats,
        const std::string& video_file = "");
    
    // 导出为HTML报告 (带图表)
    static bool ExportHTMLReport(
        const std::string& filename,
        const analyzer::StreamStats& stats,
        const std::vector<double>& fps_history,
        const std::vector<int>& bitrate_history,
        const std::string& video_file = "");
    
    // 生成报告摘要
    static std::string GenerateSummary(const analyzer::StreamStats& stats);
    
private:
    // 格式化时间
    static std::string FormatTime(double seconds);
    
    // 格式化码率
    static std::string FormatBitrate(int bps);
    
    // 转义HTML特殊字符
    static std::string EscapeHTML(const std::string& text);
};

} // namespace utils
} // namespace videoeye
