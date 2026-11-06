#pragma once

// 样本表边界交叉校验（R02）。
//
// 旧实现按 stsz.sample_count 直接构造 vector<uint64_t> dts(total)。固定样本大小的
// stsz 可以声明 UINT32_MAX 个样本却不需要提供任何样本表项，于是请求约 32 GiB，
// 随后还要逐样本、逐 chunk 重扫 stsc —— 恶意输入能把进程拖死。
//
// 现在：
//   * DTS 用 stts 游标按需推进，内存 O(stts 表项) 而不是 O(声明样本数)；
//   * 声明数与"实际表项 / 文件字节容量"交叉校验，明显不可能的声明直接截断并记账；
//   * 样本 / chunk 数量设硬预算，循环里检查取消与共享截止时间。

#include <cstdint>
#include <optional>
#include <string>

#include "core/media/container/IsobmffParser.h"
#include "core/repair/RepairBudget.h"

namespace videoeye {
namespace repair {

// 单次诊断允许检查的样本上限；超过记为 truncated_by_budget，绝不分配大数组。
constexpr uint64_t kDefaultMaxTrackSamples = 2'000'000;
constexpr uint64_t kDefaultMaxChunkChecks = 500'000;

struct RepairTrackBounds {
    bool checked = false;
    uint64_t sample_count = 0;          // 参与检查的（可能被截断的）声明样本数
    uint64_t declared_sample_count = 0; // 容器原始声明值
    uint64_t samples_examined = 0;      // 实际逐个校验过的样本数
    uint64_t first_bad_sample = 0;
    uint64_t bad_sample_count = 0;
    uint64_t first_bad_offset = 0;
    std::optional<double> salvage_seconds;
    bool overflowed = false;
    bool truncated_by_budget = false;
    bool declaration_implausible = false;
    bool timing_unknown = false;
    std::string note;
};

RepairTrackBounds AnalyzeTrackBounds(const IsobmffTrack& track, uint64_t file_size,
                                     const RepairRunBudget* budget = nullptr,
                                     uint64_t max_samples = kDefaultMaxTrackSamples);

} // namespace repair
} // namespace videoeye
