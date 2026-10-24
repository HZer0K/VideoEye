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

    // ---- 证据与覆盖状态（阶段 5）----
    //
    // 放在"问题清单"后面而不是前面：读者先关心有什么问题，再看每个维度的
    // 结论可信到什么程度。这一节的存在意义是让"未命中"变得可解释 ——
    // 没有它，"问题清单里没有 XX"与"XX 根本没跑"在文本报告里长得一样。
    if (!run.analysis.dimensions.empty()) {
        out << "\n--- 证据与覆盖 ---\n";
        for (const model::DimensionStatus& status : run.analysis.dimensions) {
            out << "  [" << model::DimensionName(status.dimension) << "] ";
            if (!status.executed) {
                out << "未执行";
                if (!status.failure_reason.empty())
                    out << "：" << status.failure_reason;
                out << "\n";
                continue;
            }
            out << model::CapabilityToString(status.capability);
            if (status.conclusive)
                out << "（结论可作通过用）";
            if (status.has_coverage) {
                out << " ｜ 覆盖 " << Int64Text(status.units_scanned);
                if (!status.scope_unit.empty())
                    out << " " << status.scope_unit;
                out << " / " << Int64Text(status.units_total) << "（" << Fixed(status.CoveragePercent(), 1) << "%）";
            } else if (status.units_scanned > 0) {
                out << " ｜ 已扫 " << Int64Text(status.units_scanned);
                if (!status.scope_unit.empty())
                    out << " " << status.scope_unit;
                out << "（总量未知）";
            }
            if (status.failures > 0)
                out << " ｜ 失败 " << Int64Text(status.failures);
            out << "\n";
            if (!status.failure_reason.empty())
                out << "      限制: " << status.failure_reason << "\n";
            for (const std::string& note : status.notes)
                out << "      说明: " << note << "\n";
        }
    }

    // ---- 参数变化时间线（阶段 1）----
    //
    // 逐条给出"什么时候、哪份参数集、从什么变成什么、在文件哪个位置"，
    // 让拼接/编码器重启这类问题可以直接定位到现场。
    if (!run.analysis.parameter_timelines.empty()) {
        out << "\n--- 参数变化时间线 ---\n";
        for (const model::ParameterTimeline& timeline : run.analysis.parameter_timelines) {
            out << "  视频流 #" << timeline.stream_index;
            if (!timeline.codec_name.empty())
                out << "（" << timeline.codec_name << "）";
            out << "：" << timeline.Summary() << "\n";
            if (!timeline.truncated)
                out << "      扫描范围: " << Int64Text(timeline.packets_scanned) << " 个包，见到 "
                    << Int64Text(timeline.parameter_set_units) << " 份参数集\n";
            else
                out << "      扫描范围: " << Int64Text(timeline.packets_scanned)
                    << " 个包（已达追踪上限，后面的变化未记录）\n";
            if (timeline.missing_reference_count > 0) {
                out << "      ❗ 有 " << Int64Text(timeline.missing_reference_count)
                    << " 处引用了从未传输的参数集（解码必然失败）\n";
            }
            for (const model::ParameterChangeEvent& event : timeline.changes) {
                out << "      - " << event.ToString() << "\n";
                if (event.replaced_same_id)
                    out << "        ❗ 同一参数集 id 的内容被替换：此前的帧与之后的帧不是同一套配置\n";
            }
            // 有界输出：参数集条目太多时只列前面几条，避免文本报告上千行
            constexpr std::size_t kMaxListedSets = 12;
            std::size_t listed = 0;
            for (const model::ParameterSetEntry& entry : timeline.parameter_sets) {
                if (listed++ >= kMaxListedSets) {
                    out << "      ...（另有 " << (timeline.parameter_sets.size() - kMaxListedSets)
                        << " 份参数集未列出）\n";
                    break;
                }
                out << "      · " << model::ParameterSetKindCode(entry.kind) << "(id=" << entry.id << ") "
                    << entry.summary << " ｜ 出现 " << Int64Text(entry.occurrences) << " 次";
                if (entry.replacements > 0)
                    out << "，内容被替换 " << Int64Text(entry.replacements) << " 次";
                if (entry.first_file_offset >= 0)
                    out << " ｜ 首次偏移 0x" << std::hex << entry.first_file_offset << std::dec;
                out << "\n";
            }
            for (const std::string& note : timeline.notes)
                out << "      说明: " << note << "\n";
        }
    }

    // ---- 帧级显示顺序（POC，仅 H.264）----
    //
    // 文本报告里逐帧明细意义不大（几千行没人看），所以这里只落统计量 + 支持范围，
    // 逐帧明细留给 JSON 与 UI。空数组时不落这一节：那代表"没有 H.264 轨"，
    // 落了反而让人以为"跑了但没结果"。
    if (!run.analysis.frame_order_timelines.empty()) {
        out << "\n--- 帧级显示顺序（POC）---\n";
        for (const model::FrameOrderTimeline& timeline : run.analysis.frame_order_timelines) {
            out << "  视频流 #" << timeline.stream_index << "：";
            if (!timeline.available) {
                out << "不可用 —— "
                    << (timeline.unavailable_reason.empty() ? std::string("原因未记录") : timeline.unavailable_reason)
                    << "\n";
                continue;
            }
            out << timeline.Summary() << "\n";
            out << "      GOP 分段 ";
            if (timeline.gop_count > 0) {
                out << Int64Text(timeline.gop_count) << " 段：最短 " << Int64Text(timeline.min_gop_frames)
                    << " 帧 / 最长 " << Int64Text(timeline.max_gop_frames) << " 帧 / 平均 "
                    << Fixed(timeline.avg_gop_frames, 1) << " 帧\n";
            } else {
                out << "无法判定（没有识别到 GOP 边界）\n";
            }
            if (timeline.HasReordering()) {
                out << "      显示顺序重排 " << Int64Text(timeline.reorder_events) << " 处，最大回退 "
                    << Int64Text(timeline.reorder_depth) << " 个 POC 槽位（正常编码结构，非缺陷）\n";
            }
            if (timeline.duplicate_poc_count > 0)
                out << "      ❗ " << Int64Text(timeline.duplicate_poc_count) << " 处 POC 重复\n";
            if (timeline.poc_gap_slots > 0)
                out << "      ❗ 按 POC 步长推断缺 " << Int64Text(timeline.poc_gap_slots) << " 帧\n";
            if (timeline.truncated)
                out << "      逐帧明细已达上限，只保留前 " << Int64Text(static_cast<int64_t>(timeline.frames.size()))
                    << " 帧（统计量仍覆盖全程）\n";
            out << "      支持范围: 仅 H.264；pic_order_cnt_type=1 与 HEVC/AV1/VP9/VVC 不支持；"
                   "不重建参考帧列表\n";
        }
    }

    // ---- 逐轨音频 QC（阶段 4）----
    //
    // 主轨的数字在 metrics 里也有一份（兼容旧读者），但多语言/评论轨只有这里能看到。
    // 每条轨必须独立给出覆盖率：轨数对上了不代表每条轨都跑满了。
    if (!run.analysis.audio_qc_streams.empty()) {
        out << "\n--- 音频 QC（逐轨）---\n";
        out << "  共 " << run.analysis.audio_qc_tracks_analyzed << " / " << run.analysis.audio_qc_tracks_total
            << " 条音轨参与分析\n";
        for (std::size_t i = 0; i < run.analysis.audio_qc_streams.size(); ++i) {
            const model::AudioQcResult& qc = run.analysis.audio_qc_streams[i];
            const int stream_index =
                (i < run.analysis.audio_qc_stream_indices.size()) ? run.analysis.audio_qc_stream_indices[i] : -1;
            out << "  [流 #" << stream_index << "] ";
            if (!qc.analyzed) {
                out << "未能分析";
                if (!qc.notes.empty())
                    out << "：" << qc.notes.front();
                out << "\n";
                continue;
            }
            out << qc.metadata.channel_layout << " " << qc.metadata.sample_rate << " Hz ｜ 积分 "
                << Fixed(qc.integrated_lufs, 2) << " LUFS ｜ 真峰 " << Fixed(qc.true_peak_dbtp, 2) << " dBTP ｜ LRA "
                << Fixed(qc.loudness_range_lu, 2) << " LU\n";
            out << "      覆盖率 " << Fixed(qc.coverage_ratio * 100.0, 1) << "%（" << Fixed(qc.covered_seconds, 2)
                << " s）｜ 静音占比 " << Fixed(qc.silence_ratio * 100.0, 1) << "% ｜ 最长静音 "
                << Fixed(qc.longest_silence_seconds, 2) << " s\n";
            if (qc.correlation_available) {
                out << "      声道相关性 均值 " << Fixed(qc.correlation_mean, 3) << " / 最差 "
                    << Fixed(qc.correlation_min, 3) << " ｜ 反相块占比 " << Fixed(qc.out_of_phase_ratio * 100.0, 1)
                    << "%\n";
            }
            const int64_t errors = qc.decode_error_packets + qc.decode_error_frames + qc.dropped_frames;
            if (errors > 0)
                out << "      解码失败/丢帧合计 " << Int64Text(errors) << "\n";
            for (const std::string& note : qc.notes)
                out << "      说明: " << note << "\n";
        }
    }

    if (run.analysis.audio_qc.analyzed) {
        out << "\n--- 响度（主音轨）---\n";
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