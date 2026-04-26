#include "ReportExporter.h"
#include "Logger.h"
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
    file << "GOP 大小: " << stats.gop_size << " 帧" << std::endl;
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
             << stats.gop_size << ","
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
    file << "  \"video_file\": \"" << video_file << "\"," << std::endl;
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
    file << "    \"gop_size\": " << stats.gop_size << "," << std::endl;
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
    file << "    <tr><td>GOP 大小</td><td>" << stats.gop_size << " 帧</td></tr>" << std::endl;
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

} // namespace utils
} // namespace videoeye
