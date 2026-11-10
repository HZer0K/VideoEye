#include "core/repair/RepairCapabilityContracts.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace videoeye {
namespace repair {
namespace {

model::RepairCapabilityBoundary MakeBase(const std::string& code) {
    model::RepairCapabilityBoundary b;
    b.code = code;
    b.status = "unsupported";
    return b;
}

model::RepairCapabilityBoundary MakeMoovRebuild() {
    auto b = MakeBase("moov_rebuild");
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
                 "当前版本已实现受限版无参考重建（RepairPlainMp4Rebuilder：ftyp + 恰好一个 mdat + "
                 "纯 AVCC H.264 或纯 ADTS AAC 单轨，且 SPS VUI 提供时基证据）。"
                 "本契约的 unsupported 指的是尚未覆盖的范围：音视频交织 mdat、HEVC、多 mdat、多轨、"
                 "以及 B 帧显示顺序 / edts / ctts 的原始取值 —— 这些仍不能安全反推。";
    return b;
}

model::RepairCapabilityBoundary MakeParameterSetRecovery() {
    auto b = MakeBase("parameter_set_recovery");
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
        "在真实媒体上复现过时基篡改。当前版本已能借同源初始化段（含 R03 时基核验）或由 in-band "
        "SPS/PPS(VPS) 自建视频初始化段（RepairInitSegmentBuilder）；本契约的 unsupported 指参数集"
        "完全缺失、裸 AAC 无 AudioSpecificConfig、音频轨自建等仍物理不可恢复的部分。";
    return b;
}

model::RepairCapabilityBoundary MakeDecodableGopTranscode() {
    auto b = MakeBase("decodable_gop_transcode");
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
                        "⚠️ 当前 S08 不插入黑帧 / 重复 / 合成帧，synthetic_frames_inserted 恒为 false；"
                        "若将来启用填补策略，必须与恢复出的原始内容分开记账，不能计入恢复率分子。";
    b.quality_risks = {
        "重编码代际损失（二次压缩），低码率下块效应与细节丢失明显",
        "丢帧导致卡顿、合成帧导致画面冻结或抖动；GOP 边界判断错误会让段首花屏",
        "音画不同步：视频时间轴重算后音频若按原时间戳复制，偏差会越积越大",
        "无参考样片时 perceptual_fidelity 只能是 unknown_without_reference，不能声称画质无损",
    };
    b.evidence = "S08_TolerantTranscode 已实现并可运行：RecoverContent 模式下解码可读视频帧、"
                 "以 libx264 重编码、音频尽量按原码流复制，并做抽帧 PSNR 画质评估"
                 "（RepairTolerantTranscode / RepairExecutor）。本契约的 unsupported 指尚未覆盖的"
                 "GOP 级局部重编码：按可解码 GOP 精确界定重编码范围、验证拼接边界兼容性，"
                 "以及原始码流字节与原始时间戳精度的恢复；这些能力仍需单独验证后才能上线。";
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

const std::vector<model::RepairCapabilityBoundary>& AllCapabilityBoundaries() {
    static const std::vector<model::RepairCapabilityBoundary> boundaries = [] {
        std::vector<model::RepairCapabilityBoundary> all;
        all.push_back(MakeMoovRebuild());
        all.push_back(MakeParameterSetRecovery());
        all.push_back(MakeDecodableGopTranscode());
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
