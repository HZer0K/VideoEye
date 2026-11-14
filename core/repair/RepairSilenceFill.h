#pragma once

// 阶段 6 / 切片 W：音频轨时间轴空洞的**静音填补**。
//
// 背景（实测确认）：S08 的音频链按源 PTS **逐帧定位**，源侧中间缺一段时把
// 位置直接跳过去（`next_pts_ = want`），于是产物里留着一段没有采样覆盖的时间。
// 这段空洞既没有内容、也不在任何结论里出现 —— 播放器怎么处理它（跳过 / 补静音 /
// 卡住）取决于实现，报告一个字都不说。
//
// 这个切片只做一件事：把「相邻已解码音频帧的 PTS 间隔 vs 编码块长度」当作证据，
// 识别出可解释的采样空洞，并在用户**显式启用**时用静音把时间轴补回去。
//
// 与视频黑帧（切片 U）的三条边界完全同构：
//   1. 静音是**合成内容**，不是从源解出来的采样。单独记账，绝不进恢复率分子。
//   2. 只有「缺失量至少半个编码块」才算洞：容器时基取整（TS 的 1/90000 落在
//      44100 上会抖 ±1 采样）不足以构成空洞，按抖动补静音会往产物里灌噪声。
//   3. 单处超上限不填（大段缺失该由「未恢复区间」如实呈现），累计超预算不填。
//
// 两条只属于音频的边界：
//   4. **码流复制轨补不了**：不重编码就没有编码器，造不出这段采样。此时只有
//      探测结论，动作必须写成「未填补（该轨为码流复制）」—— 不能写成「未启用」，
//      那会误导用户去改一个其实改了也没用的开关。
//   5. 判据只覆盖「同一轨相邻帧之间的采样空洞」。轨道起点之前的空白、整段轨道
//      缺失、视频轨空洞都不在这里。

#include <cstdint>
#include <string>

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

// 单处空洞的判定结果。只有 Fill 会真的写静音，其余都必须如实落账。
enum class SilenceFillVerdict {
    NoGap,                     // 缺失量在容差内，没有洞
    Fill,                      // 可解释的采样空洞，用户已启用且该轨可重编码 → 补静音
    NotRequested,              // 有洞，但用户没启用
    MergeContiguous,           // gap_policy 要求塌缩空洞 → 不补（策略覆盖需求）
    CopyTrackCannotSynthesize, // 有洞，但该轨是码流复制：手里没有编码器
    GapTooLarge,               // 单处超过上限 → 不补
    BudgetExhausted,           // 本次累计补进去的静音已到上限
    NoSampleRate,              // 采样率未知 → 无法把秒数换算成采样数
    EncoderRejected,           // 判为可补，但编码器只接受了一部分（调用方回填）
};

// 判定用的边界参数。sample_rate 与 block_samples 必须与**编码器**一致
// （重采样之后的时基），否则「缺几个采样」会被算错，补出来的静音长度也就错了。
struct SilenceFillPolicy {
    int sample_rate = 0;
    // 一个编码块有多少采样（aac 1024）。编码器不声明块长（flac）时由调用方
    // 退到一个代表值；<= 0 时判据只按采样率推一个默认容差。
    int64_t block_samples = 0;
    // 用户是否显式启用（RepairRequest::fill_audio_gaps_with_silence）。
    bool fill_enabled = false;
    // gap_policy == MergeContiguous：用户要求把损坏区间塌缩掉。
    bool merge_contiguous = false;
    // 该轨是否走了重编码。false（码流复制）= 没有编码器，造不出静音。
    bool reencode_available = true;
    // 单处空洞上限（秒）。超过就不补。
    double max_gap_seconds = 5.0;
    // 本次累计合成静音时长上限（秒）。
    double max_total_seconds = 30.0;
};

struct SilenceFillPlan {
    model::RepairSyntheticAudioGap gap;
    SilenceFillVerdict verdict = SilenceFillVerdict::NoGap;
};

// 纯函数：expected_samples 是「上一帧结束时的采样位置」，actual_samples 是当前帧
// 实际开始的位置（都用编码器采样率计）。两者之差就是缺口。不做 IO、不碰 FFmpeg。
SilenceFillPlan PlanSilenceFill(int64_t expected_samples, int64_t actual_samples, const SilenceFillPolicy& policy,
                                int64_t inserted_samples_so_far);

// 稳定机读码（audio_silence_*），测试钉它，不钉中文文案。
const char* SilenceFillReasonCode(SilenceFillVerdict verdict, bool fill_enabled);

// 唯一渲染入口：一处空洞 → 一行说明。
std::string SilenceFillGapText(const model::RepairSyntheticAudioGap& gap);

// 唯一渲染入口：单轨汇总行（"音频流 #1：探测到 N 处空洞 / 补静音 X 采样（y.yyy s）"）。
std::string SilenceFillSummary(const model::RepairSyntheticAudioFill& fill);

// 唯一渲染入口：判据边界说明（含「没探测」时的说法，不能留空让报告自己编）。
std::string SilenceFillBasisNote(const model::RepairSyntheticAudioFill& fill);

} // namespace repair
} // namespace videoeye
