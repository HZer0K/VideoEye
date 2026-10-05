#pragma once

// 播放器「实时流统计」快照导出器。
//
// 与 core/reporting/QcReportExporter.h 是两条独立的产品线：
//   这里导出的是 StreamAnalyzer 逐包统计出来的码率/帧率/GOP 快照（AnalysisPanel 的
//   「导出分析报告」），输入是 model::StreamStats；
//   QcReportExporter 导出的是 QC 体检报告，输入是 QcExportBundle(QcRunResult)。
//
// 以前这两件事都堆在 videoeye::ReportExporter 里，一个头文件同时 include StreamAnalyzer.h
// 和 QcReport.h，导致 reporting 层必须连全套 FFmpeg。拆开之后 reporting 只剩
// StreamAnalyzer 这一处 FFmpeg 依赖。

#include <string>
#include <vector>

#include "core/domain/model/StreamStats.h"

namespace videoeye {
namespace reporting {

class StreamStatsExporter {
public:
    StreamStatsExporter() = default;
    ~StreamStatsExporter() = default;

    // 导出为文本报告
    static bool ExportTextReport(const std::string& filename,
                                 const model::StreamStats& stats,
                                 const std::string& video_file = "");

    // 导出为 CSV 格式（用于 Excel）
    static bool ExportCSV(const std::string& filename,
                          const std::vector<model::StreamStats>& stats_history);

    // 导出为 JSON 格式
    static bool ExportJSON(const std::string& filename,
                           const model::StreamStats& stats,
                           const std::string& video_file = "");

    // 导出为 HTML 报告（带图表）
    static bool ExportHTMLReport(const std::string& filename,
                                 const model::StreamStats& stats,
                                 const std::vector<double>& fps_history,
                                 const std::vector<int>& bitrate_history,
                                 const std::string& video_file = "");

    // 生成报告摘要
    static std::string GenerateSummary(const model::StreamStats& stats);

private:
    static std::string FormatTime(double seconds);
    static std::string FormatBitrate(int bps);
    static std::string EscapeHTML(const std::string& text);
    static std::string EscapeJSON(const std::string& text);
};

}  // namespace reporting
}  // namespace videoeye
