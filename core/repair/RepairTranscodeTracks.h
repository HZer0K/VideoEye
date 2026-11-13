#pragma once

// 阶段 6 / 切片 T：S08 的**轨级决策**。
//
// 旧实现的策略是一句隐含假设：「主视频轨重编码 + 每条音频轨按原码流复制」。
// 三处后果都由实测确认：
//   1. 第二视频轨 / 封面图 / 字幕 / 数据轨**无声消失**，报告里一个字都没有；
//   2. 音频轨一律复制参数，目标容器拒绝其中一条时 `avformat_write_header`
//      整体失败，整个转码连带失败，错误文案只说「音频编码参数可能不被目标容器
//      接受」，不说哪一条；
//   3. 「为什么这条轨要重编码」从来没有落过账，报告里只有「视频轨经过重编码」。
//
// 这里把每条输入轨的处理显式落成 copy / transcode / drop 三态 + 稳定理由码，
// 并给出一条覆盖自证（每条输入轨恰好一条决策）。
//
// 判据边界（必须随结论一起报出去）：
//   * 「容器接不接受某条音频轨」由**内存试写头**实测得出（不落临时文件），
//     不查表、不按扩展名推断；只放该音频轨一条流做实验，因此结论只覆盖
//     「容器 × 该编码」，不代表整份产物一定能写。
//   * 主视频轨的「是否有编码层损伤证据」来自诊断层的 decode 层 finding
//     （`location.stream_index` 命中该轨），不是从「我们要重编码」反推的。
//   * 除主视频轨外的视频轨一律 drop：S08 只有一条解码-重编码链，这不是容器
//     限制，是本模块的能力边界。

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/codec_par.h>
}

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

// 目标容器对某条轨的接受性：只允许「实测接受 / 实测拒绝 / 没测」三种。
enum class TranscodeContainerFit {
    Unknown,  // 没测（没有可用的封装器上下文 / 探测过程本身失败）
    Accepted, // 试写头返回成功
    Rejected, // 试写头返回错误 —— 复制该轨会让整份产物写头失败
};

struct TranscodeTrackProbe {
    int input_stream_index = -1;
    // video / attached_pic / audio / subtitle / data / attachment / other
    std::string kind;
    std::string source_codec;
    // 诊断层是否给出该轨的编码层损伤证据（只对主视频轨有意义）
    bool decode_damage_detected = false;
    // 源侧已标记丢弃（AVDISCARD_ALL）：不会被读出来，产物必然没有该轨。
    bool discarded = false;
    // 音频轨：目标容器是否接受该编码（实测）；非音频轨保持 Unknown
    TranscodeContainerFit container_fit = TranscodeContainerFit::Unknown;
    // 实测的原始原因（拒绝时是封装器的原话，Unknown 时是为什么没测成）。
    // 报告里跟在理由后面，避免把「封装器原话」和我们的归纳混成一句。
    std::string container_fit_detail;
};

struct TranscodeTrackPlan {
    std::vector<model::RepairTranscodeTrackDecision> decisions;
    int primary_video_index = -1;
    std::size_t copy_count = 0;
    std::size_t transcode_count = 0;
    std::size_t drop_count = 0;
    // 覆盖自证：每条探针都有且仅有一条决策。false = 有轨被吞，报告必须明写。
    bool complete = false;
    // 渲染好的汇总（"复制 1 轨 / 重编码 1 轨 / 未保留 2 轨"）与依据边界说明。
    std::string summary;
    std::string note;
};

// 流类型 → 决策表里的 kind 词表。attached_pic（封面图）单独一类：它虽然挂在
// 视频流上，但不是内容轨，不能被选成主视频轨去重编码。
const char* TranscodeTrackKindName(AVMediaType type, bool attached_pic);

// 主视频轨选择：第一条非封面图（attached_pic）的视频轨。找不到返回 -1。
// 抽出来是因为旧实现直接用 av_find_best_stream，封面图被当成主视频轨时会
// 拿一张静态图去重编码整部片子。
int SelectPrimaryVideoIndex(const std::vector<TranscodeTrackProbe>& probes);

// 纯函数：按固定规则给每条轨判定 copy / transcode / drop。
// 规则表见 .cpp 顶部的注释；理由码是机读稳定标识，测试钉它。
TranscodeTrackPlan PlanTranscodeTracks(const std::vector<TranscodeTrackProbe>& probes);

// 容器是否接受该编码：内存试写头实测（不落临时文件、不需要真编码）。
// detail 回填失败原因（拒绝时是封装器的原话），可为 nullptr。
TranscodeContainerFit ProbeTrackFitsContainer(const char* format_name, const AVCodecParameters* parameters,
                                              std::string* detail);

// 唯一渲染入口：一条决策 → 一行说明。报告层只打印，不自己拼。
std::string TranscodeTrackDecisionText(const model::RepairTranscodeTrackDecision& decision);

// 「未保留」清单用的短形式（不含动作词）：报告里未保留项已经有「未保留: 」前缀，
// 再拼一次动作词就变成「未保留: … → 未保留：…」。理由与理由码保持不变。
std::string TranscodeTrackOmittedText(const model::RepairTranscodeTrackDecision& decision);

} // namespace repair
} // namespace videoeye
