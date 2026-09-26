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
        case IssueCategory::ColorHdr:  return "色彩/HDR";
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

    // ---- MP4/fMP4 容器一致性（core/analyzer/Mp4SampleTableAnalyzer，走自研 IsobmffParser）----
    // 这些规则只决定"是否上报 / 以什么级别上报"，问题本身（含具体样本号、偏移、分片号）
    // 由分析器给出；级别取「规则级别」与「分析器级别」中更严重的一侧，
    // 因此把规则调到 Error 可以抬高，调到 Info 也不会把 Error 级发现降没。
    add("container.mp4.sample_count_mismatch", "样本表样本数不一致", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "",
        "stts / ctts / stss / stsz / stco 等表的样本数不一致，或缺少 chunk offset 表。",
        "重新封装（-c copy）让 muxer 重写全部 sample table。");

    add("container.mp4.chunk_offset_out_of_range", "样本偏移越过文件末尾", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "个",
        "chunk offset + sample size 超出文件大小，通常是文件被截断或偏移表写坏。",
        "重新下载 / 重新封装；-c copy 重封装会按实际数据重建偏移表。");

    add("container.mp4.dts_not_monotonic", "解码时间回退", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "处",
        "DTS 出现回退，含 B 帧时解码顺序被打乱，表现为花屏或 seek 卡死。",
        "重新封装让 muxer 重算 stts；检查是否误用了 VFR 输入。");

    add("container.mp4.negative_cts", "合成时间为负", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "个",
        "ctts 负偏移导致 PTS < 0，部分播放器会出现起播黑帧、首帧被丢或音画不同步。",
        "用 elst 平移代替负 ctts：重新封装时加 -avoid_negative_ts make_zero。");

    add("container.mp4.sample_offset_gap", "chunk 内样本偏移不连续", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "处",
        "stsz 与 stco/stsc 不自洽，同一 chunk 内样本偏移不等于上一帧结束位置。",
        "重新封装（-c copy）重建 sample table。");

    add("container.mp4.first_sample_not_sync", "首个样本不是关键帧", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "起播必须先解码到下一个 IDR，首屏变慢甚至短时花屏。",
        "重新编码时在起点插入 IDR，或把剪辑点挪到关键帧上。");

    add("container.mp4.elst_first_frame_shift", "elst 造成首帧偏移", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 33.0, "ms",
        "edit list 把媒体起点推后，开头一段内容不会呈现（首帧被隐藏）。",
        "确认是否为刻意的音视频对齐；否则删除 elst 后重新封装。");

    add("container.mp4.av_start_mismatch", "音视频起点不一致", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 40.0, "ms",
        "视频与音频首个样本的呈现时间相差过大，表现为开头音画不同步。",
        "统一两侧 elst（-c copy -avoid_negative_ts make_zero），或用 -itsoffset 对齐。");

    add("container.mp4.fragment_sequence_gap", "分片序号不连续", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "处",
        "moof 的 sequence_number 跳号、重复或回退，播放器无法判断分片顺序。",
        "检查分片拼接脚本，保证序号单调递增且唯一。");

    add("container.mp4.fragment_time_gap", "分片解码时间不连续", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "处",
        "tfdt 与上一分片的结束时间对不上（时间空洞或回退），分段处会停顿、时长统计偏大。",
        "重新生成分片；拼接多段时要做时间轴平移。");

    add("container.mp4.fragment_data_offset", "分片样本数据位置缺失", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "个",
        "tfhd.base_data_offset / default-base-is-moof / trun.data_offset 三者全无，"
        "播放器无法算出样本数据的文件偏移。",
        "重新封装；CMAF 要求前两者至少有一个。");

    // ---- HLS 流媒体包（core/analyzer/HlsManifestAnalyzer）----
    // 与 MP4 那一组同样的套路：问题由分析器产出，规则决定要不要报、以什么级别报。
    add("container.hls.missing_target_duration", "缺少目标分片时长", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "媒体播放列表缺少 EXT-X-TARGETDURATION。",
        "补上 #EXT-X-TARGETDURATION:<n>，其值不小于任一 EXTINF 的整数值。");

    add("container.hls.segment_duration_over_target", "分片时长超过目标时长", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "个",
        "存在时长超过 EXT-X-TARGETDURATION 的分片，播放器按 target 预取不足会卡顿。",
        "调大 EXT-X-TARGETDURATION，或重新切片让分片时长稳定。");

    add("container.hls.segment_duration_jitter", "分片时长抖动过大", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::MaxExceeded, 0.25, "",
        "分片时长忽长忽短，ABR 切换点与 GOP 边界对不齐。",
        "按固定 GOP 切片，让码率 ladder 使用相同的切片点。");

    add("container.hls.discontinuity_unpaired", "discontinuity 未配对", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "处",
        "EXT-X-DISCONTINUITY 没有与之配对的序号、后面没有分片，或音视频两侧位置不一致。",
        "补 EXT-X-DISCONTINUITY-SEQUENCE，并让同一内容的所有 variant / 音轨在同一时间点打标记。");

    add("container.hls.missing_init_section", "缺少初始化段", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "fMP4/CMAF 分片没有 EXT-X-MAP 声明初始化段。",
        "用 #EXT-X-MAP:URI=\"init.mp4\" 给出含 moov 的初始化段。");

    add("container.hls.encryption_key", "分片已加密", IssueCategory::Container,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "",
        "清单声明了 EXT-X-KEY，播放依赖密钥服务。",
        "确认密钥服务在目标网络可达；SAMPLE-AES 与 fMP4 组合需确认播放器支持。");

    add("container.hls.partial_segment", "启用 LL-HLS 部分分片", IssueCategory::Container,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "个",
        "清单使用了 EXT-X-PART（Low-Latency HLS）。",
        "确认服务端支持阻塞式播放列表重载，且 CDN 不会缓存部分分片。");

    add("container.hls.variant_bandwidth_mismatch", "实测码率高于声明带宽", IssueCategory::Bitrate,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "条",
        "实测峰值段码率明显高于 EXT-X-STREAM-INF 的 BANDWIDTH。",
        "按实测峰值更新 BANDWIDTH，或加 VBV/maxrate 让分片码率更平。");

    add("container.hls.variant_resolution_mismatch", "码率阶梯分辨率异常", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "ladder 里有流未声明 RESOLUTION，或分辨率与码率顺序矛盾。",
        "给每条 EXT-X-STREAM-INF 补 RESOLUTION，并保证分辨率随码率单调不减。");

    add("container.hls.variant_codec_mismatch", "码率阶梯编码不一致", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "种",
        "同一条 ladder 混用了不同的视频编码，ABR 切换时解码器要重建。",
        "同一 ladder 使用同一种编码（含 profile/level 兼容）。");

    add("container.hls.variant_keyframe_misalign", "多码率关键帧不对齐", IssueCategory::Gop,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "条",
        "各码率的分片起点（关键帧）时间轴不一致，ABR 无法在切换点无缝切换。",
        "让所有码率层使用相同的 GOP 长度与切片起点。");

    add("container.hls.av_segment_count_mismatch", "音视频分片数量/时长对不上", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "视频轨与音轨的分片数量不同，且总时长差超过一个分片。",
        "让音视频使用相同的分片时长并对齐起点。");

    // ---- DASH 流媒体包（core/analyzer/DashManifestAnalyzer）----
    add("container.dash.segment_timeline_gap", "SegmentTimeline 存在时间缺口", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "处",
        "SegmentTimeline 的相邻 <S> 之间存在时间空洞，播放器会缓冲失败或跳播。",
        "补齐缺失的分片，或用 <S t=...> 显式声明每个区段的起点。");

    add("container.dash.segment_timeline_overlap", "SegmentTimeline 存在时间重叠", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "处",
        "SegmentTimeline 的相邻 <S> 时间倒退，会导致重复播放与音画错位。",
        "修正 <S> 的 t / d / r，让各区段首尾相接。");

    add("container.dash.missing_segment_info", "缺少分片定位信息", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "",
        "Representation 既没有 SegmentTemplate，也没有 SegmentList / SegmentBase。",
        "补 SegmentTemplate（推荐），或至少给 SegmentBase + indexRange。");

    add("container.dash.representation_bandwidth_mismatch", "实测码率高于声明带宽", IssueCategory::Bitrate,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "条",
        "实测峰值段码率明显高于 Representation@bandwidth。",
        "按实测峰值更新 bandwidth，或加 VBV/maxrate 让分片码率更平。");

    add("container.dash.representation_resolution_mismatch", "码率阶梯分辨率异常", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "ladder 里有 Representation 未声明 width/height，或分辨率与码率顺序矛盾。",
        "给每个 Representation 补 width / height，并保证分辨率随码率单调不减。");

    add("container.dash.representation_codec_mismatch", "码率阶梯编码不一致", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "种",
        "同一条 ladder 混用了不同的视频编码，ABR 切换时解码器要重建。",
        "同一 ladder 使用同一种编码（含 profile/level 兼容）。");

    add("container.dash.variant_keyframe_misalign", "多码率关键帧不对齐", IssueCategory::Gop,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "条",
        "各 Representation 的分片起点（关键帧）时间轴不一致。",
        "让所有码率层使用相同的 GOP 长度与切片起点。");

    add("container.dash.av_segment_count_mismatch", "音视频分片数量/时长对不上", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "同一 Period 内视频轨与音轨的总时长差超过一个分片。",
        "让音视频使用相同的分片时长并对齐起点。");

    // ---- HLS / DASH 通用（core/analyzer/SegmentQcAnalyzer）----
    add("container.streaming.segment_missing_file", "分片文件不存在", IssueCategory::Container,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "个",
        "清单声明的本地分片在磁盘上找不到。",
        "补齐分片文件，或修正清单里的相对路径。");

    add("container.streaming.segment_container_invalid", "分片容器解析失败", IssueCategory::Container,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "个",
        "分片文件存在，但用已有的容器分析器解析失败。",
        "单独打开该分片看具体报错；常见原因是分片被截断或缺少初始化段。");

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

    // ---- 色彩与 HDR（core/analyzer/ColorHdrAnalyzer）----
    add("video.color.hdr_missing_mastering", "PQ 缺少母版显示信息", IssueCategory::ColorHdr,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "传递函数为 PQ(SMPTE ST 2084)，但没有 SMPTE ST 2086 母版显示信息"
        "（mastering display metadata）。播放器只能按默认色彩体量做色调映射，"
        "在不同显示器上亮度与饱和度会明显不一致。",
        "补充母版显示信息后重封装/重编码：x265 用 -x265-params "
        "master-display=G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)；x264/ffmpeg 可用 "
        "libx265 的 master-display 或 mp4box --hdr。" "MaxCLL/MaxFALL 一并写入更稳妥。");

    add("video.color.hdr_missing_light_level", "PQ 缺少 MaxCLL/MaxFALL", IssueCategory::ColorHdr,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "传递函数为 PQ，但缺少 MaxCLL / MaxFALL（CTA-861.3 内容光level）。"
        "缺少该值时接收端无法预判内容亮度，容易整体过曝或压暗。",
        "写入内容光level：x265 用 -x265-params max-cll=1000,400；"
        "MP4 封装对应 clli 盒，Matroska 对应 MaxCLL/MaxFALL 元素。");

    add("video.color.hdr_low_bitdepth", "HDR 位深不足", IssueCategory::ColorHdr,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "bit",
        "PQ/HLG 内容使用了不足 10 bit 的像素格式。PQ/HLG 需要 10 bit 以上位深，"
        "8 bit 承载 HDR 会出现明显色带（尤其暗部渐变）。",
        "重新编码为 10 bit：-pix_fmt yuv420p10le（HDR10 常规选择）；"
        "若确实只有 8 bit 源，应改用 SDR 分发（transfer=BT.709）。");

    add("video.color.wide_gamut_sdr_transfer", "广色域搭配 SDR 传递函数", IssueCategory::ColorHdr,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "色彩原色为 BT.2020 / P3 这类广色域，但传递函数仍是传统 SDR gamma。"
        "广色域通常需要搭配 PQ/HLG 才能表达高亮度范围，否则要么被当作 SDR BT.2020 "
        "做窄范围映射（颜色发灰），要么说明整套色彩标注是误标的。",
        "确认内容本色：是 HDR 就把 transfer 改成 PQ/HLG 并补齐静态元数据；"
        "是 SDR 就把 primaries/matrix 改回 BT.709。"
        "（仅在有意分发 SDR BT.2020 时可忽略本条。）");

    add("video.color.matrix_mismatch", "矩阵系数与原色不匹配", IssueCategory::ColorHdr,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "矩阵系数与色彩原色不属于同一代标准（如 BT.2020 原色配 BT.709 矩阵）。"
        "YUV→RGB 会按错误矩阵还原，画面整体偏色（通常表现为肤色发青/发紫）。",
        "让矩阵跟原色对齐：BT.709 原色配 BT.709 矩阵，BT.2020 原色配 BT.2020 NCL 矩阵。"
        "ffmpeg 可用 -colorspace bt2020nc -color_primaries bt2020 -color_trc smpte2084。");

    add("video.color.range_conflict", "量化范围标注冲突", IssueCategory::ColorHdr,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "像素格式隐含的量化范围与容器标注不一致（如 yuvj420p 隐含 full range 却被标成 "
        "Limited，或 RGB 数据被标成 Limited）。灰阶会被整体抬高/压低，"
        "表现为画面发灰或黑位丢 detail。",
        "统一两者：either 用 yuv420p 等不带隐含范围的像素格式，"
        "either 把 AVColorRange 改成实际值（-color_range pc / tv）。");

    add("video.color.unspecified", "色彩元数据缺失", IssueCategory::ColorHdr,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "项",
        "primaries / transfer / matrix / range 中有未标注项。缺失时播放器只能猜"
        "（通常按分辨率猜 BT.601 或 BT.709），广色域与 HDR 内容几乎必然颜色错误。",
        "填全 VUI/容器色彩标注：ffmpeg 加 -color_primaries bt709 -color_trc bt709 "
        "-colorspace bt709 -color_range tv（-c copy 即可，无需重编码）。");

    add("video.color.dv_no_compatibility", "Dolby Vision 无兼容层", IssueCategory::ColorHdr,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "",
        "Dolby Vision 流的 bl_signal_compatibility_id 为 0（如 Profile 5），"
        "没有 HDR10/SDR 兼容层。非 Dolby Vision 设备回放时颜色与亮度会明显错误。",
        "外发需求覆盖多终端时改用带兼容层的 Profile 8.1（HDR10 基线）或 Profile 8.4（HLG 基线）；"
        "仅 DV 端到端场景可保留 Profile 5。");

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

    // ---- 字幕 / 时码 / 章节 / 辅助数据（功能 9，core/analyzer/SubtitleAnalyzer 等）----
    // 这几类规则一律是"存在即报"（NonZero）：具体阈值（最短/最长停留、阅读速度）
    // 在 SubtitleOptions 里，规则只负责决定要不要进报告、按什么级别进。
    add("subtitle.cue_empty", "空字幕 cue", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "条",
        "存在没有任何可见文本的字幕 cue，播放时表现为「闪一条空白」。",
        "删掉空 cue，或补上文本；批量检查可用字幕编辑器的「移除空行」功能。");

    add("subtitle.cue_overlap", "字幕 cue 重叠", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "处",
        "相邻字幕的时间区间重叠，播放时会两行压在一起或互相顶掉。",
        "把前一条的结束时间改到后一条开始时间之前（留 1-2 帧间隔）。");

    add("subtitle.cue_too_short", "字幕停留过短", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.5, "s",
        "字幕停留时间短于下限，观众来不及读完。",
        "延长到至少 0.5-1 秒；一句话字幕建议 1.5 秒以上。");

    add("subtitle.cue_too_long", "字幕停留过长", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 8.0, "s",
        "字幕停留时间长于上限，通常是忘记写结束时间或时间轴写错。",
        "补上正确的结束时间；长句建议拆成两条。");

    add("subtitle.cue_order", "字幕时间倒序", IssueCategory::Metadata,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "处",
        "字幕 cue 的开始时间早于上一条，播放器可能不显示或顺序错乱。",
        "按时间顺序重排 cue，检查是否串行号与时间轴对错位。");

    add("subtitle.cue_invalid_duration", "字幕时长非法", IssueCategory::Metadata,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "条",
        "cue 的结束时间不晚于开始时间，规范上非法。",
        "修正结束时间；SRT 里常见于小时位写错（00 写成 10）。");

    add("subtitle.cue_too_fast", "字幕阅读速度过快", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 21.0, "字符/秒",
        "按字符数/停留时间算出的阅读速度超过上限。",
        "拆分长句或延长停留；中文建议 12-16 字符/秒，英文 16-21。");

    add("subtitle.cue_out_of_range", "字幕超出媒体时长", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "条",
        "cue 的开始时间晚于媒体总时长，永远不会被显示。",
        "核对字幕时间轴是否按错的时间基准（如 25fps 字幕配到 30fps 片子）。");

    add("subtitle.missing_language", "字幕缺少语言 tag", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "条",
        "字幕流没有 language tag，播放器无法按界面语言自动选轨。",
        "封装时写语言：-metadata:s:s:0 language=chi（-c copy 即可）。");

    add("subtitle.missing_handler", "字幕缺少 handler name", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "条",
        "MP4/MOV 的字幕轨没有 hdlr handler name，部分工具识别不出这是字幕轨。",
        "用支持写 hdlr 的 muxer 重新封装（FFmpeg 的 mov/mp4 muxer 默认会写）。");

    add("timecode.missing", "缺少时码", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "",
        "既没有 tmcd 时码轨，也没有 metadata 里的 timecode tag，交付规范可能不满足。",
        "母版写入时码：ffmpeg -i in.mov -timecode 00:59:58:00 -c copy out.mov。");

    add("timecode.drop_frame_mismatch", "时码 drop-frame 与帧率不符", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "",
        "29.97/59.94 素材配 non-drop 时码（或反过来），长片会累积约 3.6 秒/小时的偏差。",
        "统一成 drop-frame 时码（00:00:00;00 写法），或把素材规整到整数帧率。");

    add("timecode.invalid_frame", "时码帧号非法", IssueCategory::Metadata,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "",
        "首帧时码的帧号越界，或落在 drop-frame 跳过的帧上。",
        "按 SMPTE 12M 重算起始时码，检查是否是手工填写时写错。");

    add("chapter.overlap", "章节重叠", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "处",
        "相邻章节的时间区间重叠，跳转时落在哪一段不确定。",
        "让章节首尾相接（后一章起点 = 前一章终点）。");

    add("chapter.out_of_range", "章节越界", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "处",
        "章节终点超出媒体时长，播放器跳转到该章节会失败。",
        "把最后一个章节的终点改到媒体时长以内。");

    add("chapter.non_monotonic", "章节倒序", IssueCategory::Metadata,
        IssueSeverity::Error, QcRuleOp::NonZero, 0.0, "处",
        "章节起点早于上一章节起点，章节列表与播放顺序不一致。",
        "按时间顺序重排章节列表。");

    add("chapter.zero_duration", "章节零时长", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "处",
        "存在时长为 0 的章节，通常是导出章节时首尾写成了同一个时间。",
        "补上章节终点时间。");

    add("chapter.missing_title", "章节无标题", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "处",
        "章节没有标题，播放器章节菜单会显示成空白。",
        "写章节 metadata：ffmpeg -i in.mp4 -i chapters.txt -map_metadata 1 -c copy out.mp4。");

    add("scte35.parse_error", "SCTE-35 解析失败", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "个",
        "有 SCTE-35 载荷解析不出来（table_id / section_length / 命令体异常）。",
        "核对 cue 生成的字节序与描述符长度；抓包确认 TS 里的 SCTE-35 PID 没被截断。");

    add("scte35.crc_invalid", "SCTE-35 CRC 校验失败", IssueCategory::Metadata,
        IssueSeverity::Warning, QcRuleOp::NonZero, 0.0, "个",
        "SCTE-35 section 的 CRC_32 不通过，cue 可能在传输或拼接环节被改坏。",
        "让上游重新计算 CRC_32（MPEG-2 多项式），不要手工改 cue 字节。");

    add("scte35.duration_missing", "SCTE-35 缺少 duration", IssueCategory::Metadata,
        IssueSeverity::Info, QcRuleOp::NonZero, 0.0, "条",
        "splice_insert cue 没给 break_duration，下游不知道广告要插多长。",
        "生成 cue 时带上 break_duration 或 segmentation_duration。");

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
