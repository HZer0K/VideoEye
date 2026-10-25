#include "core/qc/QcRuleEngineInternal.h"

#include <string>

namespace videoeye {
namespace qc {
namespace detail {

// MPEG-TS 传输层（container.ts.*）。调用方（CheckRule 前缀路由）保证 rule.id
// 属于本类，故此处不再做前缀判断。
//
// 共同前提：ts_transport.analyzed 为假时一律不报 —— "没跑过"既不是"没问题"、
// 也不是"有问题"。拿不到证据就不该出现在问题表里；报告里由"未执行"那类说明承担。
//
// 口径：所有计数都只覆盖扫描窗口。detail 里必须带出"扫了多少 / 共多少"，
// 否则用户会把"前 20 万包没问题"读成"整条流没问题"——这正是这一层最容易骗人的地方。
std::vector<model::DiagnosticIssue>
CheckTsTransportRules(const model::QcRule& rule, const model::AnalysisResult& result, const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::TransportStreamInfo& ts = result.ts_transport;
    if (!ts.analyzed)
        return issues;

    const std::string scope = ts.scan_complete ? std::string("（已扫描全文件）")
                                               : "（只扫描了前 " + std::to_string(ts.scanned_packets) + " 包，共约 " +
                                                     std::to_string(ts.estimated_packets) + " 包）";

    if (rule.id == "container.ts.missing_pat") {
        if (!ts.has_pat) {
            issues.push_back(make_issue(1.0, "未解析到 PAT（节目关联表），无法得知节目与 PMT PID" + scope +
                                                 "。多数机顶盒/播放器会直接判定为无效流。"));
        }
        return issues;
    }

    if (rule.id == "container.ts.missing_pmt") {
        if (ts.has_pat && !ts.has_pmt) {
            issues.push_back(make_issue(1.0, "PAT 有声明但未解析到 PMT（节目映射表），流表不完整" + scope + "。"));
        }
        return issues;
    }

    if (rule.id == "container.ts.sync_loss") {
        const double count = static_cast<double>(ts.sync_loss);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(count, "传输流失步 " + FormatValue(count, 0) +
                                                   " 次（同步字节 0x47 丢失后重新对齐）" + scope +
                                                   "。常见于录制中断、切片拼接、传输损伤。"));
        }
        return issues;
    }

    if (rule.id == "container.ts.continuity_error") {
        // 用错误率而不是绝对条数：长文件的偶发丢包与短文件的严重损毁不该同罪。
        const double percent = ts.ContinuityErrorPercent();
        if (Triggered(rule, percent)) {
            issues.push_back(make_issue(percent, "连续计数错误 " + std::to_string(ts.continuity_errors) +
                                                     " 次，占已扫包 " + FormatValue(percent, 3) +
                                                     "%（丢包 / 乱序 / 拼接点）" + scope + "。"));
        }
        return issues;
    }

    if (rule.id == "container.ts.pcr_gap") {
        // 没采到 PCR（无 PCR PID / 加扰 / 只有纯音频流）时不拿 0 去和阈值比，
        // 否则"没数据"会被报成"间隔良好"或被误判成异常。
        if (ts.pcr_max_gap_ms <= 0.0)
            return issues;
        if (Triggered(rule, ts.pcr_max_gap_ms)) {
            issues.push_back(
                make_issue(ts.pcr_max_gap_ms, "最大 PCR 间隔 " + FormatValue(ts.pcr_max_gap_ms, 1) + " ms" + scope +
                                                  "。PCR 稀疏会让解码端缓冲与时钟恢复失控（DVB 要求远小于此值）。"));
        }
        return issues;
    }

    if (rule.id == "container.ts.scrambled") {
        if (ts.scrambled_packets <= 0)
            return issues;
        const double count = static_cast<double>(ts.scrambled_packets);
        const double percent = (ts.scanned_packets > 0) ? 100.0 * count / static_cast<double>(ts.scanned_packets) : 0.0;
        issues.push_back(make_issue(count, "加扰包 " + std::to_string(ts.scrambled_packets) + " 个，占已扫包 " +
                                               FormatValue(percent, 2) + "%" + scope +
                                               "。加扰本身不算错误（解复用不受影响），但 ES 载荷不可读，"
                                               "编码参数与码流类结论不可信。"));
        return issues;
    }

    if (rule.id == "container.ts.scan_incomplete") {
        const double incomplete = ts.scan_complete ? 0.0 : 1.0;
        if (Triggered(rule, incomplete)) {
            issues.push_back(make_issue(incomplete, "TS 传输层只扫描了前 " + std::to_string(ts.scanned_packets) +
                                                        " 包（共约 " + std::to_string(ts.estimated_packets) +
                                                        " 包）。失步 / 连续计数 / PCR 结论只代表扫描窗口，"
                                                        "不代表整个文件。"));
        }
        return issues;
    }

    // ---- PES 载荷级 ----
    //
    // 这两条的证据来自"扫 ES 载荷"这一趟（TsStructureOptions::scan_pes_payload）。
    // 没扫过时 pes_payload_scanned 为假，一律不报：没证据不等于没问题。
    if (rule.id == "container.ts.no_idr") {
        if (!ts.pes_payload_scanned)
            return issues;
        // 没有任何视频 PID 被识别（纯音频流 / 全加扰 / 非 H.264|HEVC）时无从判定
        if (ts.max_no_idr_interval_ms <= 0.0)
            return issues;
        if (!Triggered(rule, ts.max_no_idr_interval_ms))
            return issues;
        std::string detail = "视频流最长的一段有 " + FormatValue(ts.max_no_idr_interval_ms, 1) + " ms 没有任何 IDR/CRA";
        if (!ts.max_no_idr_pid.empty())
            detail += "（PID " + ts.max_no_idr_pid + "）";
        detail += "，全窗口共识别到 " + std::to_string(ts.idr_count) + " 个随机接入点";
        if (ts.video_pids_without_idr > 0)
            detail += "，其中 " + std::to_string(ts.video_pids_without_idr) + " 条视频 PID 整段没有 IDR";
        detail += scope + "。这一段既不能起播也不能 seek。";
        issues.push_back(make_issue(ts.max_no_idr_interval_ms, detail));
        return issues;
    }

    if (rule.id == "container.ts.pid_stall") {
        if (!ts.pes_payload_scanned)
            return issues;
        if (ts.max_pid_gap_ms <= 0.0)
            return issues;
        if (!Triggered(rule, ts.max_pid_gap_ms))
            return issues;
        std::string detail = "有一条 PID 的时间戳断档达到 " + FormatValue(ts.max_pid_gap_ms, 1) + " ms";
        if (!ts.max_pid_gap_pid.empty())
            detail += "（PID " + ts.max_pid_gap_pid + "）";
        if (ts.stalled_pids > 1)
            detail += "，共 " + std::to_string(ts.stalled_pids) + " 个 PID 超过阈值";
        // 断档有两种形态：相邻两个 PES 之间，以及"这条流比其它流早停"（尾部缺口）。
        // 后者查不出来就等于放过最严重的断流，因此 detail 里必须两种都覆盖。
        detail += scope + "。断档可能是相邻 PES 之间，也可能是这条流比其它流提前结束。"
                          "表现为画面冻住、声音中断，或某条流后半段消失。";
        issues.push_back(make_issue(ts.max_pid_gap_ms, detail));
        return issues;
    }

    return issues;
}

} // namespace detail
} // namespace qc
} // namespace videoeye
