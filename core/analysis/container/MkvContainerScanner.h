#pragma once

// MKV / WebM 容器级体检（顺序扫描元素头，不建树、不读载荷）。
//
// 为什么另写一遍而不是复用 EbmlAnalyzer
// ------------------------------------
// EbmlAnalyzer 的产物是结构页要的**整棵元素树**：它把文件整份读进内存，
// 节点数受 ParserLimits::kMaxNodes（20 万）约束。两件事都决定了它不能当 QC 的
// 事实来源：
//   * 一部 3 小时的 MKV 有十万量级 Block，树一定撞上限 —— 而撞上限意味着
//     EbmlAnalyzer 直接判解析失败（半棵树不许当有效结果发出去），
//     QC 侧拿到的就是"什么都没分析到"；
//   * 文件级别上，整份读进内存对手里动辄几个 G 的母版素材不成立。
//
// 这一份只做一件事：按 EBML 元素头逐级跳过，记录 Cluster 时间戳序列、
// Tracks 的 CodecPrivate 有无、Cues / SeekHead / Tags 是否存在、以及
// unknown-size 元素个数。内存 O(1)，读放大接近 1（只读元素头）。
//
// 未知长度（size 全 1）在 Matroska 里很常见（流式写入的 Segment / Cluster），
// 必须按"延伸到下一个 level-1 元素为止"处理，否则后面整段都会错位。

#include <atomic>
#include <cstdint>
#include <string>

#include "core/domain/model/ContainerDiagnosticsInfo.h"

namespace videoeye {

struct MkvScanOptions {
    // 元素头预算。一级扫描只会碰 Cluster / Cues / Tracks 这些，几百个；
    // 只有 unknown-size Cluster 才需要逐个跳过内部 Block，所以正常文件远达不到。
    // 20 万足以覆盖 10 小时以上的长片，同时挡住畸形文件把扫描拖死。
    uint64_t max_elements = 200000;
};

/// MKV/WebM 元素级顺序扫描器（QC 用事实来源）
class MkvContainerScanner {
public:
    // 返回 false 表示"不是可解析的 EBML/MKV/WebM"（魔数不符 / 找不到 Segment /
    // 打开失败）。成功时 diagnostics.analyzed=true。
    // 被取消时返回 false 且 analyzed 保持 false —— 调用方必须自己用取消令牌区分
    // "取消"和"失败"，不要把取消换成 fallback 成功。
    bool Analyze(const std::string& file_path, model::MkvDiagnostics& diagnostics,
                 const std::atomic<bool>* cancel = nullptr, const MkvScanOptions& options = MkvScanOptions());
};

} // namespace videoeye
