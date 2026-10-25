#pragma once

// 帧级顺序的统计汇总：把逐帧明细（FrameOrderTimeline::frames）算成
// 重排次数 / 重排深度 / GOP 结构 / POC 重复 / POC 缺口。
//
// 为什么从 H264PocTracker 里抽出来独立成文件：这段规则**不含任何编码器语义**，
// 它只看 FrameOrderTimeline 的字段。H.264 与 HEVC 的 POC 推导公式完全不同，
// 但下面三条判据一模一样：
//   * 解码顺序里 POC 变小 = 显示顺序被重排（B 帧存在的直接证据）
//   * POC 在随机接入点归零 = GOP 边界（不是"重排"，也不是"重复"）
//   * POC 集合的最小正差 = 一帧占几个 POC 槽（据此推"缺了几帧"）
// 抄成两份必然会在第二次改动时只改一处 —— 那样两个编码器给出的同一份文件会
// 得出互相矛盾的结论，而这种矛盾通常要等到用户投诉才会被发现。

#include "core/domain/model/FrameOrderInfo.h"

namespace videoeye {

// 幂等：只读 timeline.frames，把结果写回 timeline 的统计字段。
// 不修改 frames / available / unavailable_reason / field_coded / truncated /
// frames_observed —— 那些是追踪器在扫包过程中定的，汇总层无权改。
void ComputeFrameOrderStatistics(model::FrameOrderTimeline& timeline);

} // namespace videoeye
