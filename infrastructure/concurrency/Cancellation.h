#pragma once

// 取消信号（cancel flag）的统一判空入口。
//
// 全项目的可中断流程都用一个 std::atomic<bool> 当取消标志：AnalysisEngine 持有它，
// 传给 FFmpeg 的 AVIO 中断回调、容器解析器、分片探测等。调用方大多是"可选取消"
// （nullptr = 不关心取消），于是每个解析循环里都要写一遍
//
//     if (cancel && cancel->load(std::memory_order_acquire)) { ... }
//
// 漏判空不崩，只是悄悄不生效 —— 这类 bug 极难从现象反推。所以判空统一收在这里。
//
// 依赖方向：infrastructure 只能靠 stdlib（check_layering.py 硬规则），别在这里 include core/*。

#include <atomic>

namespace videoeye {
namespace infrastructure {

/// 取消标志是否已被置位。cancel 为 nullptr 时恒为 false（该流程不参与取消）。
inline bool IsCanceled(const std::atomic<bool>* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_acquire);
}

/// 循环内的取消检查：命中即返回 true，调用方据此退出并标记"已取消"。
///
/// 用法：在每次迭代开头写 `if (infra::Checkpoint(cancel)) break;`
/// 只在入口查一次是不够的 —— 大文件扫描会一路走到天亮，期间没人再抬头看一眼标志。
inline bool Checkpoint(const std::atomic<bool>* cancel) {
    return IsCanceled(cancel);
}

} // namespace infrastructure
} // namespace videoeye
