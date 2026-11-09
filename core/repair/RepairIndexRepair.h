#pragma once

// moov 样本表索引修复（stco/co64 偏移、stts/ctts/stss 计数）。
//
// 只处理"moov 还在、但索引不自洽"的普通（非分片）MP4：
//   * stco/co64：当 chunk 偏移整体越界时，按最小偏移与 mdat 载荷起点的差
//     做整体位移；保留 chunk 的相对顺序，不重排交错布局；
//   * stts：条目计数与样本数不一致时，按 mdhd 平均帧时长重建为单条目；
//   * ctts：计数不一致时移除（并写明 B 帧显示顺序风险）；
//   * stss：越界同步样本标记会被剔除。
//
// 任何一条无法证明安全（多 mdat、分片、表被截断、偏移无法整体解释）都会
// 直接拒绝，不写一个"能打开但内容错位"的文件。

#include <atomic>
#include <cstdint>
#include <string>

namespace videoeye {
namespace repair {

class RepairRunBudget;

struct RepairIndexRepairResult {
    bool attempted = false;
    bool ok = false;
    std::uint64_t rebuilt_chunks = 0;
    std::int64_t applied_shift_bytes = 0;
    std::string note;
    std::string error;
};

class RepairIndexRepair {
public:
    // 需要修复时返回 attempted=true；成功写出 output_path 时 ok=true。
    // output_path 由调用方独占创建，函数内部只做 trunc 覆盖。
    // budget 为空时退回只看取消标志；非空时重建循环也遵守共享截止时间。
    static RepairIndexRepairResult Rebuild(const std::string& source_path, const std::string& output_path,
                                           const std::atomic<bool>* cancel, const RepairRunBudget* budget = nullptr);
};

} // namespace repair
} // namespace videoeye
