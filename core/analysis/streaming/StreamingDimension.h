#pragma once

// 流媒体包（HLS / DASH）的维度状态构建。
//
// 为什么单独成文件：流媒体清单走的是引擎的独立分流路径（StreamingManifestScan），
// 收尾时**不经过 AnalysisResultAssembler**（引擎在打开任何 IO 之前就 return 了）。
// 而 QC 规则、JSON 导出、流媒体面板全部以 DimensionExecuted(Streaming) 为闸门 ——
// 这条路径若不自己把 Streaming 维度落进 result.dimensions，闸门恒为 false，
// 一份有问题的清单它的所有流媒体问题都会被当成"没跑过"而整体丢弃。
//
// 装配器里的 BuildStreamingStatus 与本函数共用同一份口径（同一处构造，两处调用），
// 避免"清单路径"与"全扫描路径"各写一遍导致漂移。
//
// 纯 domain 依赖（DimensionStatus + StreamingPackageResult），不碰 Qt / FFmpeg，
// 因此可被只链 analysis/streaming 的单测直接编入。

#include "core/domain/model/DimensionStatus.h"
#include "core/domain/model/StreamPackageInfo.h"

namespace videoeye {

// 由一份**已成功执行**的流媒体包结果构建 Streaming 维度（executed=true）。
//
// 只负责"已执行"这一支：选项关闭 / 输入非清单的 skip 判定仍由调用方决定
// （装配器用 MakeSkippedDimension，清单路径根本不会走到这里）。
// conclusive 不在此置位 —— 与其它维度一致，由调用方统一 MarkConclusive()。
model::DimensionStatus BuildStreamingDimensionStatus(const model::StreamingPackageResult& package);

} // namespace videoeye
