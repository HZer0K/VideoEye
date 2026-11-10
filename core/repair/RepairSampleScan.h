#pragma once

// 统一样本恢复层的执行侧框架：分块扫描 + 有界账本。
//
// 目标是把「容器解析 / 码流扫描 / 参考样片分析」产出的样本事实收敛到同一个
// RecoveredSample 序列，再由输出层按 segment_index / track 聚合到上三层。
//
// 两条硬约束：
//   * 内存有界：单次只允许一个块常驻，禁止把整个 mdat 读进内存（旧的 1 GiB
//     上限是明确的债务）；逐块调用 RepairRunBudget::Checkpoint()。
//   * 统计不丢：明细缓存到双上限后是「抽样」，聚合计数仍然全量精确，且
//     detail_truncated 必须随结论一起进报告，不许静默丢样本。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/model/RecoveredSample.h"
#include "core/domain/model/RepairTypes.h"
#include "core/repair/RepairBudget.h"

namespace videoeye {
namespace repair {

// 一个待扫描的载荷窗口。offset 是绝对文件偏移；轨上下文跟着窗口走，
// 避免 producer 各自维护一套 track_id / stream_index 映射。
struct ScanWindow {
    uint64_t file_offset = 0;
    uint64_t size = 0;
    uint32_t track_id = 0;
    int stream_index = -1;
    std::optional<uint32_t> segment_index;
    std::string config_id;
};

// producer 只通过这一个接口取字节。Read 允许跨块，返回 false 表示窗口越界、
// 读取失败或预算命中；具体原因看 Error()。
class SampleByteSource {
public:
    virtual ~SampleByteSource() = default;
    virtual uint64_t Size() const = 0;
    virtual uint64_t AbsoluteOffset(uint64_t relative) const = 0;
    virtual bool Read(uint64_t relative, uint8_t* dst, std::size_t len) = 0;
    virtual const std::string& Error() const = 0;
    // 供验收断言：确实是从文件分块取的，不是偷偷整段读入。
    virtual uint64_t BytesFetched() const = 0;
    virtual uint64_t ChunksLoaded() const = 0;
};

// 整块对照源：只在测试 / 小样本里用。生产链一律用 ChunkedByteSource。
class MemoryByteSource final : public SampleByteSource {
public:
    explicit MemoryByteSource(const std::vector<uint8_t>& bytes, const ScanWindow& window);

    uint64_t Size() const override;
    uint64_t AbsoluteOffset(uint64_t relative) const override;
    bool Read(uint64_t relative, uint8_t* dst, std::size_t len) override;
    const std::string& Error() const override;
    uint64_t BytesFetched() const override;
    uint64_t ChunksLoaded() const override;

private:
    const std::vector<uint8_t>& bytes_;
    ScanWindow window_;
    std::string error_;
    uint64_t bytes_fetched_ = 0;
};

// 文件上的有界分块源：每块读满 chunk_bytes（末尾按窗口截断），逐块检查预算。
// 单次 Read 超过 max_buffer_bytes 时直接拒绝，不让坏长度字段把内存撑爆。
class ChunkedByteSource final : public SampleByteSource {
public:
    static constexpr std::size_t kDefaultChunkBytes = 64u * 1024u;
    static constexpr std::size_t kDefaultMaxBufferBytes = 8u * 1024u * 1024u;

    ChunkedByteSource(const std::string& path, const ScanWindow& window, std::size_t chunk_bytes,
                      const RepairRunBudget* budget, const std::atomic<bool>* cancel,
                      std::size_t max_buffer_bytes = kDefaultMaxBufferBytes);

    uint64_t Size() const override;
    uint64_t AbsoluteOffset(uint64_t relative) const override;
    bool Read(uint64_t relative, uint8_t* dst, std::size_t len) override;
    const std::string& Error() const override;
    uint64_t BytesFetched() const override;
    uint64_t ChunksLoaded() const override;

private:
    // 每次只装一个块；即使调用方一次要 1 MiB，也按块循环拼接，绝不扩成整段。
    bool Fill(uint64_t absolute);

    std::ifstream in_;
    ScanWindow window_;
    std::size_t chunk_bytes_ = kDefaultChunkBytes;
    std::size_t max_buffer_bytes_ = kDefaultMaxBufferBytes;
    const RepairRunBudget* budget_ = nullptr;
    const std::atomic<bool>* cancel_ = nullptr;
    std::vector<uint8_t> buffer_;
    uint64_t buffer_start_ = 0;
    std::size_t buffer_len_ = 0;
    std::string error_;
    uint64_t bytes_fetched_ = 0;
    uint64_t chunks_loaded_ = 0;
};

// 样本账本：明细双上限（条数 + 源字节数），聚合计数始终精确。
class RepairSampleLedger {
public:
    struct Limits {
        std::size_t max_detail_samples = 4096;
        uint64_t max_detail_bytes = 64ull * 1024ull * 1024ull;
    };

    explicit RepairSampleLedger(Limits limits = {});

    void Add(const model::RecoveredSample& sample);

    uint64_t Total() const {
        return total_;
    }
    // 全量精确，不受明细缓存截断影响；写样本表 / 统计总字节数只能取这里。
    uint64_t TotalBytes() const {
        return total_bytes_;
    }
    uint64_t CountForTrack(int stream_index) const;
    uint64_t CountForSegment(uint32_t segment_index) const;
    const std::vector<model::RecoveredSample>& Samples() const {
        return samples_;
    }
    bool DetailTruncated() const {
        return detail_truncated_;
    }
    uint64_t DroppedDetail() const {
        return dropped_detail_;
    }
    const std::vector<std::string>& Limitations() const {
        return limitations_;
    }

    model::RepairSampleLedgerSummary Summary() const;

private:
    Limits limits_;
    std::vector<model::RecoveredSample> samples_;
    std::map<int, uint64_t> by_track_;
    std::map<uint32_t, uint64_t> by_segment_;
    uint64_t total_ = 0;
    uint64_t total_bytes_ = 0;
    uint64_t cached_bytes_ = 0;
    uint64_t dropped_detail_ = 0;
    uint64_t by_track_count_ = 0;
    uint64_t by_segment_count_ = 0;
    uint64_t verified_samples_ = 0;
    uint64_t inferred_samples_ = 0;
    uint64_t decoded_ok_samples_ = 0;
    uint64_t damaged_samples_ = 0;
    bool detail_truncated_ = false;
    std::vector<std::string> limitations_;
};

// 样本序列的规范化摘要：分块扫描与整块扫描必须逐字节一致，用这个串比较。
std::string SampleSequenceDigest(const std::vector<model::RecoveredSample>& samples);

// 上三层（区间 / 轨 / 执行总量）的实测值必须能由样本层逐项对上。
// 这不是「再统计一遍」：调用方要用 ledger 的数值填 packets_written，本函数只做
// 守恒核对，返回的不一致项要写进结论而不是静默修正。
struct RepairSampleUpperLayerView {
    uint64_t expected_total = 0;
    uint64_t expected_segment_total = 0;
    uint64_t expected_track_total = 0;
    bool consistent = false;
    std::vector<std::string> problems;
};

RepairSampleUpperLayerView CheckUpperLayerConservation(const RepairSampleLedger& ledger,
                                                       const std::vector<model::RepairRecoveredSegment>& segments,
                                                       const std::vector<model::RepairTrackConservation>& tracks,
                                                       uint64_t reported_total);

} // namespace repair
} // namespace videoeye
