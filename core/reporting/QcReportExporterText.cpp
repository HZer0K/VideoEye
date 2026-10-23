#include "core/reporting/QcReportExporter.h"

#include "core/reporting/QcReportExporterInternal.h"

#include <sstream>

namespace videoeye {
namespace reporting {

using detail::Fixed;
using detail::Int64Text;
using detail::WriteUtf8File;

// ===========================================================================
// 纯文本
// ===========================================================================

std::string QcReportExporter::BuildText(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    std::ostringstream out;
    out << "========================================\n  VideoEye QC 报告\n"
        << "========================================\n";
    out << "模板: " << bundle.profile_name << " (" << bundle.profile_id << ")\n";
    out << "文件: " << report.file_path << "\n";
    out << "生成时间: " << report.generated_at << "\n";
    out << "分析耗时: " << Fixed(run.elapsed_ms, 0) << " ms\n";
    out << "评分: " << Fixed(report.score, 1) << " / 100 (" << report.verdict << ")\n";
    if (report.partial)
        out << "注意: 本次为抽样扫描（命中包数上限），结果仅覆盖部分数据，结论仅供参考\n";
    else if (!report.completed)
        out << "注意: 分析未跑完，结果不完整\n";
    if (!run.error.empty())
        out << "错误: " << run.error << "\n";

    out << "\n--- 汇总 ---\n";
    out << "致命 " << report.CountBySeverity(model::IssueSeverity::Critical) << " / 错误 "
        << report.CountBySeverity(model::IssueSeverity::Error) << " / 警告 "
        << report.CountBySeverity(model::IssueSeverity::Warning) << " / 提示 "
        << report.CountBySeverity(model::IssueSeverity::Info) << "\n";
    out << "时长 " << Fixed(report.duration_seconds, 3) << " s ｜ 大小 " << report.file_size_bytes << " 字节 ｜ 码率 "
        << Fixed(report.overall_bitrate_bps / 1000.0, 1) << " kbps\n";

    out << "\n--- 问题清单 ---\n";
    if (report.issues.empty()) {
        out << "未发现问题。\n";
    } else {
        for (std::size_t i = 0; i < report.issues.size(); ++i) {
            const auto& issue = report.issues[i];
            out << "[" << (i + 1) << "] " << issue.SeverityText() << " / " << issue.CategoryText() << " / "
                << issue.title << " (" << issue.rule_id << ")\n";
            out << "    位置: " << issue.range.ToString() << "\n";
            out << "    " << issue.detail << "\n";
            if (!issue.suggestion.empty())
                out << "    建议: " << issue.suggestion << "\n";
        }
    }

    if (run.analysis.audio_qc.analyzed) {
        out << "\n--- 响度 ---\n";
        out << "  积分响度: " << Fixed(run.analysis.audio_qc.integrated_lufs, 2) << " LUFS\n";
        out << "  真峰值: " << Fixed(run.analysis.audio_qc.true_peak_dbtp, 2) << " dBTP\n";
        out << "  LRA: " << Fixed(run.analysis.audio_qc.loudness_range_lu, 2) << " LU\n";
    }

    // MPEG-TS 传输层体检。落这些数字的用意见 QcReportExporterJson.cpp 里同一节的注释：
    // 规则只在命中时才有 detail，未命中什么都不留，所以"扫了多少包、是否扫完"
    // 必须由报告自己说清楚，否则 container.ts.* 的未命中无法区分"没问题"与"没证据"。
    if (run.analysis.ts_transport.applicable) {
        const model::TransportStreamInfo& ts = run.analysis.ts_transport;
        out << "\n--- MPEG-TS 传输层 ---\n";
        if (!ts.analyzed) {
            out << "  未拿到传输层结论，以下 container.ts.* 规则的未命中只代表没有证据，"
                   "不代表没有问题。\n";
            if (!ts.error_message.empty())
                out << "  原因: " << ts.error_message << "\n";
        } else {
            out << "  包长: " << ts.packet_size << " 字节\n";
            out << "  扫描范围: " << Int64Text(ts.scanned_packets) << " / " << Int64Text(ts.estimated_packets) << " 包"
                << (ts.scan_complete ? "（已扫全文件）" : "（只扫了前若干包，结论不代表整个文件）") << "\n";
            out << "  节目 / 流 / 服务: " << ts.programs << " / " << ts.streams << " / " << ts.services << "\n";
            out << "  PAT / PMT / SDT: " << (ts.has_pat ? "有" : "无") << " / " << (ts.has_pmt ? "有" : "无") << " / "
                << (ts.has_sdt ? "有" : "无") << "\n";
            out << "  失步次数: " << Int64Text(ts.sync_loss) << "\n";
            out << "  连续计数错误: " << Int64Text(ts.continuity_errors) << "（占已扫包 "
                << Fixed(ts.ContinuityErrorPercent(), 3) << "%）\n";
            out << "  加扰包: " << Int64Text(ts.scrambled_packets) << "\n";
            out << "  最大 PCR 间隔: " << Fixed(ts.pcr_max_gap_ms, 1) << " ms"
                << (ts.pcr_max_gap_ms > 0.0 ? "" : "（未采到 PCR）") << "\n";
            // 载荷级的两个数字：没扫过就说"没扫过"，别让 0 被读成"很健康"。
            if (!ts.pes_payload_scanned) {
                out << "  PES 载荷级检查: 未执行（非 H.264/HEVC，或 scan_pes_payload 已关闭）\n";
            } else if (ts.pes_streams_checked <= 0) {
                out << "  PES 载荷级检查: 没有可跟踪的 ES（缺 PMT / 全加扰）\n";
            } else {
                out << "  PES 载荷级检查: 跟踪 " << ts.pes_streams_checked << " 条 ES，识别到 "
                    << Int64Text(ts.idr_count) << " 个随机接入点\n";
                out << "  最长无 IDR: " << Fixed(ts.max_no_idr_interval_ms, 1) << " ms"
                    << (ts.max_no_idr_pid.empty() ? "" : ("（PID " + ts.max_no_idr_pid + "）"))
                    << (ts.video_pids_without_idr > 0
                            ? "，有 " + std::to_string(ts.video_pids_without_idr) + " 条视频 PID 整段没有 IDR"
                            : "")
                    << "\n";
                out << "  最大 PES 断档: " << Fixed(ts.max_pid_gap_ms, 1) << " ms"
                    << (ts.max_pid_gap_pid.empty() ? "" : ("（PID " + ts.max_pid_gap_pid + "）"))
                    << (ts.stalled_pids > 1 ? "，共 " + std::to_string(ts.stalled_pids) + " 个 PID 超阈值" : "")
                    << "\n";
            }
        }
    }

    if (bundle.has_comparison) {
        out << "\n--- 对比 ---\n" << BuildComparisonText(bundle.comparison);
    }

    return out.str();
}

bool QcReportExporter::ExportText(const std::string& path, const QcExportBundle& bundle) {
    return WriteUtf8File(path, BuildText(bundle));
}

// ===========================================================================
// 对比导出
// ===========================================================================

std::string QcReportExporter::BuildComparisonText(const qc::QcComparison& comparison) {
    std::ostringstream out;
    out << "原文件  : " << comparison.left_path << "\n";
    out << "对比文件: " << comparison.right_path << "\n";
    out << "评分    : " << Fixed(comparison.left_score, 1) << " -> " << Fixed(comparison.right_score, 1) << "\n";
    out << "差异项  : " << comparison.DifferentCount() << " / " << comparison.AvailableRowCount() << "\n";

    for (const auto& group : comparison.GroupNames()) {
        out << "\n[" << group << "]\n";
        for (const auto& row : comparison.rows) {
            if (row.group != group)
                continue;
            out << "  " << row.field << ": " << row.left;
            if (!row.unit.empty())
                out << " " << row.unit;
            out << "  ->  " << row.right;
            if (!row.unit.empty())
                out << " " << row.unit;
            if (row.diff == qc::QcFieldDiff::Different) {
                out << "   (差 " << Fixed(row.delta, 3) << (row.unit.empty() ? "" : " " + row.unit) << ")";
            } else if (row.diff != qc::QcFieldDiff::Same) {
                out << "   (" << qc::ToString(row.diff) << ")";
            }
            out << "\n";
        }
    }
    return out.str();
}

bool QcReportExporter::ExportComparisonText(const std::string& path, const qc::QcComparison& comparison) {
    return WriteUtf8File(path, BuildComparisonText(comparison));
}

} // namespace reporting
} // namespace videoeye