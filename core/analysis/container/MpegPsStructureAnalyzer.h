#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {

// MPEG-PS（Program Stream，.mpg/.vob/.m2p）结构解析选项。
//
// 与 TS 不同，PS 没有固定包长，也没有 PAT/PMT：流表只能从 PSM（Program Stream Map）
// 或 PES 的 stream_id 推断。老 .mpg/.vob 里 PSM 经常缺失，所以两条路都要走。
struct MpegPsStructureOptions {
    // 是否生成元素树（QC 只取事实时可以关掉）。
    bool build_tree = true;

    // 最多扫描多少字节（<= 0 表示扫到文件尾）。
    // 默认 32 MiB：足以覆盖 PSM/系统头/各类 PES，又不会让 4 GB 的 VOB 打开即卡死。
    int64_t max_scan_bytes = 32LL << 20;

    // 树里最多保留多少个子节点（Pack Header 可能上万个，全塞进树没有意义）。
    uint32_t max_tree_elements = 200;
};

/// MPEG-PS 容器结构解析器
///
/// 覆盖：
///   * Pack Header（0xBA，MPEG-1 12 字节 / MPEG-2 14 字节 + stuffing）
///   * System Header（0xBB）
///   * Program Stream Map（0xBC）：stream_type → 编码名、基本流映射
///   * PES（0xBD private_stream_1 / 0xC0-0xDF audio / 0xE0-0xEF video）与首末 PTS
///   * padding（0xBE）与 Program End Code（0xB9）
///
/// 解析时顺手把 model::MpegPsDiagnostics（Pack/PSM/PES 计数、End Code、
/// 时间戳单调性与 PSM 对账结果）填进 result.diagnostics.mpeg_ps，
/// QC 的 container.ps.* 只读这份摘要。
class MpegPsStructureAnalyzer {
public:
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr,
                 const MpegPsStructureOptions& options = MpegPsStructureOptions{});
};

} // namespace videoeye
