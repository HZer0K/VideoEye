#include "core/repair/RepairDeclaredExclusions.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace videoeye {
namespace repair {
namespace {

model::RepairDeclaredExclusion MakeBase(const char* code, const char* title, const char* applies_to,
                                        const char* consequence, const char* evidence) {
    model::RepairDeclaredExclusion item;
    item.code = code;
    item.title = title;
    item.scope = "version_capability";
    item.applies_to = applies_to;
    item.consequence = consequence;
    item.evidence = evidence;
    return item;
}

// 每条只认结构化字段。返回空 = 本次没有事实依据，不许挂载这条声明。
// ⚠️ 不从 summary / error / limitations 等自由文本反推类别。
bool AnyAttemptedExecution(const model::RepairResult& result) {
    return std::any_of(result.executions.begin(), result.executions.end(),
                       [](const model::RepairActionExecution& execution) { return execution.attempted; });
}

std::vector<std::string> TriggerFactsFor(const std::string& code, const model::RepairResult& result) {
    std::vector<std::string> facts;
    if (code == "edit_list_not_rebuilt") {
        for (const auto& execution : result.executions) {
            if (!execution.attempted)
                continue;
            facts.push_back("executions[].attempted=true");
            facts.push_back(std::string("executions[].action=") + model::ToString(execution.action));
            break;
        }
    } else if (code == "vfr_frame_durations_not_recovered") {
        if (result.timeline_basis.vfr_declared)
            facts.push_back("timeline_basis.vfr_declared=true");
        // mode 默认就是 unknown（没拿到证据），它本身不构成「本次修复了什么」
        // 的事实：预检失败 / 诊断之前就结束的路径不得借它挂免责声明。
        if (result.timeline_basis.mode == "unknown" && AnyAttemptedExecution(result))
            facts.push_back("timeline_basis.mode=unknown");
        // 切片 M：参考样片辅助重建（C/H/I）的逐帧时长来自参考片的 stts/mdhd，
        // 源侧 moov 已丢失。这是「借来的时基」而非源侧实测，同样适用本条声明。
        if (result.timeline_basis.mode == "reference_borrowed")
            facts.push_back("timeline_basis.mode=reference_borrowed");
    } else if (code == "original_display_offsets_not_restored") {
        if (result.display_order.analyzed) {
            facts.push_back("display_order.analyzed=true");
            if (result.display_order.ctts_written)
                facts.push_back("display_order.ctts_written=true");
        }
    } else if (code == "input_timeline_span_not_reconstructed") {
        if (result.fidelity.video_reencoded)
            facts.push_back("fidelity.video_reencoded=true");
    } else if (code == "synthetic_content_not_inserted") {
        if (result.fidelity.video_reencoded && !result.fidelity.synthetic_frames_inserted) {
            facts.push_back("fidelity.video_reencoded=true");
            facts.push_back("fidelity.synthetic_frames_inserted=false");
        }
    } else if (code == "transcode_properties_not_inherited") {
        // 切片 J 的反向守护：没探测过就不能拿「有损失」当事实，更不能拿空表
        // 冒充「没有损失」。只有探测过且实测出丢失项才适用。
        if (result.fidelity.transcode.inspected && !result.fidelity.transcode.lost_properties.empty()) {
            facts.push_back("fidelity.transcode.inspected=true");
            for (const auto& lost : result.fidelity.transcode.lost_properties)
                facts.push_back("fidelity.transcode.lost_properties[]=" + lost);
        }
    } else if (code == "fragment_resync_not_attempted") {
        if (result.diagnosis.fragmented)
            facts.push_back("diagnosis.fragmented=true");
    } else if (code == "gop_local_reencode_not_attempted") {
        if (result.fidelity.video_reencoded)
            facts.push_back("fidelity.video_reencoded=true");
    } else if (code == "extra_tracks_not_rebuilt") {
        // 切片 T：只有「决策表里真的被判未保留」的轨才构成适用事实。
        // 容器拒绝（audio_drop_container_rejected）与源侧已丢弃（stream_marked_discard）
        // 不属于「我们不重建」这条能力边界，得排除掉 —— 否则会把别人的决定
        // 算成自己的能力缺口。
        for (const auto& decision : result.fidelity.transcode.track_decisions) {
            if (decision.action != "drop")
                continue;
            if (decision.reason_code == "audio_drop_container_rejected" ||
                decision.reason_code == "stream_marked_discard")
                continue;
            facts.push_back("fidelity.transcode.track_decisions[]=" + decision.text);
        }
    }
    return facts;
}

} // namespace

const std::vector<model::RepairDeclaredExclusion>& AllDeclaredExclusions() {
    static const std::vector<model::RepairDeclaredExclusion> catalog = [] {
        std::vector<model::RepairDeclaredExclusion> items;
        items.push_back(MakeBase("edit_list_not_rebuilt", "编辑列表（elst）不重建", "重建样本表 / 时间轴的策略",
                                 "产物不保证保留原始编辑偏移、起始裁剪与由 elst 表达的音画同步起点；"
                                 "源是否含 elst 本次未检测，因此也不能断言这些偏移已经丢失。",
                                 "路线图 F/I/K 明确不做 elst 重建：负偏移用 ctts version 1 表达，"
                                 "不做「整体平移 + 裁开头」；没有样本表就没有逐帧时长的证据。"));
        items.push_back(MakeBase("vfr_frame_durations_not_recovered", "VFR 逐帧时长不恢复",
                                 "时基证据不足 / 码流声明变帧率 / 参考样片辅助重建",
                                 "样本时长只能按名义或推定恒定帧率写入、或从参考样片借用样本时长表；"
                                 "若源为 VFR，逐帧时长与原始不一致，报告不得据此声称逐帧时长已恢复。",
                                 "参考路径 I 只重建完整 stts run 序列；无参考且无样本表时逐帧时长没有证据，"
                                 "时基 unknown 时按推定帧率从零重建（切片 K）。参考样片辅助重建（C/H/I）"
                                 "借的是参考片的 stts/mdhd，源侧 moov 已丢失，逐帧时长同样没有独立证据（切片 M）。"));
        items.push_back(MakeBase("original_display_offsets_not_restored", "原始 B 帧显示偏移不恢复",
                                 "做过显示顺序分析的路径",
                                 "产物里的显示顺序是 POC 推导的重建值（或保持解码序），不是原始 ctts；"
                                 "原始 B 帧偏移与首帧显示延迟不保证保留。",
                                 "切片 E 用 POC 推导显示序并按需写 ctts，这是重建而非还原原始样本表；"
                                 "开放 GOP 前导图像仍判不可用，pic_order_cnt_type == 1 不推缺帧。"));
        items.push_back(MakeBase("input_timeline_span_not_reconstructed", "输入侧时间轴覆盖区间不重建",
                                 "S08 容忍式转码",
                                 "产物时间轴按推定帧率从零重建，不提供输入侧覆盖区间；"
                                 "不可与 lost_ranges 的损失区间相减，也不得把 gap_count=0 读成无损。",
                                 "切片 K 刻意不补输入侧时间戳：解码器异步、源包与输出帧不是一一对应，"
                                 "硬关联只会造出假的覆盖区间。"));
        items.push_back(MakeBase("synthetic_content_not_inserted", "本次没有插入合成黑帧 / 静音 / 重复帧",
                                 "S08 容忍式转码",
                                 "产物里没有补出来的画面或声音：要么本次未启用「空档补黑帧」，要么源时间轴"
                                 "没有可解释的整帧空洞。静音填补与重复帧填补仍属空能力 —— 音轨只有码流复制 / "
                                 "剔除两条路，补静音必须先把它转码成目标容器能收的编码。",
                                 "切片 U 已实现**显式启用**的视频黑帧填补（逐处空洞记账、填补时长不计入恢复率"
                                 "分子）；黑帧以外的合成内容仍未实现，synthetic_frames_inserted=false 只表示"
                                 "这次没插，不代表能力缺失清单里「没有这一项」。"));
        items.push_back(MakeBase("transcode_properties_not_inherited", "实测到的转码属性损失不补回",
                                 "S08 且做过源侧保真探测",
                                 "位深 / 色彩标记 / 旋转矩阵 / HDR 静态元数据等实测损失本版本不补回；"
                                 "产物只能说「属性损失已列明」，不能声称视觉无损。",
                                 "切片 J 起逐项实测并写入 fidelity.transcode；同色深的 SAR / 色度位置 / 色彩标记"
                                 "已继承，旋转与 HDR 只记账不继承，10-bit 编码器尚未选型。"));
        items.push_back(MakeBase("fragment_resync_not_attempted", "容器级分片重同步不做", "分片（fragmented）输入",
                                 "不做 moof/trun/mdat 级重同步与分片样本表重建；分片边界丢失的内容不会"
                                 "通过容器重同步找回，只能靠已存在的初始化段与可读分片恢复。",
                                 "切片 D 层 1 明确不做：需要另一套「分片样本表重建 + 自建 init/moof」流程，"
                                 "当前只做层 2/3 的直接扫描与 GOP 恢复。"));
        items.push_back(
            MakeBase("gop_local_reencode_not_attempted", "GOP 级局部重编码不做", "S08 容忍式转码",
                     "整条视频轨统一重编码，未按可解码 GOP 精确界定重编码范围，"
                     "也未验证 profile / level / 像素格式 / time_base 的拼接边界兼容性。",
                     "阶段 6 / 切片 T 已落地逐轨决策（copy / transcode / drop + 实测容器接受性），"
                     "但重编码范围仍是整条视频轨：按 GOP 局部重编码需要拼接边界参数兼容性校验，本版本未做；"
                     "实际使用的编码器以报告「重编码编码器」一行为准（本构建首选 libx264 不可用，会回退）。"));
        items.push_back(MakeBase("extra_tracks_not_rebuilt", "第二视频轨 / 字幕 / 数据轨不重建", "S08 容忍式转码",
                                 "S08 的输出链只有一条解码-重编码视频轨与可复制的音频轨；其余轨（第二视频轨、"
                                 "封面图、字幕、数据、附件）在产物中不存在，已在「轨道决策」表与「未保留」"
                                 "清单里逐条列出，不能读成「源里没有这些轨」。",
                                 "切片 T：轨级决策显式落账。多视频轨需要并行的解码-重编码链与各自的时基 / "
                                 "参数集管理，本版本未做；容器拒绝的音频轨宁可丢弃并报出，也不让整份产物"
                                 "写头失败。"));
        return items;
    }();
    return catalog;
}

model::RepairDeclaredExclusions BuildDeclaredExclusions(const model::RepairResult& result) {
    model::RepairDeclaredExclusions declared;
    declared.available = true;
    declared.limitations = {
        "本清单只列「本版本明确不做且本次有事实依据命中」的项目，不是本次损失清单；"
        "未列出的能力不代表已覆盖。",
        "每条只声明能力口径：源是否真的含有该项内容未做检测，不能把「不保证保留」读成「已丢失」。",
    };
    // 诊断 / 执行之前就结束（请求预检失败、产物路径预留失败）时，没有任何
    // 「本次采用了什么修复策略」的事实，不能把全局能力声明糊到这次运行上。
    const bool entered_repair = !result.diagnosis.checks.empty() || !result.executions.empty();
    if (!entered_repair) {
        declared.limitations.push_back("本次在诊断 / 执行之前结束，没有可供判定适用性的修复事实，因此不挂载能力声明。");
        return declared;
    }
    for (const auto& base : AllDeclaredExclusions()) {
        std::vector<std::string> facts = TriggerFactsFor(base.code, result);
        if (facts.empty())
            continue;
        model::RepairDeclaredExclusion item = base;
        item.trigger_facts = std::move(facts);
        declared.items.push_back(std::move(item));
    }
    return declared;
}

} // namespace repair
} // namespace videoeye
