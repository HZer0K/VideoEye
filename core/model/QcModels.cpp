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

    add("container.extension_mismatch", "扩展名与实际容器不符", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "文件扩展名与实际封装格式不一致。FFmpeg 等按内容探测的工具可正常打开，"
        "但按扩展名选择解析器的播放器/剪辑工具/上传平台会打开失败或识别错误。",
        "方案一：按实际容器重命名扩展名（如 xxx.ts 实为 MOV 则改为 xxx.mov）；"
        "方案二：转封装为扩展名对应的容器，流不重编码："
        "ffmpeg -i 输入.ts -c copy 输出.mp4。");

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

    // ---- 码率与 GOP 深度分析（core/analyzer/BitrateGopAnalyzer）----
    add("video.gop.long_count", "存在超长 GOP", IssueCategory::Gop,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "个",
        "存在时长或帧数超过阈值的 GOP，seek 需解码大量帧。",
        "固定 GOP 长度：-g <帧数> -keyint_min <帧数>，或用 -force_key_frames 限制最大间隔。");

    add("video.gop.scene_without_keyframe", "场景切换后缺少关键帧", IssueCategory::Gop,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "处",
        "场景切换点附近没有关键帧，新镜头只能从前一场景预测，画质与 seek 精度受损。",
        "开启场景切换自适应关键帧（x264 默认开启），或用 -force_key_frames 在切换点插入 IDR。");

    add("video.gop.sparse_keyframes", "关键帧过于稀疏", IssueCategory::Gop,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "全片关键帧数量极少（不足 2 个），几乎无法随机访问。",
        "按目标 seek 粒度插入关键帧，例如每 2~5 秒一个 IDR。");

    add("video.bitrate.peak_overshoot", "瞬时码率超过目标峰值", IssueCategory::Bitrate,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "段",
        "存在滑动窗口码率持续超过目标峰值的区间，播放器/CDN 可能出现缓冲。",
        "收紧 VBV（-maxrate / -bufsize）或改用 CBR；确认缓冲区能吸收突发。");

    add("video.frame.oversized", "存在异常大帧", IssueCategory::Video,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "帧",
        "个别帧远大于全片平均帧大小，解码与网络抖动风险升高。",
        "检查是否为场景切换/高动态画面；必要时开启 VBV 限制单帧上限。");

    add("video.frame.oversized_i", "I 帧过大", IssueCategory::Video,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "帧",
        "个别 I 帧远大于平均 I 帧，seek/首屏会产生瞬时带宽尖峰。",
        "降低 I 帧质量权重或调大 GOP；大 IDR 也可能导致首帧解码耗时偏高。");

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

    // ---- 音频 QC（core/analyzer/AudioQcAnalyzer，需解码音频）----
    // 目标响度窗口写成"上限 / 下限"两条规则，便于在「规则与阈值」页直接改容差
    add("audio.loudness.target_high", "响度高于目标", IssueCategory::Audio,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, -22.0, "LUFS",
        "积分响度高于目标上限（默认 -23 ± 1 LUFS），平台会做衰减或触发响度归一。",
        "整体衰减到目标响度：ffmpeg -af loudnorm=I=-23:TP=-1:LRA=11 或 volume=-Xd。");

    add("audio.loudness.target_low", "响度低于目标", IssueCategory::Audio,
        IssueSeverity::Warning, QcRuleOp::MinBelow, -24.0, "LUFS",
        "积分响度低于目标下限（默认 -23 ± 1 LUFS），观众需要手动调大音量。",
        "提升整体增益到目标响度，注意同时守住真峰值上限（如 -1 dBTP）。");

    add("audio.loudness.range", "响度动态范围过大", IssueCategory::Audio,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 20.0, "LU",
        "LRA 偏大，移动端/电视外放场景下轻声段听不清、大声段过响。",
        "适度压缩动态：-af loudnorm=LRA=11 或加轻度限幅/压缩。");

    add("audio.true_peak", "真峰值超过上限", IssueCategory::Audio,
        IssueSeverity::Error, QcRuleOp::MaxExceeded, -1.0, "dBTP",
        "4× 过采样真峰值超过交付上限，转码/播放端可能出现削波失真。",
        "加真峰值限幅：-af loudnorm=TP=-1 或 alimiter/limiter，留 1 dB 余量。");

    add("audio.clipping", "存在削波", IssueCategory::Audio,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "样本",
        "采样值达到满刻度（≥ -0.01 dBFS），波形已被削平，无法后期修复。",
        "回到源头降低增益重新混音；已削波的文件只能靠限幅掩盖。");

    add("audio.silence.longest", "存在过长静音", IssueCategory::Audio,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 10.0, "s",
        "存在过长的静音段，可能是音轨缺失、封装错误或编码中断。",
        "确认该段确实无内容；否则检查音轨是否被静音/丢帧。");

    add("audio.silence.ratio", "静音占比过高", IssueCategory::Audio,
        IssueSeverity::Info, QcRuleOp::MaxExceeded, 50.0, "%",
        "全片静音占比偏高，需确认是否为预期（如纯音乐视频的间隔）。",
        "确认音轨完整；必要时裁剪静音段或改用正确音轨。");

    add("audio.dc_offset", "直流偏移过大", IssueCategory::Audio,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 0.01, "",
        "存在直流分量，会浪费动态余量并让某些编码器/设备出现爆音。",
        "加高通去除直流：-af highpass=f=20 或 dcshift 修正。");

    add("audio.phase_correlation", "声道反相", IssueCategory::Audio,
        IssueSeverity::Warning, QcRuleOp::MinBelow, -0.5, "",
        "声道间相关性为负，单声道下混时会互相抵消（人声被削弱甚至消失）。",
        "检查是否有声道接反/极性反转；用 -af 'pan' 或交换声道修正。");

    add("audio.metadata.layout", "声道布局未标注", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "",
        "多声道但容器未写明声道位置，响度加权与下混顺序只能靠猜测。",
        "封装时写入正确的 channel layout（如 5.1 的 FL FR FC LFE BL BR）。");

    add("audio.metadata.duration_mismatch", "音视频时长不一致", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "音频流时长与容器/视频时长相差明显，可能导致结尾音画不同步。",
        "检查是否丢帧/尾帧被截断，重新封装或补齐音频尾部。");

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
