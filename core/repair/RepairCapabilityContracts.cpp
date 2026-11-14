#include "core/repair/RepairCapabilityContracts.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace videoeye {
namespace repair {
namespace {

// 状态词只允许这三个取值。新增取值必须同时扩 CapabilityStatusLabel 与守护用例，
// 否则报告会渲染成「未知状态」（这是刻意的：宁可显得难看，也不能让一个陌生词
// 被默认读成「支持」或「不支持」）。
constexpr const char* kPartiallySupported = "partially_supported";
constexpr const char* kUnsupported = "unsupported";
constexpr const char* kDeferred = "deferred";

model::RepairCapabilityBoundary MakeBase(const std::string& code, const std::string& status) {
    model::RepairCapabilityBoundary b;
    b.code = code;
    b.status = status;
    return b;
}

model::RepairCapabilityBoundary MakeMoovRebuild() {
    auto b = MakeBase("moov_rebuild", kPartiallySupported);
    b.covered_scope = {
        "无参考受限重建：ftyp + 恰好一个 mdat + 单轨，且载荷为纯 AVCC H.264 或纯 ADTS AAC"
        "（RepairPlainMp4Rebuilder）",
        "时长只在 SPS VUI 提供时基证据时才推定；证据不足时直接拒绝，不猜一个看起来合理的值",
    };
    b.not_covered_scope = {
        "音视频交织的 mdat、多个 mdat、多轨并行",
        "HEVC 及其他编码类型的样本表重建（本版本只覆盖 AVCC H.264 / ADTS AAC）",
        "无时基证据时的时长推定（本版本选择拒绝而不是估算）",
    };
    b.preconditions = {
        "输入必须是 ISOBMFF（MP4/MOV/M4V/3GP），且 mdat 载荷未被截断、可逐样本定界",
        "能取得每条轨的编码类型、NAL 长度前缀字节数、媒体时基（mdhd timescale）与关键帧序列——"
        "来自同源初始化段/完整样片，或可被验证的码流内参数",
        "音频轨若为裸 AAC 等无自同步头的载荷，必须另有 AudioSpecificConfig",
        "允许产物是重新推导的索引，而不是原始 moov 字节",
    };
    b.unrecoverable_scope = {
        "moov 独有的元数据：编辑列表 elst、章节、字幕/文本轨、自定义 box、DRM/加密描述、"
        "轨道语言与旋转矩阵",
        "原始 stts/ctts/stsc/stsz/stco 的精确取值，以及 chunk 在文件里的物理布局",
        "B 帧显示顺序的原始偏移、音画同步的原始起点（无 ctts/elst 时无法还原）",
        "mdat 本身被截断、覆盖或加密的部分：没有样本边界信息就切不出访问单元",
    };
    b.timeline_impact = "必然改写：原始 stts/ctts/elst 不可得，只能重建为恒定帧率或按码流内时间戳推算；"
                        "起始偏移、总时长、B 帧显示顺序与音画同步起点都要重算。"
                        "fidelity.timeline_status 只能是 modified / unknown，不能写 unchanged。";
    b.quality_risks = {
        "不重编码时像素无损，但帧率/时长重建错误会造成快放、慢放或音画不同步",
        "关键帧判定错误会让 seek 落点花屏或跳转失败",
        "若被迫重编码以补齐缺失帧，会引入代际压缩损失",
        "没有原始完整视频作参考，无法给出恢复比例，也不能断言无失真",
    };
    b.evidence = "普通 MP4 的样本表、时基与解码配置全部位于 moov；moov 丢失后文件里只剩 mdat 裸载荷。"
                 "上述已覆盖范围由 RepairPlainMp4Rebuilder 实现并在真实语料上验证；"
                 "未覆盖范围与不可恢复范围各自独立——前者是本版本没做，后者是任何实现都拿不回来。";
    return b;
}

model::RepairCapabilityBoundary MakeParameterSetRecovery() {
    auto b = MakeBase("parameter_set_recovery", kPartiallySupported);
    b.covered_scope = {
        "借同源初始化段：从中取 mdhd timescale 与解码配置，并按 sidx 估算时长与逐轨 trun/tfdt 声明"
        "时长交叉核对（R03 时基核验）",
        "由 in-band SPS/PPS(VPS) 自建视频初始化段（RepairInitSegmentBuilder），不依赖任何参考文件",
    };
    b.not_covered_scope = {
        "音频轨初始化段自建：裸 AAC 既无 ADTS 同步头、也没有同源 AudioSpecificConfig 时不重建",
        "参数集残缺时的补全（例如只有 SPS 没有 PPS）：不做枚举收敛，直接判为不可恢复",
    };
    b.preconditions = {
        "文件内存在完整的 in-band SPS/PPS（HEVC 还需 VPS），或能提供同源的 avcC/hvcC/esds"
        "（含 AudioSpecificConfig）",
        "能确定 track→codec 映射、NAL 长度前缀字节数、mdhd timescale 与 track id",
        "借来的时基有源侧独立证据（sidx 估算时长与逐轨 trun/tfdt 声明时长交叉核对），"
        "且参数集内容与 slice 语法一致",
    };
    b.unrecoverable_scope = {
        "参数集完全不在文件里时物理上不可恢复：真实样本 17959 个 NAL 中 VPS/SPS/PPS 计数全为 0；"
        "缺少 SPS/PPS 里的 num_extra_slice_header_bits / output_flag_present_flag / SAO 等标志，"
        "连 slice header 的位域切分都无法确定，是循环依赖，不能靠枚举参数收敛",
        "裸 AAC 载荷无 ADTS 同步头时，AOT/采样率/声道数没有来源；编码器版本 tag 不是 AudioSpecificConfig",
        "被覆盖、加密或截断的参数集字节不可逆；mdat 压缩数据里偶然出现的 esds/avc1 字节不是解码配置",
    };
    b.timeline_impact = "参数集本身不改时间轴，但必须从同源初始化段借 mdhd timescale；借用未核验的时基会悄悄改写"
                        "播放时长（实测 mdhd timescale ×2 后帧数不变、播放时长减半）。"
                        "在 reference_compatibility=verified 且时长比对一致之前，timeline_status 只能是 unknown。";
    b.quality_risks = {
        "参数不匹配时 demuxer 会静默丢 AU：实测 126 MiB 源只写出 14.6 MiB，载荷哈希仍可能显示一致",
        "in-band 参数集与 slice 不兼容会出现「能打开但整段花屏」，解码 API 未必报错",
        "HEVC 的 VPS/SPS/PPS 缺一不可；只有 SPS/PPS 不足以建立解码配置",
        "音频参数猜错会产生噪声或全静音，且无法用视频轨证据反证",
    };
    b.evidence =
        "RepairFragmentForensics 逐 NAL 实测 + RepairInitSegmentReader 逐轨 trun/tfdt 时长交叉核对；"
        "TamperedTimescaleActuallyHalvesPlaybackDuration / TamperedNonFirstTrackTimescaleDoesNotPublishCleanResult "
        "在真实媒体上复现过时基篡改。两条已覆盖路径（借同源初始化段、in-band 自建）都在真实语料上跑通过；"
        "未覆盖范围是本版本没做，不可恢复范围是物理上拿不回来，两者不能合并成一句「不支持」。";
    return b;
}

model::RepairCapabilityBoundary MakeDecodableGopTranscode() {
    auto b = MakeBase("decodable_gop_transcode", kPartiallySupported);
    b.covered_scope = {
        "整轨容错重编码（S08）：从第一个可解码关键帧起、到 salvage_end_seconds 为止解码视频帧，"
        "按 H.264 重编码；音频在目标容器接受原编码时按码流复制，拒绝时整轨重编码成实测能收的编码",
        "重编码前后逐项实测源侧保真度（像素格式 / 位深 / 色彩标记 / 旋转矩阵 / HDR 静态元数据 / "
        "动态分辨率），三份报告同源呈现",
        "抽帧 PSNR 画质评估（有参考样片时才给数值）",
    };
    b.not_covered_scope = {
        "GOP 级局部重编码：只重编损坏 GOP、其余段原样拷贝（当前一律整轨重编码）",
        "拼接边界的兼容性验证：重编段与原始段的参数集、参考帧链能否衔接",
        "静音填补 / 重复帧填补：音轨能复制 / 重编码 / 剔除；静音填补要在时间轴空洞里"
        "生成源里不存在的采样（切片 W 已实现，但只对**重编码**轨有效，码流复制轨没有编码器）；"
        "重复帧填补仍未实现",
        "视频黑帧填补要显式启用（RepairRequest::fill_video_gaps_with_black_frames），且只覆盖"
        "「相邻解码帧之间整帧缺失」这一种形态，不覆盖轨道起点之前 / 整段轨道缺失",
    };
    b.preconditions = {
        "能定位 IDR / 关键帧边界，且从该点起解码器可连续解出至少一组完整 GOP",
        "解码参数集可用（见 parameter_set_recovery 契约），解码器能正常打开该轨",
        "用户接受重编码产物（RecoverContent），并接受产物不再是原码流",
        "有明确的抢救边界（salvage_end_seconds），且边界之外的内容被明确放弃",
    };
    b.unrecoverable_scope = {
        "关键帧本身损坏、参数集缺失的段：没有起点可以重建参考帧链",
        "丢失参考帧之后的 P/B 帧只能丢弃或用前一帧冻结，无法还原原始画面",
        "原始码流字节、原始时间戳精度、moov 元数据与原始画质都不可恢复",
        "音频轨损坏必须独立判定，不能由「视频 GOP 可解码」推断音频完整",
    };
    b.timeline_impact = "必然改写：丢弃不可解码段会留下时间轴缺口；S08 按源声明帧率推定为恒定帧率，"
                        "起点、时长与音画同步都要重算。content_completeness 只能是 partial，"
                        "timeline_status=modified，salvage_end_seconds 之后的内容明确丢失。"
                        "默认不插入任何合成内容（synthetic_frames_inserted=false）；显式启用「空档补黑帧」/"
                        "「空档补静音」时只补视频轨的整帧空洞与重编码音频轨的整块采样空洞（码流复制轨只探测、"
                        "不补），逐处记账，填补时长不计入恢复率分子，报告同时标出产物含合成内容。";
    b.quality_risks = {
        "重编码代际损失（二次压缩），低码率下块效应与细节丢失明显",
        "丢帧导致卡顿、合成帧导致画面冻结或抖动；GOP 边界判断错误会让段首花屏",
        "音画不同步：视频时间轴重算后音频若按原时间戳复制，偏差会越积越大",
        "无参考样片时 perceptual_fidelity 只能是 unknown_without_reference，不能声称画质无损",
        "源侧位深 / 色彩标记 / 旋转矩阵 / HDR 静态元数据可能在统一 8-bit yuv420p 重编码里丢失；"
        "切片 J 起逐项实测并写入 fidelity.transcode，未实测时不得声称属性均已保留",
        "实际编码器可能与首选的 libx264 不同（LGPL 构建不含 libx264）；画质与码率特性要按"
        "fidelity.transcode.encoder_used 记录的实际编码器评估，不能套用首选编码器的表现",
    };
    b.evidence = "S08_TolerantTranscode 已实现并可运行：RecoverContent 模式下解码可读视频帧、"
                 "按 H.264 重编码、音频按容器实测结果复制或重编码，并做抽帧 PSNR 画质评估"
                 "（RepairTolerantTranscode / RepairExecutor）。阶段 6 切片 J 起，重编码前逐项实测源侧"
                 "像素格式 / 位深 / 色彩标记 / 旋转 / HDR / 动态分辨率，并同源写进三种报告格式。"
                 "重编码首选 libx264，但本项目锁定的 FFmpeg 是 LGPL 变体"
                 "（`cmake/ffmpeg-version.json`：--disable-libx264 --disable-libx265），首选必然不可用、"
                 "自动回退到构建内可用的 H.264 编码器；实际编码器由运行时探测写入 "
                 "fidelity.transcode.encoder_used，报告不得把首选名当成既成事实。"
                 "未覆盖范围里的 GOP 级局部重编码尚无实现，不能读成本版本已能精确定界重编码范围。";
    return b;
}

const std::vector<std::string>& TriggersFor(const std::string& code) {
    static const std::vector<std::string> moov = {"container.moov_missing"};
    static const std::vector<std::string> params = {"codec.parameter_sets_absent", "codec.decoder_config_absent",
                                                    "codec.in_band_parameter_sets_present",
                                                    "container.init_segment_missing"};
    static const std::vector<std::string> gop = {"decode.sample_failed", "track.sample_out_of_bounds",
                                                 "container.fragment_incomplete", "container.read_error_after_data"};
    if (code == "moov_rebuild")
        return moov;
    if (code == "parameter_set_recovery")
        return params;
    return gop;
}

} // namespace

const std::vector<std::string>& CapabilityStatusVocabulary() {
    static const std::vector<std::string> words = {kPartiallySupported, kUnsupported, kDeferred};
    return words;
}

bool IsKnownCapabilityStatus(const std::string& status) {
    const auto& words = CapabilityStatusVocabulary();
    return std::any_of(words.begin(), words.end(), [&status](const std::string& word) { return word == status; });
}

std::string CapabilityStatusLabel(const std::string& status) {
    if (status == kPartiallySupported)
        return "部分支持";
    if (status == kUnsupported)
        return "未支持";
    if (status == kDeferred)
        return "暂缓";
    return "未知状态（" + (status.empty() ? std::string("空值") : status) + "）";
}

std::string CapabilityStatusHeadline(const model::RepairCapabilityBoundary& boundary) {
    const std::string covered = std::to_string(boundary.covered_scope.size());
    const std::string gap = std::to_string(boundary.not_covered_scope.size());
    if (boundary.status == kPartiallySupported) {
        // 词义与内容矛盾时按更保守的一侧读：不能让「已实现 0 项」被当成部分支持。
        if (boundary.covered_scope.empty())
            return "部分支持：但「已覆盖范围」为空 —— 状态词与内容矛盾，应按未支持解读";
        return "部分支持：本版本已实现 " + covered + " 项（见「已覆盖范围」），另有 " + gap +
               " 项未覆盖（见「未覆盖范围」）；本条描述能力整体，不代表本次运行已生效";
    }
    if (boundary.status == kUnsupported) {
        if (!boundary.covered_scope.empty())
            return "未支持：但「已覆盖范围」非空 —— 状态词与内容矛盾，不能读成「本版本无实现」";
        return "未支持：本版本无任何实现，此处只声明边界";
    }
    if (boundary.status == kDeferred)
        return "暂缓：已知能力，本版本不排期；未覆盖范围与不可恢复范围同样成立";
    return "未知状态（" + (boundary.status.empty() ? std::string("空值") : boundary.status) +
           "）：不在状态词表内，不能据此判断支持程度";
}

const std::vector<model::RepairCapabilityBoundary>& AllCapabilityBoundaries() {
    static const std::vector<model::RepairCapabilityBoundary> boundaries = [] {
        std::vector<model::RepairCapabilityBoundary> all;
        all.push_back(MakeMoovRebuild());
        all.push_back(MakeParameterSetRecovery());
        all.push_back(MakeDecodableGopTranscode());
        // 总述在两张范围表都填好之后统一渲染 —— 提前渲染会拿到空的 covered_scope，
        // 写出一句「已实现 0 项」。
        for (auto& boundary : all) {
            boundary.status_label = CapabilityStatusLabel(boundary.status);
            boundary.status_headline = CapabilityStatusHeadline(boundary);
        }
        return all;
    }();
    return boundaries;
}

std::vector<model::RepairCapabilityBoundary> BuildCapabilityBoundaries(const model::RepairDiagnosis& diagnosis) {
    std::vector<model::RepairCapabilityBoundary> matched;
    for (const auto& base : AllCapabilityBoundaries()) {
        std::vector<std::string> hits;
        for (const auto& trigger : TriggersFor(base.code)) {
            const bool hit =
                std::any_of(diagnosis.findings.begin(), diagnosis.findings.end(),
                            [&trigger](const model::DamageFinding& finding) { return finding.code == trigger; });
            if (hit)
                hits.push_back(trigger);
        }
        if (hits.empty())
            continue;
        model::RepairCapabilityBoundary entry = base;
        entry.trigger_findings = std::move(hits);
        matched.push_back(std::move(entry));
    }
    return matched;
}

const char* BoundaryCodeForAction(model::RepairAction action) {
    if (action == model::RepairAction::S08_TolerantTranscode)
        return "decodable_gop_transcode";
    return nullptr;
}

} // namespace repair
} // namespace videoeye
