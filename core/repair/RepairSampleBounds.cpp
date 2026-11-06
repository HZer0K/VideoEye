#include "core/repair/RepairSampleBounds.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace videoeye {
namespace repair {
namespace {

// stts 是"连续 count 个样本共享一个 delta"的行程编码。只保留当前 entry 与
// 已消费数，就能在 O(1) 空间里给出任意样本的 DTS，不用展开成数组。
struct SttsCursor {
    const std::vector<SttsEntry>* entries = nullptr;
    std::size_t entry = 0;
    uint32_t used = 0;
    uint64_t time = 0;
    bool valid = false;
    bool exhausted = false;

    void SkipEmpty() {
        while (entries && entry < entries->size() && (*entries)[entry].sample_count == 0)
            ++entry;
        if (!entries || entry >= entries->size())
            exhausted = true;
    }

    void Init(const std::vector<SttsEntry>& table, bool table_present) {
        entries = &table;
        valid = table_present && !table.empty();
        exhausted = !valid;
        if (valid)
            SkipEmpty();
    }

    // 处理完第 i 个样本后推进到第 i+1 个样本的 DTS。
    void Advance() {
        if (!valid || exhausted)
            return;
        const SttsEntry& current = (*entries)[entry];
        time += current.sample_delta;
        ++used;
        if (used >= current.sample_count) {
            ++entry;
            used = 0;
            SkipEmpty();
        }
    }

    // 当前样本的 DTS 是否可信（stts 表耗尽后的时间只是外推值）。
    bool TimingKnown() const {
        return valid && !exhausted;
    }
};

// stsc 按 first_chunk 单调递增：用游标而不是每个 chunk 从表头重扫。
struct StscCursor {
    const std::vector<StscEntry>* entries = nullptr;
    std::size_t index = 0;

    uint32_t SamplesPerChunk(uint32_t chunk_index_1based) {
        if (!entries || entries->empty())
            return 0;
        while (index + 1 < entries->size() && (*entries)[index + 1].first_chunk <= chunk_index_1based)
            ++index;
        if ((*entries)[index].first_chunk > chunk_index_1based)
            return 0;
        return (*entries)[index].samples_per_chunk;
    }
};

uint64_t DeclaredSttsSamples(const std::vector<SttsEntry>& table) {
    uint64_t total = 0;
    for (const auto& entry : table) {
        if (total > std::numeric_limits<uint64_t>::max() - entry.sample_count)
            return std::numeric_limits<uint64_t>::max();
        total += entry.sample_count;
    }
    return total;
}

} // namespace

RepairTrackBounds AnalyzeTrackBounds(const IsobmffTrack& track, uint64_t file_size, const RepairRunBudget* budget,
                                     uint64_t max_samples) {
    RepairTrackBounds bounds;
    if (!track.has_stsz && !track.has_stz2)
        return bounds;
    if (!track.has_stco && !track.has_co64)
        return bounds;
    if (track.chunk_offsets.empty())
        return bounds;

    const uint64_t declared = track.stsz.sample_count;
    if (declared == 0)
        return bounds;
    bounds.declared_sample_count = declared;

    // ---- 声明数与"实际存在的证据"交叉校验 ----
    // default_size != 0 时不需要提供逐样本大小表，但文件字节数仍然是天然上界；
    // default_size == 0 时逐样本大小表的长度就是上界。
    uint64_t plausible_ceiling = declared;
    std::ostringstream why;
    if (track.stsz.default_size != 0) {
        const uint64_t min_sample = track.stsz.default_size;
        const uint64_t by_file = file_size / min_sample + 1;
        if (declared > by_file) {
            plausible_ceiling = by_file;
            bounds.declaration_implausible = true;
            why << "stsz 声明 " << declared << " 个样本、固定大小 " << min_sample << " 字节，但文件只有 " << file_size
                << " 字节，最多容纳 " << by_file << " 个；";
        }
    } else if (declared > track.stsz.sizes.size()) {
        plausible_ceiling = track.stsz.sizes.size();
        bounds.declaration_implausible = true;
        why << "stsz 声明 " << declared << " 个样本，但逐样本大小表只有 " << track.stsz.sizes.size() << " 项；";
    }
    if (track.tables_truncated) {
        bounds.declaration_implausible = true;
        why << "样本表解析被表项上限截断；";
    }
    if (track.has_stts && !track.stts.empty()) {
        const uint64_t stts_total = DeclaredSttsSamples(track.stts);
        if (stts_total < declared) {
            bounds.declaration_implausible = true;
            why << "stts 只覆盖 " << stts_total << " 个样本，少于 stsz 声明的 " << declared << " 个；";
        }
    } else {
        bounds.timing_unknown = true;
        why << "缺少有效 stts，无法给出可靠的时间位置；";
    }

    // 声明数本来就在可安全检查预算内时，逐个查完更有价值：越界样本的个数和
    // 首个坏样本的位置本身就是损伤证据。按文件容量反推的上界只用来约束巨型
    // 声明（UINT32_MAX 之类）的工作量，否则会把一个 4 样本的小样本表截成 3 个，
    // bad_sample_count 少报一个 —— 资源受控不该以损失结论精度为代价。
    uint64_t total = std::min(declared, max_samples);
    if (declared > max_samples) {
        total = std::min(plausible_ceiling, max_samples);
        bounds.truncated_by_budget = true;
        why << "声明样本数 " << declared << " 超过单次预算 " << max_samples << "，按文件容量上界只检查 " << total
            << " 个；";
    }
    if (plausible_ceiling > max_samples) {
        bounds.truncated_by_budget = true;
        why << "样本预算 " << max_samples << " 个，超出部分未检查；";
    }
    if (total == 0) {
        bounds.checked = true;
        bounds.note = why.str() + "没有可安全检查的样本";
        return bounds;
    }

    bounds.checked = true;
    bounds.sample_count = total;
    bounds.note = why.str();

    SttsCursor cursor;
    cursor.Init(track.stts, track.has_stts);
    if (!cursor.valid)
        bounds.timing_unknown = true;
    StscCursor stsc;
    stsc.entries = &track.stsc;

    uint64_t sample_index = 0;
    const uint64_t chunk_limit = std::min<uint64_t>(track.chunk_offsets.size(), kDefaultMaxChunkChecks);
    if (track.chunk_offsets.size() > kDefaultMaxChunkChecks) {
        bounds.truncated_by_budget = true;
        bounds.note += "chunk 数超过预算，尾部未检查；";
    }

    for (uint64_t chunk = 0; chunk < chunk_limit && sample_index < total; ++chunk) {
        // 每 4096 个样本查一次取消 / 共享截止时间，兼顾开销与响应速度。
        if (budget != nullptr && (sample_index & 0xFFFULL) == 0 && budget->Checkpoint()) {
            bounds.truncated_by_budget = true;
            bounds.note += "诊断阶段被取消或超出共享时间预算；";
            break;
        }
        const uint64_t chunk_offset = track.chunk_offsets[static_cast<std::size_t>(chunk)];
        const uint32_t spc = stsc.SamplesPerChunk(static_cast<uint32_t>(chunk + 1));
        if (spc == 0)
            continue;

        uint64_t offset_in_chunk = 0;
        for (uint32_t j = 0; j < spc && sample_index < total; ++j, ++sample_index) {
            const uint32_t sample_size =
                track.stsz.default_size != 0
                    ? track.stsz.default_size
                    : (sample_index < track.stsz.sizes.size() ? track.stsz.sizes[static_cast<std::size_t>(sample_index)]
                                                              : 0);
            if (offset_in_chunk > std::numeric_limits<uint64_t>::max() - sample_size ||
                chunk_offset > std::numeric_limits<uint64_t>::max() - offset_in_chunk) {
                bounds.overflowed = true;
                bounds.note += "样本偏移溢出 uint64；";
                sample_index = total;
                break;
            }
            const uint64_t sample_offset = chunk_offset + offset_in_chunk;
            const bool bad = sample_offset > file_size ||
                             (sample_size > 0 && sample_size > file_size - std::min(sample_offset, file_size));

            if (bad) {
                if (bounds.bad_sample_count == 0) {
                    bounds.first_bad_sample = sample_index;
                    bounds.first_bad_offset = sample_offset;
                    if (sample_index == 0) {
                        bounds.salvage_seconds = 0.0;
                    } else if (track.media_timescale > 0 && cursor.TimingKnown()) {
                        bounds.salvage_seconds =
                            static_cast<double>(cursor.time) / static_cast<double>(track.media_timescale);
                    } else {
                        bounds.timing_unknown = true;
                    }
                }
                ++bounds.bad_sample_count;
            } else if (bounds.bad_sample_count > 0) {
                // 越界之后重新出现文件范围内的样本 → 中间损坏而不是尾部截断。
                bounds.has_valid_after_bad = true;
            }

            offset_in_chunk += sample_size;
            cursor.Advance();
            ++bounds.samples_examined;
        }
    }
    if (budget != nullptr && budget->Checkpoint() && !bounds.truncated_by_budget) {
        bounds.truncated_by_budget = true;
        bounds.note += "诊断结束时已取消或超出共享时间预算；";
    }
    if (bounds.samples_examined < total)
        bounds.truncated_by_budget = true;
    return bounds;
}

} // namespace repair
} // namespace videoeye
