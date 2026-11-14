#pragma once

// P2-13 S08 容忍式转码：解码可读帧后重新编码，并对产物做抽帧 PSNR 画质评估。
//
// 适用场景：源文件索引与时间戳都无法自洽（例如普通 MP4 的 moov 缺失、GOP 内
// 数据损坏但仍有可解码帧）。此时保码流重封装救不出编码层已损坏的内容，
// 只能"能解多少解多少，重新编码成可播放文件"。
//
// ⚠️ 契约：
//   * 视频：主视频轨统一重编码；第二视频轨 / 封面 / 字幕 / 数据轨显式列为未保留。
//   * 音频：目标容器接受原编码则按码流复制；拒绝时（切片 V）整轨解码后重编码成
//     实测能收的编码；一条候选都过不了试写头才剔除，并逐条报出被拒原因。
//   * 解码失败的包 / 帧被计数，不能静默丢弃。
//   * 产物帧率由源声明帧率推定为恒定帧率，时间轴会被改写 —— 必须写入保真度。
//   * 画质只做抽帧 PSNR（亮度平面），不是逐帧全量比对；样本数写进报告。

#include <atomic>
#include <string>

#include "core/domain/model/RepairRequest.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairBudget.h"
#include "core/repair/RepairExecutor.h"
#include "core/repair/RepairOutputWriter.h"
#include "core/repair/RepairProgress.h"

namespace videoeye {
namespace repair {

class RepairTolerantTranscode {
public:
    // 产物沿用 paths.part_path 命名；返回结构与重封装一致，便于引擎复用验证 / 发布流程。
    static RepairExecutionOutcome Execute(const model::RepairRequest& request, const model::RepairDiagnosis& diagnosis,
                                          const model::RepairPlan& plan, const RepairOutputPaths& paths,
                                          const std::atomic<bool>* cancel, const RepairProgress& progress,
                                          const RepairRunBudget* budget);
};

// 测试专用故障注入（默认全关）：模拟「已经写过若干包之后」写包失败 / 封装尾失败。
// 真实磁盘错误无法在单测里稳定制造，但「部分写入后仍报成功」是必须钉死的回归。
// nth_write = 0 表示关闭；> 0 表示在第 N 次写包调用处返回失败。
void SetTranscodeWriteFailureForTesting(int nth_write);
void SetTranscodeTrailerFailureForTesting(bool enabled);

// 测试专用故障注入（默认关闭，-1 = 关）：把指定输入音频轨的「目标容器是否接受」
// 强制判成拒绝，用来跑通音频重编码链。
//
// 为什么必须是注入而不是造语料：源容器里能装下的编码，目标容器也装得下 ——
// mov 写不进 flac / opus，所以「mov + flac」这种被拒组合永远不会成为源。
// 注入只影响**容器接受性这一个判定**，转码目标本身仍然是真实实测出来的
// （编码器存在 + 参数能开 + 试写头接受），链上的编解码 / 时间轴 / 记账全是真的。
void SetTranscodeAudioContainerRejectedForTesting(int input_stream_index);

} // namespace repair
} // namespace videoeye
