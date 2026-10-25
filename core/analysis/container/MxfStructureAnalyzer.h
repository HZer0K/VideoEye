#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "core/domain/model/ContainerStructureInfo.h"

namespace videoeye {

// MXF（SMPTE 377M，KLV 容器）结构解析选项。
//
// MXF 的元数据全在 Partition Pack / Primer Pack / Index Table 里，画面与声音是
// 一个个 Essence Element KLV。结构页只需要把这些 KLV 认出来、按轨道归类；
// 不解引用（Primer）就无法还原描述符里的编码细节，所以本解析器不声称解析 codec。
struct MxfStructureOptions {
    bool build_tree = true;

    // 最多扫描多少字节（<= 0 表示扫到文件尾）。
    // 默认 64 MiB：一个 50 Mbps 的素材每秒约 6 MB，64 MiB 覆盖前十来秒的 KLV 结构；
    // 索引段通常在文件尾，扫不到时会在 metadata 里标"部分扫描"。
    int64_t max_scan_bytes = 64LL << 20;

    // 最多解析多少个 KLV 对象（防止损坏文件把每个字节都当成对象）。
    uint32_t max_objects = 200000;

    // 树里最多保留多少个子节点（Essence KLV 按帧出现，全塞进树没有意义）。
    uint32_t max_tree_elements = 300;
};

/// MXF 容器结构解析器
///
/// 覆盖：
///   * 16 字节 SMPTE Universal Label + BER 长度（短/长格式，最多 8 字节）
///   * Partition Pack（key[13] = 0x02 头 / 0x03 体 / 0x04 尾）：KAG、分区偏移、OP
///   * Primer Pack（key[5] = 0x05）与 Index Table Segment（key[5] = 0x53）
///   * Essence Element（key[4] = 0x01，bytes 8-11 = 0D 01 03 01）按 track 归类
///   * KLV 长度越界 / 键前缀失配时的重新同步计数
class MxfStructureAnalyzer {
public:
    bool Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                 const std::atomic<bool>* cancel = nullptr, const MxfStructureOptions& options = MxfStructureOptions{});
};

} // namespace videoeye
