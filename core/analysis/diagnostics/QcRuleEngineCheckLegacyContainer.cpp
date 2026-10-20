#include "core/analysis/diagnostics/QcRuleEngineInternal.h"

#include <string>

namespace videoeye {
namespace diagnostics {
namespace detail {

// AVI（container.avi.*）。调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
//
// 共同前提：avi.analyzed 为假时不报 —— "没解析过"既不是"没问题"也不是"有问题"。
//
// 口径说明（B2）：这组规则读的是 AviStructureAnalyzer 在解析 RIFF 块时顺手记下的
// 索引事实（idx1 / indx / dmlh），不是 FFmpeg demux 的结论。传统 AVI 的索引只覆盖
// 首个 RIFF（<= 4 GB），OpenDML 用 'AVIX' + dmlh + indx 扩展；任何一环缺失，
// 表现都是"能打开、能播、但一拖进度条就错位或卡住"。
std::vector<model::DiagnosticIssue> CheckAviContainerRules(const model::QcRule& rule,
                                                           const model::AnalysisResult& result,
                                                           const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::AviDiagnostics& avi = result.container_diagnostics.avi;
    if (!avi.analyzed) return issues;

    if (rule.id == "container.avi.riff_size_exceeds_file") {
        if (avi.riff_size_exceeds_file) {
            issues.push_back(make_issue(
                1.0, "顶级 RIFF 声明的长度超过文件实际大小，文件被截断"
                     "（录制中断 / 下载不全），末尾的媒体数据不完整。"));
        }
        return issues;
    }

    if (rule.id == "container.avi.missing_index") {
        // 只有"确实有媒体数据却没有索引"才算问题。
        // 空的/仅头的 AVI 不在这里报（那是别的规则的事）。
        if (!avi.has_idx1 && !avi.has_indx && avi.video_streams + avi.audio_streams > 0) {
            issues.push_back(make_issue(
                1.0, "未发现任何索引（既没有传统 idx1，也没有 OpenDML indx）。"
                     "没有索引只能顺序读取，拖动进度条需要从头扫描；"
                     "超过 4 GB 的文件 seek 会明显不准。"));
        }
        return issues;
    }

    if (rule.id == "container.avi.index_bad_offsets") {
        const double count = static_cast<double>(avi.idx1_bad_offsets);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "传统索引 idx1 中有 " + FormatValue(count, 0) +
                           " 个条目的偏移 + 长度越过文件末尾"
                           "（共 " + std::to_string(avi.idx1_entries) + " 项）。"
                           "seek 到这些帧会读到越界数据。"));
        }
        return issues;
    }

    if (rule.id == "container.avi.indx_truncated") {
        if (avi.indx_truncated) {
            issues.push_back(make_issue(
                1.0, "OpenDML 索引 indx 头声明的条目数超出该块实际长度，"
                     "索引被截断（共解析到 " + std::to_string(avi.indx_entries) + " 项）。"
                     "按索引跳转时会落到错误的偏移。"));
        }
        return issues;
    }

    if (rule.id == "container.avi.no_opendml") {
        // 有 AVIX 却没有 dmlh：OpenDML 结构不完整。
        if (avi.has_avix && !avi.has_dmlh) {
            issues.push_back(make_issue(
                1.0, "存在 OpenDML 扩展 RIFF（'AVIX'）但没有 dmlh 头。"
                     "超过 4 GB 的 AVI 依赖 dmlh 记录总帧数，"
                     "缺少它时跨 4 GB 边界的 seek 会失真。"));
        }
        return issues;
    }

    if (rule.id == "container.avi.truncated_chunks") {
        const double count = static_cast<double>(avi.truncated_chunks);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "有 " + FormatValue(count, 0) +
                           " 个 RIFF 块声明的长度越过文件末尾（数据不完整）。"
                           "通常只影响末尾，但被截断处的 seek 会不准。"));
        }
        return issues;
    }

    if (rule.id == "container.avi.zero_length_streams") {
        const double count = static_cast<double>(avi.zero_length_streams);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "有 " + FormatValue(count, 0) +
                           " 个音视频流的 strh.dwLength 为 0。"
                           "时长与帧数只能靠扫描推算，进度条与 seek 会失效。"));
        }
        return issues;
    }

    return issues;
}

// FLV（container.flv.*）。事实来自 FlvStructureAnalyzer 的 tag 级扫描。
//
// FLV 的结构约束很弱：tag 之间只靠 PreviousTagSize 串起来，时间戳是每类 tag
// 各自单调的。因此这一组只抓三类"能播但不可靠"的情形：
// 时序（时间戳回退）、可解码性（缺序列头）、完整性（截断 / 大小不符）。
std::vector<model::DiagnosticIssue> CheckFlvContainerRules(const model::QcRule& rule,
                                                           const model::AnalysisResult& result,
                                                           const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::FlvDiagnostics& flv = result.container_diagnostics.flv;
    if (!flv.analyzed) return issues;

    const std::string scope =
        flv.scan_complete
            ? std::string("（已扫描全文件）")
            : "（只扫描了前 " + std::to_string(flv.scanned_tags) + " 个 tag）";

    if (rule.id == "container.flv.missing_metadata") {
        // 有音视频但没有 onMetaData：能播，但拿不到时长/分辨率，
        // 点播播放器要扫完整文件才能出进度条。
        if (!flv.has_metadata && flv.video_tags + flv.audio_tags > 0 && flv.scan_complete) {
            issues.push_back(make_issue(
                1.0, "未发现 onMetaData 脚本标记，播放器无法直接获得时长/分辨率，"
                     "需要扫描完整文件才能显示进度条。"));
        }
        return issues;
    }

    if (rule.id == "container.flv.missing_avc_sequence_header") {
        const bool needs_sequence_header =
            flv.video_codec_avc || flv.video_codec_hevc || flv.video_codec_av1;
        if (needs_sequence_header && !flv.video_sequence_header_seen && flv.scan_complete) {
            issues.push_back(make_issue(
                1.0, "视频使用 H.264 / HEVC / AV1 编码，但全程没有发现 Sequence Start 包头，"
                     "缺少 SPS/PPS（或对应配置），解码器无法初始化，画面会黑屏。"));
        }
        return issues;
    }

    if (rule.id == "container.flv.timestamp_backwards") {
        const double count = static_cast<double>(flv.timestamp_backwards);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "同一类型 tag 内时间戳出现 " + FormatValue(count, 0) +
                           " 次回退" + scope + "。时序错乱会导致卡顿、音画不同步。"));
        }
        return issues;
    }

    if (rule.id == "container.flv.truncated_tags") {
        const double count = static_cast<double>(flv.truncated_tags);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "有 " + FormatValue(count, 0) +
                           " 个 tag 的完整数据越过文件末尾（文件被截断）。"
                           "末尾数据缺失，seek 到结尾可能卡住。"));
        }
        return issues;
    }

    if (rule.id == "container.flv.tag_size_mismatch") {
        const double count = static_cast<double>(flv.tag_size_mismatch);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "有 " + FormatValue(count, 0) +
                           " 处 PreviousTagSize 与实际 tag 长度不符" + scope + "。"
                           "严格校验的播放器会拒绝播放后续 tag。"));
        }
        return issues;
    }

    return issues;
}

// ASF / WMV / WMA（container.asf.*）。事实来自 AsfStructureAnalyzer 的 GUID 对象遍历。
//
// ASF 的"索引"是可选对象，缺索引本身不算错误（流式场景本来就没有），
// 所以这里不设 missing_index；真正影响可播性的是 Data Object 缺失与头被截断。
std::vector<model::DiagnosticIssue> CheckAsfContainerRules(const model::QcRule& rule,
                                                           const model::AnalysisResult& result,
                                                           const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::AsfDiagnostics& asf = result.container_diagnostics.asf;
    if (!asf.analyzed) return issues;

    if (rule.id == "container.asf.missing_data_object") {
        if (!asf.has_data_object) {
            issues.push_back(make_issue(
                1.0, "遍历整个文件后没有找到 ASF Data Object，"
                     "文件只有头/元数据，没有可播放的媒体数据。"));
        }
        return issues;
    }

    if (rule.id == "container.asf.header_object_size_invalid") {
        const double count = static_cast<double>(asf.invalid_object_sizes);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "有 " + FormatValue(count, 0) +
                           " 个 ASF 对象的 size 小于对象头（24 字节）或越过文件末尾。"
                           "这些对象及其后的解析都不可信。"));
        }
        return issues;
    }

    if (rule.id == "container.asf.header_size_exceeds_file") {
        if (asf.header_size_exceeds_file) {
            issues.push_back(make_issue(
                1.0, "Header Object 声明的长度超过文件实际大小，文件头被截断，"
                     "元数据与流属性可能不完整。"));
        }
        return issues;
    }

    if (rule.id == "container.asf.duration_invalid") {
        if (asf.duration_invalid) {
            issues.push_back(make_issue(
                1.0, "File Properties 解析出的时长为 0 或负值（" +
                         FormatValue(asf.duration_seconds, 3) + " 秒）。"
                         "常见于未正常收尾的录制文件，进度条 seek 不可用。"));
        }
        return issues;
    }

    return issues;
}

// OGG（container.ogg.*）。事实来自 OggStructureAnalyzer 的页级扫描。
//
// Ogg 的"索引"就是页头本身（serial + page_seq + granule），没有单独的索引对象。
// 因此结构性问题只有几种：页序号断裂（丢页）、缺 BOS/EOS（首尾截断）、
// 页数据越界（文件截断）、以及需要重新同步（中间损坏）。
std::vector<model::DiagnosticIssue> CheckOggContainerRules(const model::QcRule& rule,
                                                           const model::AnalysisResult& result,
                                                           const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::OggDiagnostics& ogg = result.container_diagnostics.ogg;
    if (!ogg.analyzed) return issues;

    const std::string scope =
        ogg.scan_complete
            ? std::string("（已扫描全文件）")
            : "（只扫描了前 " + std::to_string(ogg.pages) + " 页）";

    if (rule.id == "container.ogg.page_sequence_gap") {
        const double count = static_cast<double>(ogg.sequence_gaps);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "逻辑流的页序号出现 " + FormatValue(count, 0) +
                           " 处不连续（共 " + std::to_string(ogg.pages) + " 页）" + scope +
                           "。丢页或拼接错位会让对应位置的数据缺失。"));
        }
        return issues;
    }

    if (rule.id == "container.ogg.missing_bos") {
        const double count = static_cast<double>(ogg.missing_bos);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "有 " + FormatValue(count, 0) +
                           " 个逻辑流的第一个页面不是 BOS，文件开头被截断，"
                           "缺少 codec 标识头，解码器无法初始化。"));
        }
        return issues;
    }

    if (rule.id == "container.ogg.missing_eos") {
        // 只扫了窗口时不下结论：窗口之外完全可能还有 EOS。
        if (ogg.scan_complete) {
            const double count = static_cast<double>(ogg.missing_eos);
            if (Triggered(rule, count)) {
                issues.push_back(make_issue(
                    count, "有 " + FormatValue(count, 0) +
                               " 个逻辑流扫描到文件末尾仍未出现 EOS 标记，"
                               "文件没有正常收尾（截断 / 录制中断）。"));
            }
        }
        return issues;
    }

    if (rule.id == "container.ogg.truncated_pages") {
        const double count = static_cast<double>(ogg.truncated_pages);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "有 " + FormatValue(count, 0) +
                           " 个页的 header/segment table/数据越过文件末尾，"
                           "页数据不完整。"));
        }
        return issues;
    }

    if (rule.id == "container.ogg.resyncs") {
        const double count = static_cast<double>(ogg.resyncs);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(
                count, "解析过程中 " + FormatValue(count, 0) +
                           " 次找不到 \"OggS\" 同步码并重新对齐" + scope + "。"
                           "偶发一次通常是文件中间被改写；持续出现说明容器损坏。"));
        }
        return issues;
    }

    return issues;
}

}  // namespace detail
}  // namespace diagnostics
}  // namespace videoeye
