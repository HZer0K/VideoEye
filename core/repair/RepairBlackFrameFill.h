#pragma once

// 阶段 6 / 切片 U：视频轨时间轴空洞的**黑帧填补**。
//
// 背景（实测确认，不是推测）：S08 的视频 PTS 是 `帧序号 × 1/fps` 重新生成的，
// 源侧时间戳只用来算一次「统一原点」。于是源视频轨中间缺一段（丢包 / 局部
// 解码失败 / 录制被打断）时，产物把这段静默压掉：视频变短，音频仍按原时长
// 走，越到后面口型差得越多，而报告里一句话都没有。
//
// 这个切片只做一件事：把「源解码帧的 PTS 间隔 vs 标称帧率」当作证据，识别出
// 可解释的整帧空洞，并在用户**显式启用**时用合成黑帧把时间轴补回去。
//
// 三条边界必须随结论一起报出去：
//   1. 黑帧是**合成内容**，不是从源解出来的。它单独记账，且绝不进恢复率分子
//      —— 否则「填了 5 s 黑场」会被算成「多恢复了 5 s 内容」。
//   2. 间隔不是标称帧时长的整数倍时**不填**：这类跳变更可能是时间戳本身坏了
//      （stts 被改坏 / 拼接处时基不一致），按帧数外推补黑场等于用编出来的
//      画面掩盖一个未诊断的时间轴缺陷。
//   3. 单处空洞超过上限也**不填**：大段缺失该由「未恢复区间」如实呈现，
//      不该用黑场抹平成「看起来完整」。
//
// 判据只覆盖「相邻解码帧之间有整帧空洞」这一种形态。整段轨道缺失、轨道起点
// 之后的空白、音轨空洞（需要转码成容器能收的编码，本模块只有 copy/drop）
// 都不在这个判据里。

#include <cstdint>
#include <string>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

// 单处空洞的判定结果。只有 Fill 会真的插帧，其余都必须如实落账。
enum class BlackFillVerdict {
    NoGap,           // 相邻解码帧的源 PTS 间隔在容差内，没有洞
    Fill,            // 可解释的整帧空洞，用户已启用 → 插帧
    NotRequested,    // 可解释的整帧空洞，用户未启用 → 只记账，不插帧
    MergeContiguous, // gap_policy 明确要求塌缩空洞 → 不插帧（策略覆盖需求）
    GapTooLarge,     // 单处超过上限 → 不插帧
    NotIntegral,     // 间隔不是标称帧时长的整数倍 → 时间戳可疑，不猜
    NoFrameRate,     // 帧率不是源侧证据 → 无法把秒数换算成帧数
    BudgetExhausted, // 本次累计合成帧数已到上限
    EncoderRejected, // 判为可填，但编码器只接受了其中一部分（调用方回填的结论）
};

// 判定用的边界参数。frame_seconds 必须与编码器时基一致（1/fps），否则
// 「缺几帧」会被算错，插出来的黑场长度也就跟着错。
struct BlackFillPolicy {
    double frame_seconds = 0.0;
    // 帧率是否来自源侧证据（容器声明 / 码流）。兜底 25 fps 不算。
    bool frame_rate_from_source = false;
    // 用户是否显式启用（RepairRequest::fill_video_gaps_with_black_frames）。
    bool fill_enabled = false;
    // gap_policy == MergeContiguous：用户要求把损坏区间删掉让播放连续。
    bool merge_contiguous = false;
    // 单处空洞上限（秒）。超过就不填。
    double max_gap_seconds = 5.0;
    // 本次累计合成帧数上限。
    int64_t max_total_frames = 750;
};

// 判定结果：gap 直接是报告用的结构，verdict 给测试与调用方用。
struct BlackFillPlan {
    model::RepairSyntheticGap gap;
    BlackFillVerdict verdict = BlackFillVerdict::NoGap;
};

// 纯函数：给定相邻两个解码帧的源 PTS（秒）与边界参数，判定中间是否有空洞、
// 该不该填、填几帧。不做 IO、不碰 FFmpeg。
BlackFillPlan PlanBlackFill(double previous_seconds, double current_seconds, const BlackFillPolicy& policy,
                            int64_t synthetic_frames_so_far);

// 稳定机读码（black_fill_*），测试钉它。
const char* BlackFillReasonCode(BlackFillVerdict verdict, bool fill_enabled);

// 唯一渲染入口：一处空洞 → 一行说明。报告层只打印，不自己拼。
std::string BlackFillGapText(const model::RepairSyntheticGap& gap);

// 唯一渲染入口：汇总行（"探测到 N 处空洞 / 填补 M 处共 K 帧（x.xx s）…"）。
std::string BlackFillSummary(const model::RepairSyntheticFill& fill);

// 唯一渲染入口：判据边界说明（含「没探测」时的说法，不能留空让报告自己编）。
std::string BlackFillBasisNote(const model::RepairSyntheticFill& fill);

} // namespace repair
} // namespace videoeye
