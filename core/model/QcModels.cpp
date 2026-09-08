#include "core/model/QcRule.h"

#include <algorithm>

namespace videoeye {
namespace model {

const char* ToString(IssueSeverity severity) {
    switch (severity) {
        case IssueSeverity::Info:     return "提示";
        case IssueSeverity::Warning:  return "警告";
        case IssueSeverity::Error:    return "错误";
        case IssueSeverity::Critical: return "致命";
    }
    return "未知";
}

const char* ToString(IssueCategory category) {
    switch (category) {
        case IssueCategory::Container: return "容器";
        case IssueCategory::Video:     return "视频";
        case IssueCategory::Audio:     return "音频";
        case IssueCategory::Timing:    return "时间戳";
        case IssueCategory::Bitrate:   return "码率";
        case IssueCategory::Gop:       return "GOP";
        case IssueCategory::Metadata:  return "元数据";
        case IssueCategory::Other:     return "其他";
    }
    return "未知";
}

const char* ToString(QcRuleOp op) {
    switch (op) {
        case QcRuleOp::MaxExceeded: return "超过上限";
        case QcRuleOp::MinBelow:    return "低于下限";
        case QcRuleOp::NonZero:     return "存在即告警";
    }
    return "未知";
}

std::vector<QcRule> DefaultQcRules() {
    std::vector<QcRule> rules;

    auto add = [&rules](const char* id, const char* name, IssueCategory category,
                        IssueSeverity severity, QcRuleOp op, double threshold,
                        const char* unit, const char* desc, const char* suggestion) {
        QcRule rule;
        rule.id = id;
        rule.name = name;
        rule.category = category;
        rule.severity = severity;
        rule.op = op;
        rule.threshold = threshold;
        rule.unit = unit ? unit : "";
        rule.enabled = true;
        rule.description = desc ? desc : "";
        rule.suggestion = suggestion ? suggestion : "";
        rules.push_back(std::move(rule));
    };

    // ---- 容器 ----
    add("container.duration_invalid", "时长异常", IssueCategory::Container,
        IssueSeverity::Critical, QcRuleOp::MinBelow, 0.01, "s",
        "容器时长缺失或接近 0，多数播放器无法正确 seek。",
        "重新封装时用 -movflags +faststart 或写回正确的 duration/mvhd。");

    add("container.unseekable", "不可随机访问", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "",
        "底层 IO 不支持 seek（管道/网络流），只能线性播放。",
        "确认输入为本地文件；网络场景请使用支持 Range 请求的协议。");

    add("container.moov_after_mdat", "moov 位于 mdat 之后", IssueCategory::Container,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "",
        "MP4 索引在媒体数据之后（未 faststart），点播需下载完整个文件才能起播。",
        "重封装时加 -movflags +faststart，或用 qt-faststart 前置 moov。");

    add("container.missing_video", "缺少视频流", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "文件中没有视频流。",
        "确认转码参数是否误用了 -vn。");

    add("container.missing_audio", "缺少音频流", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "文件中没有音频流。",
        "确认转码参数是否误用了 -an。");

    // ---- 码率 ----
    add("video.bitrate.peak_ratio", "码率波动过大", IssueCategory::Bitrate,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 3.0, "倍",
        "峰值/均值码率比过高，直播/CDN 场景容易触发缓冲。",
        "改用 CBR 或收紧 VBV（maxrate/bufsize）。");

    add("video.bitrate.low_bpp", "编码码率偏低", IssueCategory::Bitrate,
        IssueSeverity::Info, QcRuleOp::MinBelow, 0.05, "bpp",
        "每像素每帧比特数偏低，高动态画面可能出现块效应。",
        "提高目标码率或降低分辨率/帧率。");

    // ---- GOP ----
    add("video.gop.max_seconds", "关键帧间隔过长", IssueCategory::Gop,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 10.0, "s",
        "相邻关键帧间隔过长，seek 慢且拖动卡顿。",
        "设置合理的 GOP：如 -g 50 -keyint_min 50 -sc_threshold 0。");

    add("video.gop.irregular", "关键帧间隔不均匀", IssueCategory::Gop,
        IssueSeverity::Info, QcRuleOp::MaxExceeded, 0.6, "",
        "关键帧间隔标准差/均值偏高，场景切换自适应插入导致。",
        "若不追求自适应关键帧，可关闭 sc_threshold 使用固定 GOP。");

    // ---- 视频 ----
    add("video.fps.unstable", "帧率不稳定", IssueCategory::Video,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 2.5, "fps",
        "逐秒帧率标准差偏大，播放可能出现抖动/丢帧。",
        "检查源是否为可变帧率(VFR)，必要时用 -vsync cfr / fps filter 规整。");

    add("video.resolution.odd", "分辨率非偶数", IssueCategory::Video,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "宽或高为奇数，4:2:0 采样下多数编码器要求偶数。",
        "缩放至偶数分辨率：-vf scale=trunc(iw/2)*2:trunc(ih/2)*2。");

    // ---- 时间戳（PTS 非单调 / 时间戳跳变等同步类问题统一由 TimelineAnalyzer 产出，
    //        以 category=Timing 的 DiagnosticIssue 并入诊断报告，此处不再重复规则） ----
    add("timing.dts_missing", "DTS 大量缺失", IssueCategory::Timing,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 5.0, "%",
        "缺失 DTS 的包占比过高，含 B 帧时排序依赖播放器猜测。",
        "确保封装器写出 DTS（多数 muxer 默认会写，检查是否走了裸流）。");

    // ---- 音频 ----
    add("audio.sample_rate_low", "音频采样率偏低", IssueCategory::Audio,
        IssueSeverity::Info, QcRuleOp::MinBelow, 32000.0, "Hz",
        "音频采样率低于 32 kHz，音质受限。",
        "转码时指定 -ar 44100 或 -ar 48000。");

    add("audio.channel_missing", "声道信息缺失", IssueCategory::Audio,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "",
        "音频流声道数未标注（0），下游混音可能异常。",
        "检查容器是否写入了正确的 channel layout。");

    return rules;
}

QcRule* FindQcRule(std::vector<QcRule>& rules, const std::string& id) {
    auto it = std::find_if(rules.begin(), rules.end(),
                           [&id](const QcRule& r) { return r.id == id; });
    return (it == rules.end()) ? nullptr : &(*it);
}

const QcRule* FindQcRule(const std::vector<QcRule>& rules, const std::string& id) {
    auto it = std::find_if(rules.begin(), rules.end(),
                           [&id](const QcRule& r) { return r.id == id; });
    return (it == rules.end()) ? nullptr : &(*it);
}

} // namespace model
} // namespace videoeye
