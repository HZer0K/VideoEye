#include "core/qc/QcRuleEngineInternal.h"

#include <string>

namespace videoeye {
namespace qc {
namespace detail {

namespace {

// "Header 12 / Body 3 / Footer 1" —— 报告里让人一眼看出分区结构长什么样，
// 比只说"缺 Footer"更能说明问题出在哪一层。
std::string PartitionLayout(const model::MxfDiagnostics& mxf) {
    return "Header " + std::to_string(mxf.header_partitions) + " / Body " + std::to_string(mxf.body_partitions) +
           " / Footer " + std::to_string(mxf.footer_partitions);
}

} // namespace

// MXF（container.mxf.*）。事实来自 MxfStructureAnalyzer 的顺序 KLV 扫描：
// 只读 16 字节键与 BER 长度、载荷 Seek 跳过，内存 O(1)。
//
// 与 MKV 那组规则的差别在"结论从哪来"：
//   * Footer Partition / Index Table 靠正向扫描（QC 走全文件，不看结构页的 64 MiB 窗口）；
//   * Random Index Pack 靠**反向读文件尾**，所以即使文件巨大也一定看得见。
//
// 调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue>
CheckMxfContainerRules(const model::QcRule& rule, const model::AnalysisResult& result, const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::MxfDiagnostics& mxf = result.container_diagnostics.mxf;
    if (!mxf.analyzed)
        return issues;

    if (rule.id == "container.mxf.missing_footer_partition") {
        // Footer 在文件尾：没扫完就不能断言它不存在。
        // 截断文件另有 truncated_klv 记账，也不在这里重复扣分。
        if (!mxf.scan_complete || mxf.truncated || mxf.footer_partitions > 0)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "文件有分区结构（" + PartitionLayout(mxf) +
                                                   "）但没有 Footer 分区：写入在中途被打断，文件没有正常收尾。"
                                                   "部分摄录设备与后期系统会拒绝导入，或只能按破损素材做只读抢救。"));
        }
        return issues;
    }

    if (rule.id == "container.mxf.missing_rip") {
        // RIP 是文件尾探测出来的事实，与扫描窗口无关，不需要 scan_complete 门控。
        // 但截断文件本来就把 RIP 截掉了，那是 truncated_klv 的账，不重复扣分。
        if (mxf.has_rip || mxf.truncated)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "文件尾没有 Random Index Pack（RIP）。RIP 记录每个分区的位置，"
                                               "缺了它播放器与后期系统必须从文件头线性扫描才能定位分区，"
                                               "打开与随机访问都会明显变慢。"));
        }
        return issues;
    }

    if (rule.id == "container.mxf.missing_index") {
        // 没有 Index Table 这个结论要求：扫到片尾、文件没被截断、且确实有媒体数据。
        if (!mxf.scan_complete || mxf.truncated || mxf.index_segments > 0 || mxf.essence_objects == 0)
            return issues;
        // 「缺失」类规则的度量就是 1.0（命中）—— 与 container.mkv.missing_cues 同一口径。
        // 用 index_segments 当度量配 MaxExceeded>0 会让"0 个索引"永远不触发。
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "扫完整个文件（" + std::to_string(mxf.essence_objects) +
                                                   " 个 Essence KLV）都没有找到 Index Table Segment。"
                                                   "精确定位依赖索引表，缺了它只能按近似帧率估算位置，"
                                                   "拖动进度条容易落到非关键帧，逐帧预览明显变慢。"));
        }
        return issues;
    }

    if (rule.id == "container.mxf.missing_primer") {
        // 只有"文件从 Header 分区开始"时 Primer 才是必须的：
        // 从 Body 分区截出来的一段素材本来就没有头，不算问题。
        if (mxf.has_primer || mxf.first_partition_kind != 0x02)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "Header 分区里没有 Primer Pack。Primer 是「动态 UL → 标签」的映射表，"
                                               "缺了它描述符里的动态标签无法解析，部分后期系统读不到编码、"
                                               "分辨率、帧率等基本信息。"));
        }
        return issues;
    }

    if (rule.id == "container.mxf.truncated_klv") {
        // 只报"越过文件末尾"，不报"越过扫描窗口" —— 后者对正常文件也会发生。
        if (!mxf.truncated)
            return issues;
        const double count = static_cast<double>(mxf.truncated_values);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(count, "有 KLV 声明的长度越过文件末尾，末尾数据缺失"
                                               "（共 " +
                                                   std::to_string(mxf.truncated_values) +
                                                   " 处，拷贝/传输/录制中断）。"));
        }
        return issues;
    }

    if (rule.id == "container.mxf.resync") {
        const double count = static_cast<double>(mxf.resyncs);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(count, "解析时有 " + FormatValue(count, 0) +
                                                   " 次键前缀失配（本该是 06 0E 2B 34 的 SMPTE 键位置不是），"
                                                   "字节流里混入了垃圾或结构被破坏。"));
        }
        return issues;
    }

    if (rule.id == "container.mxf.scan_incomplete") {
        if (mxf.scan_complete)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "体检没能扫到文件尾（已解析 " + std::to_string(mxf.essence_objects) +
                                                   " 个 Essence KLV），Footer / Index Table 等结论"
                                                   "只覆盖已扫描的部分。"));
        }
        return issues;
    }

    return issues;
}

} // namespace detail
} // namespace qc
} // namespace videoeye
