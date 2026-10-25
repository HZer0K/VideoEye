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
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '&':
            out += "&amp;";
            break;
        case '\"':
            out += "&quot;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

// 文件偏移用十六进制显示（与十六进制编辑器/日志对齐）；无偏移时给 "-"。
std::string OffsetText(long long offset) {
    if (offset < 0)
        return std::string("-");
    std::ostringstream out;
    out << "0x" << std::hex << offset;
    return out.str();
}

} // namespace

// ===========================================================================
// HTML
// ===========================================================================

std::string QcReportExporter::BuildHtml(const QcExportBundle& bundle) {
    const qc::QcRunResult& run = bundle.run;
    const model::QcReport& report = run.report;

    auto severity_class = [](model::IssueSeverity severity) {
        switch (severity) {
        case model::IssueSeverity::Critical:
            return "critical";
        case model::IssueSeverity::Error:
            return "error";
        case model::IssueSeverity::Warning:
            return "warning";
        case model::IssueSeverity::Info:
            return "info";
        }
        return "info";
    };

    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
        << "  <meta charset=\"UTF-8\">\n  <title>VideoEye QC 报告 - " << Html(report.file_name) << "</title>\n"
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

    const std::string verdict_class = (report.score >= 90.0) ? "pass" : ((report.score >= 70.0) ? "warn" : "fail");

    out << "  <h1>VideoEye QC 报告</h1>\n";
    out << "  <p class=\"meta\">模板: " << Html(bundle.profile_name) << " (" << Html(bundle.profile_id)
        << ") ｜ 生成时间: " << Html(report.generated_at) << " ｜ 分析耗时: " << Fixed(run.elapsed_ms, 0) << " ms"
        << (report.completed ? (report.partial ? " ｜ <b>抽样完成·非全量</b>" : "") : " ｜ <b>分析未跑完</b>")
        << "</p>\n";

    out << "  <h2>总体结论</h2>\n";
    out << "  <p class=\"score " << verdict_class << "\">" << Fixed(report.score, 1) << " / 100 — "
        << Html(report.verdict) << "</p>\n";
    out << "  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
    out << "    <tr><td>文件</td><td>" << Html(report.file_path) << "</td></tr>\n";
    out << "    <tr><td>容器格式</td><td>" << Html(report.container_format) << "</td></tr>\n";
    out << "    <tr><td>时长</td><td>" << Fixed(report.duration_seconds, 3) << " s</td></tr>\n";
    out << "    <tr><td>文件大小</td><td>" << Int64Text(report.file_size_bytes) << " 字节</td></tr>\n";
    out << "    <tr><td>整体码率</td><td>" << Fixed(report.overall_bitrate_bps / 1000.0, 1) << " kbps</td></tr>\n";
    out << "    <tr><td>视频流 / 音频流</td><td>" << report.video_stream_count << " / " << report.audio_stream_count
        << "</td></tr>\n";
    out << "    <tr><td>致命 / 错误 / 警告 / 提示</td><td>" << report.CountBySeverity(model::IssueSeverity::Critical)
        << " / " << report.CountBySeverity(model::IssueSeverity::Error) << " / "
        << report.CountBySeverity(model::IssueSeverity::Warning) << " / "
        << report.CountBySeverity(model::IssueSeverity::Info) << "</td></tr>\n";
    if (!run.error.empty())
        out << "    <tr><td>错误信息</td><td>" << Html(run.error) << "</td></tr>\n";
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

    // ---- 证据与覆盖（阶段 5）----
    //
    // 放在流列表**之前**：读者要先知道"哪些维度真跑了、覆盖多少"，
    // 才知道后面那些数字能推到什么结论。
    if (!run.analysis.dimensions.empty()) {
        out << "  <h2>证据与覆盖</h2>\n";
        out << "  <p class=\"meta\">“结论可作通过用”是唯一权威口径：只有该维度确实执行、"
               "能力完整、无失败且覆盖达标时才成立。未执行的行会写明原因。</p>\n";
        out << "  <table>\n    <tr><th>维度</th><th>状态</th><th>能力</th><th>结论可用</th>"
               "<th>处理对象</th><th>扫描 / 总量</th><th>覆盖率</th><th>失败</th><th>说明</th></tr>\n";
        for (const model::DimensionStatus& status : run.analysis.dimensions) {
            const std::string scope = status.scope_unit.empty() ? std::string("-") : status.scope_unit;
            const std::string units = status.has_coverage
                                          ? (Int64Text(status.units_scanned) + " / " + Int64Text(status.units_total))
                                          : (Int64Text(status.units_scanned) + " / 未知");
            const std::string coverage =
                status.has_coverage ? (Fixed(status.CoveragePercent(), 1) + " %") : std::string("-");
            std::string remark = status.failure_reason;
            for (const std::string& note : status.notes) {
                if (!remark.empty())
                    remark += "；";
                remark += note;
            }
            out << "    <tr" << (status.executed ? "" : " class=\"warn\"") << "><td>"
                << Html(model::DimensionName(status.dimension)) << "</td>"
                << "<td>" << (status.executed ? "已执行" : "未执行") << "</td>"
                << "<td>" << Html(model::CapabilityToString(status.capability)) << "</td>"
                << "<td>" << (status.conclusive ? "是" : "否") << "</td>"
                << "<td>" << Html(scope) << "</td>"
                << "<td>" << Html(units) << "</td>"
                << "<td>" << Html(coverage) << "</td>"
                << "<td>" << Int64Text(status.failures) << "</td>"
                << "<td>" << Html(remark) << "</td></tr>\n";
        }
        out << "  </table>\n";
    }

    // ---- 参数变化时间线（阶段 1）----
    if (!run.analysis.parameter_timelines.empty()) {
        out << "  <h2>参数变化时间线</h2>\n";
        for (const model::ParameterTimeline& timeline : run.analysis.parameter_timelines) {
            out << "  <h3>视频流 #" << timeline.stream_index;
            if (!timeline.codec_name.empty())
                out << "（" << Html(timeline.codec_name) << "）";
            out << "</h3>\n";
            out << "  <p class=\"meta\">" << Html(timeline.Summary()) << " ｜ 扫描 "
                << Int64Text(timeline.packets_scanned) << " 个包，见到 " << Int64Text(timeline.parameter_set_units)
                << " 份参数集";
            if (timeline.truncated)
                out << "（已达追踪上限，后面的变化未记录）";
            out << "</p>\n";
            if (timeline.missing_reference_count > 0) {
                out << "  <p class=\"meta\"><strong>有 " << Int64Text(timeline.missing_reference_count)
                    << " 处引用了从未传输的参数集</strong>（解码必然失败）。</p>\n";
            }
            if (!timeline.changes.empty()) {
                out << "  <table>\n    <tr><th>时间</th><th>来源</th><th>变化前</th><th>变化后</th>"
                       "<th>差异</th><th>包号</th><th>文件偏移</th><th>同 id 替换</th></tr>\n";
                for (const model::ParameterChangeEvent& event : timeline.changes) {
                    out << "    <tr" << (event.replaced_same_id ? " class=\"warn\"" : "") << ">"
                        << "<td>" << (event.has_time ? (Fixed(event.time_seconds, 3) + " s") : std::string("无 PTS"))
                        << "</td>"
                        << "<td>" << Html(event.source) << "</td>"
                        << "<td>" << Html(event.old_config.empty() ? std::string("-") : event.old_config) << "</td>"
                        << "<td>" << Html(event.new_config.empty() ? std::string("-") : event.new_config) << "</td>"
                        << "<td>" << Html(event.changes) << "</td>"
                        << "<td>" << Int64Text(event.packet_index) << "</td>"
                        << "<td>" << OffsetText(static_cast<long long>(event.file_offset)) << "</td>"
                        << "<td>" << (event.replaced_same_id ? "是（危险）" : "否") << "</td></tr>\n";
                }
                out << "  </table>\n";
            } else {
                out << "  <p>未检测到配置变化。</p>\n";
            }
            for (const std::string& note : timeline.notes)
                out << "  <p class=\"meta\">" << Html(note) << "</p>\n";
        }
    }

    // ---- 帧级显示顺序（POC，仅 H.264）----
    //
    // HTML 报告是给人快速扫的，所以这里给统计表 + 前若干帧的解码/显示对照，
    // 全量逐帧明细仍以 JSON 为准（几千行塞进 HTML 没人看，还会拖慢打开）。
    if (!run.analysis.frame_order_timelines.empty()) {
        out << "  <h2>帧级显示顺序（POC）</h2>\n";
        out << "  <p class=\"meta\">来自逐 slice 解析出的 POC。支持范围：H.264 与 HEVC；"
               "AV1/VP9/VVC 未实现，H.264 的 pic_order_cnt_type=1 不支持；不重建参考帧列表。</p>\n";
        for (const model::FrameOrderTimeline& timeline : run.analysis.frame_order_timelines) {
            out << "  <h3>视频流 #" << timeline.stream_index << "</h3>\n";
            if (!timeline.available) {
                out << "  <p class=\"meta\">不可用 —— "
                    << Html(timeline.unavailable_reason.empty() ? std::string("原因未记录")
                                                                : timeline.unavailable_reason)
                    << "</p>\n";
                continue;
            }
            out << "  <p class=\"meta\">" << Html(timeline.Summary()) << "</p>\n";
            out << "  <table>\n    <tr><th>帧数</th><th>I</th><th>P</th><th>B</th><th>IDR</th>"
                   "<th>GOP 段数</th><th>最短/最长/平均</th><th>重排处数</th><th>最大回退(POC)</th>"
                   "<th>POC 重复</th><th>推断缺帧</th></tr>\n";
            out << "    <tr"
                << ((timeline.duplicate_poc_count > 0 || timeline.poc_gap_slots > 0) ? " class=\"warn\"" : "")
                << "><td>" << Int64Text(timeline.frames_observed) << "</td>"
                << "<td>" << timeline.i_count << "</td>"
                << "<td>" << timeline.p_count << "</td>"
                << "<td>" << timeline.b_count << "</td>"
                << "<td>" << timeline.idr_count << "</td>"
                << "<td>" << timeline.gop_count << "</td>"
                << "<td>" << Int64Text(timeline.min_gop_frames) << " / " << Int64Text(timeline.max_gop_frames) << " / "
                << Fixed(timeline.avg_gop_frames, 1) << "</td>"
                << "<td>" << Int64Text(timeline.reorder_events) << "</td>"
                << "<td>" << Int64Text(timeline.reorder_depth) << "</td>"
                << "<td>" << Int64Text(timeline.duplicate_poc_count) << "</td>"
                << "<td>" << Int64Text(timeline.poc_gap_slots) << "</td></tr>\n";
            out << "  </table>\n";

            // 前若干帧的解码顺序 vs 显示顺序对照：这是"POC 到底对不对"最直观的呈现
            // —— 解码顺序里 POC 回退的那一帧，就是显示时要往回插的 B 帧。
            constexpr std::size_t kMaxListedFrames = 40;
            if (!timeline.frames.empty()) {
                out << "  <table>\n    <tr><th>解码序</th><th>类型</th><th>frame_num</th><th>POC</th>"
                       "<th>包号</th><th>时间</th></tr>\n";
                for (std::size_t i = 0; i < timeline.frames.size() && i < kMaxListedFrames; ++i) {
                    const model::FrameOrderEntry& entry = timeline.frames[i];
                    const bool reorder = i > 0 && entry.poc < timeline.frames[i - 1].poc;
                    out << "    <tr" << (reorder ? " class=\"warn\"" : "") << "><td>" << i << "</td><td>"
                        << entry.TypeChar() << (entry.idr ? "(IDR)" : "") << "</td><td>" << entry.frame_num
                        << "</td><td>" << Int64Text(entry.poc) << "</td><td>" << Int64Text(entry.packet_index)
                        << "</td><td>" << (entry.has_time ? (Fixed(entry.time_seconds, 3) + " s") : std::string("-"))
                        << "</td></tr>\n";
                }
                out << "  </table>\n";
                if (timeline.frames.size() > kMaxListedFrames) {
                    out << "  <p class=\"meta\">另有 "
                        << Int64Text(static_cast<int64_t>(timeline.frames.size() - kMaxListedFrames))
                        << " 帧未列出，完整明细见 JSON 报告的 frame_orders.frames。</p>\n";
                }
            }
        }
    }

    out << "  <h2>流列表</h2>\n";
    out << "  <table>\n    <tr><th>#</th><th>类型</th><th>编码</th><th>Profile</th>"
           "<th>分辨率 / 采样率</th><th>码率</th><th>帧数 / 包数</th></tr>\n";
    for (const auto& stream : run.analysis.streams) {
        // 封面图在流列表里是"视频"类型的一帧 MJPEG，但用户看到的不是画面
        const char* type =
            stream.attached_picture ? "封面" : (stream.IsVideo() ? "视频" : (stream.IsAudio() ? "音频" : "其他"));
        const std::string resolution =
            stream.IsVideo()
                ? (std::to_string(stream.width) + "x" + std::to_string(stream.height))
                : (std::to_string(stream.sample_rate) + " Hz / " + std::to_string(stream.channels) + " 声道");
        out << "    <tr><td>" << stream.index << "</td><td>" << type << "</td><td>" << Html(stream.codec_name)
            << "</td><td>" << Html(stream.profile_name) << "</td><td>" << Html(resolution) << "</td><td>"
            << Fixed(stream.bitrate_bps / 1000.0, 1) << " kbps</td><td>" << stream.frame_count << " / "
            << stream.packet_count << "</td></tr>\n";
    }
    out << "  </table>\n";

    // ---- 逐轨音频 QC（阶段 4）----
    //
    // 先出横向对照表（多语言/评论轨一眼可比），再给主轨的详细指标。
    if (!run.analysis.audio_qc_streams.empty()) {
        out << "  <h2>音频 QC（逐轨）</h2>\n";
        out << "  <p class=\"meta\">共 " << run.analysis.audio_qc_tracks_analyzed << " / "
            << run.analysis.audio_qc_tracks_total
            << " 条音轨参与分析。每条轨的覆盖率独立计算 —— 轨数对上不代表每条都跑满。</p>\n";
        out << "  <table>\n    <tr><th>流</th><th>声道布局</th><th>采样率</th><th>积分响度</th>"
               "<th>真峰值</th><th>LRA</th><th>静音占比</th><th>最长静音</th><th>覆盖率</th><th>失败/丢帧</th></tr>\n";
        for (std::size_t i = 0; i < run.analysis.audio_qc_streams.size(); ++i) {
            const model::AudioQcResult& qc = run.analysis.audio_qc_streams[i];
            const int stream_index =
                (i < run.analysis.audio_qc_stream_indices.size()) ? run.analysis.audio_qc_stream_indices[i] : -1;
            if (!qc.analyzed) {
                out << "    <tr class=\"warn\"><td>" << stream_index << "</td><td colspan=\"9\">未能分析"
                    << (qc.notes.empty() ? "" : "：" + Html(qc.notes.front())) << "</td></tr>\n";
                continue;
            }
            const int64_t errors = qc.decode_error_packets + qc.decode_error_frames + qc.dropped_frames;
            out << "    <tr><td>" << stream_index << "</td>"
                << "<td>" << Html(qc.metadata.channel_layout) << "</td>"
                << "<td>" << qc.metadata.sample_rate << " Hz</td>"
                << "<td>" << Fixed(qc.integrated_lufs, 2) << " LUFS</td>"
                << "<td>" << Fixed(qc.true_peak_dbtp, 2) << " dBTP</td>"
                << "<td>" << Fixed(qc.loudness_range_lu, 2) << " LU</td>"
                << "<td>" << Fixed(qc.silence_ratio * 100.0, 2) << " %</td>"
                << "<td>" << Fixed(qc.longest_silence_seconds, 2) << " s</td>"
                << "<td>" << Fixed(qc.coverage_ratio * 100.0, 1) << " %</td>"
                << "<td>" << Int64Text(errors) << "</td></tr>\n";
        }
        out << "  </table>\n";
    }

    if (run.analysis.audio_qc.analyzed) {
        out << "  <h2>响度与音频 QC（主音轨）</h2>\n  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
        out << "    <tr><td>积分响度</td><td>" << Fixed(run.analysis.audio_qc.integrated_lufs, 2)
            << " LUFS</td></tr>\n";
        out << "    <tr><td>真峰值</td><td>" << Fixed(run.analysis.audio_qc.true_peak_dbtp, 2) << " dBTP</td></tr>\n";
        out << "    <tr><td>响度范围 LRA</td><td>" << Fixed(run.analysis.audio_qc.loudness_range_lu, 2)
            << " LU</td></tr>\n";
        out << "    <tr><td>采样峰值</td><td>" << Fixed(run.analysis.audio_qc.sample_peak_dbfs, 2)
            << " dBFS</td></tr>\n";
        out << "    <tr><td>静音占比</td><td>" << Fixed(run.analysis.audio_qc.silence_ratio * 100.0, 2)
            << " %</td></tr>\n";
        out << "    <tr><td>削波样本</td><td>" << run.analysis.audio_qc.clipping_sample_count << "</td></tr>\n";
        out << "  </table>\n";
    }

    // MPEG-TS 传输层体检。为什么必须整体落进报告见 QcReportExporterJson.cpp 同节注释：
    // 规则只在命中时才有 detail，未命中什么都不留，所以"扫了多少包、是否扫完"要由
    // 报告自己说，否则 container.ts.* 的未命中无法区分"没问题"与"没证据"。
    if (run.analysis.ts_transport.applicable) {
        const model::TransportStreamInfo& ts = run.analysis.ts_transport;
        out << "  <h2>MPEG-TS 传输层</h2>\n";
        if (!ts.analyzed) {
            out << "  <p class=\"meta\">未拿到传输层结论，以下 container.ts.* 规则的未命中"
                   "只代表没有证据，不代表没有问题。";
            if (!ts.error_message.empty()) {
                out << " 原因: " << Html(ts.error_message);
            }
            out << "</p>\n";
        } else {
            out << "  <table>\n    <tr><th>指标</th><th>值</th></tr>\n";
            out << "    <tr><td>包长</td><td>" << ts.packet_size << " 字节</td></tr>\n";
            out << "    <tr><td>扫描范围</td><td>" << Int64Text(ts.scanned_packets) << " / "
                << Int64Text(ts.estimated_packets) << " 包 "
                << (ts.scan_complete ? "（已扫全文件）" : "（只扫了前若干包，结论不代表整个文件）") << "</td></tr>\n";
            out << "    <tr><td>节目 / 流 / 服务</td><td>" << ts.programs << " / " << ts.streams << " / " << ts.services
                << "</td></tr>\n";
            out << "    <tr><td>PAT / PMT / SDT</td><td>" << (ts.has_pat ? "有" : "无") << " / "
                << (ts.has_pmt ? "有" : "无") << " / " << (ts.has_sdt ? "有" : "无") << "</td></tr>\n";
            out << "    <tr><td>失步次数</td><td>" << Int64Text(ts.sync_loss) << "</td></tr>\n";
            out << "    <tr><td>连续计数错误</td><td>" << Int64Text(ts.continuity_errors) << "（占已扫包 "
                << Fixed(ts.ContinuityErrorPercent(), 3) << "%）</td></tr>\n";
            out << "    <tr><td>加扰包</td><td>" << Int64Text(ts.scrambled_packets) << "</td></tr>\n";
            out << "    <tr><td>最大 PCR 间隔</td><td>" << Fixed(ts.pcr_max_gap_ms, 1) << " ms"
                << (ts.pcr_max_gap_ms > 0.0 ? "" : "（未采到 PCR）") << "</td></tr>\n";
            // 载荷级：没扫过要明说，否则 0 会被读成"没有断流 / 每个 GOP 都有 IDR"。
            if (!ts.pes_payload_scanned) {
                out << "    <tr><td>PES 载荷级检查</td><td>未执行（非 H.264/HEVC，"
                       "或 scan_pes_payload 已关闭）</td></tr>\n";
            } else if (ts.pes_streams_checked <= 0) {
                out << "    <tr><td>PES 载荷级检查</td><td>没有可跟踪的 ES"
                       "（缺 PMT / 全加扰）</td></tr>\n";
            } else {
                out << "    <tr><td>PES 载荷级检查</td><td>跟踪 " << ts.pes_streams_checked << " 条 ES，识别到 "
                    << Int64Text(ts.idr_count) << " 个随机接入点</td></tr>\n";
                out << "    <tr><td>最长无 IDR</td><td>" << Fixed(ts.max_no_idr_interval_ms, 1) << " ms"
                    << (ts.max_no_idr_pid.empty() ? "" : ("（PID " + Html(ts.max_no_idr_pid) + "）"))
                    << (ts.video_pids_without_idr > 0
                            ? "，有 " + std::to_string(ts.video_pids_without_idr) + " 条视频 PID 整段没有 IDR"
                            : "")
                    << "</td></tr>\n";
                out << "    <tr><td>最大 PES 断档</td><td>" << Fixed(ts.max_pid_gap_ms, 1) << " ms"
                    << (ts.max_pid_gap_pid.empty() ? "" : ("（PID " + Html(ts.max_pid_gap_pid) + "）"))
                    << (ts.stalled_pids > 1 ? "，共 " + std::to_string(ts.stalled_pids) + " 个 PID 超阈值" : "")
                    << "</td></tr>\n";
            }
            out << "  </table>\n";
        }
    }

    if (bundle.has_comparison) {
        out << "  <h2>文件对比</h2>\n";
        out << "  <p class=\"meta\">原文件: " << Html(bundle.comparison.left_path)
            << " ｜ 对比文件: " << Html(bundle.comparison.right_path)
            << " ｜ 差异项: " << bundle.comparison.DifferentCount() << "</p>\n";
        out << "  <table>\n    <tr><th>分组</th><th>字段</th><th>原文件</th><th>对比文件</th>"
               "<th>差异</th></tr>\n";
        for (const auto& row : bundle.comparison.rows) {
            if (row.diff == qc::QcFieldDiff::Unavailable)
                continue;
            out << "    <tr><td>" << Html(row.group) << "</td><td>" << Html(row.field) << "</td><td>" << Html(row.left)
                << " " << Html(row.unit) << "</td><td>" << Html(row.right) << " " << Html(row.unit) << "</td><td>"
                << Html(qc::ToString(row.diff)) << "</td></tr>\n";
        }
        out << "  </table>\n";
    }

    out << "  <h2>规则快照</h2>\n";
    out << "  <table>\n    <tr><th>规则</th><th>类别</th><th>阈值</th><th>启用</th></tr>\n";
    for (const auto& rule : report.rules) {
        out << "    <tr><td>" << Html(rule.name) << " (" << Html(rule.id) << ")</td><td>"
            << Html(model::ToString(rule.category)) << "</td><td>" << Fixed(rule.threshold, 2) << " " << Html(rule.unit)
            << "</td><td>" << (rule.enabled ? "是" : "否") << "</td></tr>\n";
    }
    out << "  </table>\n";

    out << "  <p class=\"meta\">Generated by VideoEye 2.0 ｜ schema version " << kReportSchemaVersion
        << "</p>\n</body>\n</html>\n";
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
    out << "  <p class=\"meta\">原文件: " << Html(comparison.left_path)
        << "<br>对比文件: " << Html(comparison.right_path) << "<br>评分 " << Fixed(comparison.left_score, 1) << " → "
        << Fixed(comparison.right_score, 1) << " ｜ 差异项 " << comparison.DifferentCount() << " / "
        << comparison.AvailableRowCount() << "</p>\n";
    out << "  <table>\n    <tr><th>分组</th><th>字段</th><th>原文件</th><th>对比文件</th>"
           "<th>单位</th><th>判定</th><th>差值</th></tr>\n";
    for (const auto& row : comparison.rows) {
        const bool different = row.diff == qc::QcFieldDiff::Different;
        out << "    <tr" << (different ? " class=\"different\"" : "") << "><td>" << Html(row.group) << "</td><td>"
            << Html(row.field) << "</td><td>" << Html(row.left) << "</td><td>" << Html(row.right) << "</td><td>"
            << Html(row.unit) << "</td><td>" << Html(qc::ToString(row.diff)) << "</td><td>"
            << (row.numeric ? Fixed(row.delta, 3) : std::string()) << "</td></tr>\n";
    }
    out << "  </table>\n</body>\n</html>\n";
    return out.str();
}

bool QcReportExporter::ExportComparisonHtml(const std::string& path, const qc::QcComparison& comparison) {
    return WriteUtf8File(path, BuildComparisonHtml(comparison));
}

bool QcReportExporter::ExportBatchSummaryHtml(const std::string& path, const QcBatchSummaryInput& input) {
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
    out << "  <p class=\"meta\">目录: " << Html(input.root) << " ｜ 模板: " << Html(input.profile_name) << " ("
        << Html(input.profile_id) << ") ｜ 完成 " << input.succeeded << " / 失败 " << input.failed << " / 超时 "
        << input.timed_out << " / 取消 " << input.cancelled << " / 跳过 " << input.skipped << " ｜ 耗时 "
        << Fixed(input.elapsed_ms, 0) << " ms" << (input.completed ? "" : " ｜ <b>未完成（被取消）</b>") << "</p>\n";
    out << "  <table>\n    <tr><th>文件</th><th>状态</th><th>评分</th><th>结论</th>"
           "<th>致命</th><th>错误</th><th>警告</th><th>提示</th><th>耗时(ms)</th><th>输出</th></tr>\n";
    for (const auto& row : input.rows) {
        const bool bad = row.critical_count > 0 || row.error_count > 0;
        out << "    <tr" << (bad ? " class=\"bad\"" : "") << "><td>" << Html(row.path) << "</td><td>"
            << Html(row.status) << "</td><td>" << Fixed(row.score, 1) << "</td><td>" << Html(row.verdict) << "</td><td>"
            << row.critical_count << "</td><td>" << row.error_count << "</td><td>" << row.warning_count << "</td><td>"
            << row.info_count << "</td><td>" << Fixed(row.elapsed_ms, 0) << "</td><td>" << Html(row.output_path)
            << "</td></tr>\n";
    }
    out << "  </table>\n</body>\n</html>\n";
    return WriteUtf8File(path, out.str());
}

} // namespace reporting
} // namespace videoeye