#pragma once

// 流媒体清单解析的取消检查。
//
// 为什么清单解析也必须可取消: HLS/DASH 清单的解析量完全由文件内容决定 —— 一个几十万行
// EXTINF 的媒体播放列表、或上千个 Representation 的 MPD，逐行 / 逐分片 / 逐时间轴条目的
// 循环加起来可以跑很久（Validate 里的时长抖动比对还是 O(n²)）。而 QtAnalysisController
// 在启动新扫描时会 join 旧线程，解析期间的中断不了就等于"点取消没反应、重新扫描也点不动"。
//
// 三个解析器（HLS / DASH / SegmentQc）共用这一份判断，避免各自实现一套语义不同的取消。
// 头文件只依赖 <atomic>，不引入 Qt / FFmpeg，纯逻辑单测可继续直接调用。

#include <atomic>

namespace videoeye {
namespace analyzer {

// 取消标志为 nullptr 表示"调用方不关心取消"（离线批处理、单测），此时恒返回 false。
// 用 memory_order_acquire 读取: 解析线程与置位线程之间需要建立可见性。
inline bool IsStreamingCancelled(const std::atomic<bool>* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_acquire);
}

// 循环里按固定间隔检查取消，避免在每行都做一次原子读把热路径拖慢。
// 传进来的计数是"已处理条数"，返回 true 时调用方应立即中断并标记结果不完整。
constexpr unsigned kStreamingCancelCheckMask = 0x3FFu;  // 每 1024 次检查一遍

inline bool ShouldCheckStreamingCancel(unsigned long long processed) {
    return (processed & kStreamingCancelCheckMask) == 0;
}

} // namespace analyzer
} // namespace videoeye
