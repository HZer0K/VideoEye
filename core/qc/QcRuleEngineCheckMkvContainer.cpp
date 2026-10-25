#include "core/qc/QcRuleEngineInternal.h"

#include <string>

namespace videoeye {
namespace qc {
namespace detail {

// MKV / WebM（container.mkv.*）。事实来自 MkvContainerScanner 的元素级顺序扫描：
// 只读元素头、不建树，内存 O(1)，因此这些结论覆盖整份文件 —— 与结构页那棵
// 受 kMaxNodes 约束的元素树不是一回事。
//
// 调用方（CheckRule 前缀路由）保证 rule.id 属于本类。
std::vector<model::DiagnosticIssue>
CheckMkvContainerRules(const model::QcRule& rule, const model::AnalysisResult& result, const IssueFactory& make_issue) {
    std::vector<model::DiagnosticIssue> issues;
    const model::MkvDiagnostics& mkv = result.container_diagnostics.mkv;
    if (!mkv.analyzed)
        return issues;

    const std::string container = mkv.doc_type.empty() ? "MKV" : mkv.doc_type;
    const std::string scope = mkv.scan_complete ? std::string("（已扫描全文件）")
                                                : "（只扫描了前 " + std::to_string(mkv.clusters) + " 个 Cluster）";

    if (rule.id == "container.mkv.missing_cues") {
        // 没有 Cues 这个结论必须建立在"扫到过片尾"上：截断文件天然没有索引，
        // 那是 container.mkv.truncated 的账，不要在这里重复扣分。
        // 直播录制（时长未知）也不判 —— 边写边发的文件本来就不写 Cues。
        if (!mkv.scan_complete || mkv.truncated || mkv.has_cues || result.duration_seconds <= 0.0)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "整个文件扫完没有找到 Cues 索引（" + container + "，" +
                                                   std::to_string(mkv.clusters) +
                                                   " 个 Cluster）。"
                                                   "播放器与剪辑软件只能线性扫描定位，拖进度条和逐帧预览会明显变慢，"
                                                   "部分工具会直接判定为不可 seek。"));
        }
        return issues;
    }

    if (rule.id == "container.mkv.cluster_timestamp_backwards") {
        const double count = static_cast<double>(mkv.cluster_timestamp_backwards);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(count, "相邻 Cluster 的时间戳出现 " + FormatValue(count, 0) + " 处回退（共 " +
                                                   std::to_string(mkv.clusters) + " 个 Cluster" + scope +
                                                   "）。"
                                                   "时间轴非单调会让播放卡顿、音画不同步，seek 落点也会错位。"));
        }
        return issues;
    }

    if (rule.id == "container.mkv.missing_codec_private") {
        const double count = static_cast<double>(mkv.tracks_missing_codec_private);
        if (Triggered(rule, count)) {
            std::string detail = "有 " + FormatValue(count, 0) +
                                 " 条轨道缺少 CodecPrivate，解码器拿不到参数集，会出现「轨道识别到了但黑屏/无声」";
            if (!mkv.missing_codec_private_codecs.empty())
                detail += "（" + mkv.missing_codec_private_codecs + "）";
            detail += "。";
            issues.push_back(make_issue(count, detail));
        }
        return issues;
    }

    if (rule.id == "container.mkv.unknown_size_element") {
        const double count = static_cast<double>(mkv.unknown_size_elements);
        if (Triggered(rule, count)) {
            issues.push_back(make_issue(count, "有 " + FormatValue(count, 0) +
                                                   " 个元素使用「长度未知」写法（size 全 1，常见于流式写出的 "
                                                   "Segment / Cluster）。这类文件的时长与索引往往一并缺失，"
                                                   "部分播放器会当成直播流处理而不显示进度条。"));
        }
        return issues;
    }

    if (rule.id == "container.mkv.truncated") {
        if (!mkv.truncated)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "解析到元素头/载荷越过了 Segment 或文件末尾，文件被截断，"
                                               "末尾数据缺失" +
                                                   scope + "。"));
        }
        return issues;
    }

    if (rule.id == "container.mkv.scan_incomplete") {
        if (mkv.scan_complete)
            return issues;
        const double value = 1.0;
        if (Triggered(rule, value)) {
            issues.push_back(make_issue(value, "元素级体检撞到元素预算上限提前结束（已扫 " +
                                                   std::to_string(mkv.clusters) +
                                                   " 个 Cluster），Cues / 时间戳回退等结论只代表已扫描的部分。"));
        }
        return issues;
    }

    return issues;
}

} // namespace detail
} // namespace qc
} // namespace videoeye
