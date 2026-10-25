#include "core/qc/QcRuleEngineInternal.h"

#include <string>

namespace videoeye {
namespace qc {
namespace detail {

// MPEG-PS（container.ps.*）。事实来自 MpegPsStructureAnalyzer 的起始码顺序扫描。
//
// 三条口径不能省：
//   0. 不判"缺少 Program End Code（00 00 01 B9）"。ffmpeg 的 mpeg/vob 复用器从不写它，
//      DVD 的 VOB 按规范也不写（它们是更长节目链的一段）—— 这条规则会对几乎所有真实
//      PS 文件误报。切片中断由 truncated_start_code 兜住：文件被切在任意位置时，
//      切口必然落在某个元素内部（元素首尾相接），起始码长度就会越过文件末尾。
//   1. 单调性只认 DTS。PS 的 PES 按解码序排列，有 B 帧时相邻 PTS 本来就回退一个 GOP；
//      分析器已经保证只把 DTS（或纯音频流的 PTS）算进 timestamp_backwards，
//      规则层直接用它，不要改成 PTS。
//   2. 缺时间戳必须与"解析器认不出时间戳"区分开。门控是"同文件里别的流解析出了
//      时间戳"（streams_with_timestamp > 0）：一个时间戳都解析不出来的文件，
//      说明不了是流缺时间戳还是解析失败，宁可不报。
//
// 调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue>
CheckPsContainerRules(const model::QcRule& rule, const model::AnalysisResult& result, const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::MpegPsDiagnostics& ps = result.container_diagnostics.mpeg_ps;
    if (!ps.analyzed)
        return issues;

    if (rule.id == "container.ps.truncated_start_code") {
        const double count = static_cast<double>(ps.truncated_start_codes);
        if (Triggered(rule, count)) {
            issues.push_back(
                make_issue(count, "有 " + FormatValue(count, 0) + " 个起始码声明的包长度越过文件末尾，文件被截断。"));
        }
        return issues;
    }

    if (rule.id == "container.ps.timestamp_backwards") {
        const double count = static_cast<double>(ps.timestamp_backwards);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(count, "同一流内 DTS 出现 " + FormatValue(count, 0) +
                                                   " 次回退。PS 按解码序排列，DTS 必须单调非降；"
                                                   "回退意味着时间轴错乱，播放会卡顿、音画不同步，"
                                                   "多段拼接的文件尤其常见。"));
        }
        return issues;
    }

    if (rule.id == "container.ps.stream_without_timestamp") {
        // 一个时间戳都没解析出来的文件说明不了问题 —— 见文件头口径 2。
        if (ps.streams_with_timestamp == 0)
            return issues;
        const double count = static_cast<double>(ps.streams_without_timestamp);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(count, "有 " + FormatValue(count, 0) + " 条视频/音频流一个时间戳都没有（其余 " +
                                                   std::to_string(ps.streams_with_timestamp) +
                                                   " 条流正常）。没有 PTS/DTS 播放器只能按帧率推算位置，"
                                                   "起播偏移与音画同步都会逐渐漂移。"));
        }
        return issues;
    }

    if (rule.id == "container.ps.stream_map_mismatch") {
        // 没有 PSM 的文件谈不上"对账"，那时流表只能靠 stream_id 推断。
        if (ps.psm_count == 0 || !ps.scan_complete)
            return issues;
        const double count = static_cast<double>(ps.unmapped_streams + ps.declared_but_missing_streams);
        if (Triggered(rule, count)) {
            std::string detail = "PSM 声明的基本流与实际 PES 对不上：";
            if (ps.unmapped_streams > 0)
                detail += std::to_string(ps.unmapped_streams) + " 条流没有被 PSM 声明";
            if (ps.unmapped_streams > 0 && ps.declared_but_missing_streams > 0)
                detail += "，";
            if (ps.declared_but_missing_streams > 0)
                detail += std::to_string(ps.declared_but_missing_streams) + " 条声明了却没有任何数据";
            detail += "。播放器只能按 stream_id 猜编码，猜错时会解不出画面或声音。";
            issues.push_back(make_issue(count, detail));
        }
        return issues;
    }

    if (rule.id == "container.ps.scan_incomplete") {
        if (ps.scan_complete)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "只扫描了文件的前一部分（已解析 " + std::to_string(ps.pes_packets) +
                                                   " 个 PES），End Code / 时间戳回退等结论"
                                                   "只代表已扫描的部分。"));
        }
        return issues;
    }

    return issues;
}

} // namespace detail
} // namespace qc
} // namespace videoeye
