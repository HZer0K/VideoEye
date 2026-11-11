#pragma once

// 码流级重同步（切片 D 层 2）：在字节窗口内直接找随机访问点。
//
// 存在理由：`TryResync` 完全依赖 demuxer 索引。索引损坏时 seek 直接失效，
// 于是「文件里明明还有完整 GOP」也会被报成「未找到恢复点」。这一层不看容器，
// 只在载荷窗口里按 4 字节 NAL 长度前缀走链，收集含 IDR / CRA 的候选 AU。
//
// 三条硬约束（都是路线图点名禁止的偷懒）：
//   * 不把「扫到参数集」当成「配置有效」—— 参数集只作为候选 AU 的附属证据记录；
//   * 不把候选当成「可解码」—— 候选只有再经试解码才能升级为 TrialDecodeOk；
//   * 不整段读入 —— 逐块扫描，块内 O(1) 剪枝，链验证按需小读，逐块检查预算。
//
// 这一层给不出时间戳：没有容器索引就没有时基证据，`byte_offset` 是唯一硬事实。
// 上层若要把字节位置换算成秒，必须另找证据（样本表 / 参考样片），不能猜。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/repair/RepairBudget.h"

namespace videoeye {
namespace repair {

struct RepairBitstreamRap {
    uint64_t byte_offset = 0;        // AU 起点在文件内的绝对偏移
    uint64_t au_bytes = 0;           // 已走通的 AU 字节数（链长度下界）
    bool has_parameter_sets = false; // 该 AU 内（VCL 之前）出现 SPS/PPS/VPS
    std::string codec;               // "h264" / "hevc"
    std::string evidence;            // 人读依据：走了几个 NAL、命中什么
};

struct RepairBitstreamResyncOptions {
    // 单次扫描最多看多少字节：索引坏掉的文件可能上百 GiB，不设上限会变成全盘扫描。
    uint64_t max_scan_bytes = 32ull * 1024ull * 1024ull;
    std::size_t chunk_bytes = 64u * 1024u;
    // 单次最多收集多少个候选，够了就停（上层只需要一个可用起点）。
    int max_candidates = 8;
    // 链最多走多少个 NAL（一个 AU 通常 < 8 个 NAL；给 16 足够覆盖 SEI + 参数集）。
    int max_nals_per_chain = 16;
    // true = 只收带参数集的候选（更保守，适合「参数集只在 IDR 前出现」的流）。
    bool require_parameter_sets = false;
};

struct RepairBitstreamResyncResult {
    bool attempted = false;
    bool found = false;
    // 未找到 / 未执行的原因（结构化描述，直接进报告，不许留空）。
    std::string error;
    std::string codec;
    std::vector<RepairBitstreamRap> candidates;
    uint64_t bytes_scanned = 0;
    uint64_t chunks_loaded = 0;
    bool truncated_by_budget = false; // 扫描窗口被 max_scan_bytes 截断
    std::string limitations;
};

// codec 只接受 "h264" / "hevc"（大小写不敏感）；其它编码返回 attempted=false，
// 不猜语法。窗口是半开区间 [window_start, window_end)，越界会被夹到文件范围内。
RepairBitstreamResyncResult ScanForRandomAccessPoints(const std::string& path, uint64_t window_start,
                                                      uint64_t window_end, const std::string& codec,
                                                      const RepairBitstreamResyncOptions& options,
                                                      const std::atomic<bool>* cancel, const RepairRunBudget* budget);

// 顶层盒扫描：找第一个非空 mdat 的载荷区间。
// 与「必须恰好一个 mdat」的重建器不同，这里只为限定扫描窗口，多 mdat 时取第一个
// 并置 multiple_mdats=true —— 扫描窗口不完整必须如实告知，不能假装扫了全文件。
struct RepairMdatWindow {
    bool found = false;
    bool multiple_mdats = false;
    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
    std::string error;
};

RepairMdatWindow FindPrimaryMdatPayload(const std::string& path);

} // namespace repair
} // namespace videoeye
