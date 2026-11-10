#include "core/repair/RepairSampleScan.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "core/domain/io/Utf8Path.h"

namespace videoeye {
namespace repair {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

class DigestBuilder {
public:
    void Mix(uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash_ ^= (value >> (i * 8)) & 0xFF;
            hash_ *= kFnvPrime;
        }
    }

    void MixString(const std::string& value) {
        Mix(value.size());
        for (const unsigned char ch : value) {
            hash_ ^= ch;
            hash_ *= kFnvPrime;
        }
        Mix(0x9E3779B97F4A7C15ull); // 字段边界，防止相邻字符串拼接后摘要相同
    }

    void MixOptionalUint(const std::optional<uint64_t>& value) {
        Mix(value.has_value() ? 1u : 0u);
        Mix(value.value_or(0));
    }

    void MixOptionalDouble(const std::optional<double>& value) {
        Mix(value.has_value() ? 1u : 0u);
        uint64_t bits = 0;
        if (value.has_value())
            std::memcpy(&bits, &(*value), sizeof(bits));
        Mix(bits);
    }

    std::string Hex() const {
        std::ostringstream out;
        out << std::hex << hash_;
        return out.str();
    }

private:
    uint64_t hash_ = kFnvOffset;
};

} // namespace

// ---------------- MemoryByteSource ----------------

MemoryByteSource::MemoryByteSource(const std::vector<uint8_t>& bytes, const ScanWindow& window)
    : bytes_(bytes), window_(window) {
    if (window_.size > bytes_.size())
        error_ = "内存字节源小于扫描窗口";
}

uint64_t MemoryByteSource::Size() const {
    return window_.size;
}

uint64_t MemoryByteSource::AbsoluteOffset(uint64_t relative) const {
    return window_.file_offset + relative;
}

bool MemoryByteSource::Read(uint64_t relative, uint8_t* dst, std::size_t len) {
    if (!error_.empty())
        return false;
    if (relative > window_.size || len > window_.size - relative || len > bytes_.size() - relative) {
        error_ = "内存扫描窗口越界";
        return false;
    }
    if (len > 0)
        std::memcpy(dst, bytes_.data() + static_cast<std::size_t>(relative), len);
    bytes_fetched_ += len;
    return true;
}

const std::string& MemoryByteSource::Error() const {
    return error_;
}

uint64_t MemoryByteSource::BytesFetched() const {
    return bytes_fetched_;
}

uint64_t MemoryByteSource::ChunksLoaded() const {
    return 1;
}

// ---------------- ChunkedByteSource ----------------

ChunkedByteSource::ChunkedByteSource(const std::string& path, const ScanWindow& window, std::size_t chunk_bytes,
                                     const RepairRunBudget* budget, const std::atomic<bool>* cancel,
                                     std::size_t max_buffer_bytes)
    : in_(ToNativePath(path), std::ios::binary), window_(window),
      chunk_bytes_(chunk_bytes == 0 ? kDefaultChunkBytes : chunk_bytes), max_buffer_bytes_(max_buffer_bytes),
      budget_(budget), cancel_(cancel) {
    if (!in_) {
        error_ = "无法打开源文件做分块扫描";
        return;
    }
    if (max_buffer_bytes_ == 0)
        max_buffer_bytes_ = kDefaultMaxBufferBytes;
    chunk_bytes_ = std::min(chunk_bytes_, max_buffer_bytes_);
}

uint64_t ChunkedByteSource::Size() const {
    return window_.size;
}

uint64_t ChunkedByteSource::AbsoluteOffset(uint64_t relative) const {
    return window_.file_offset + relative;
}

bool ChunkedByteSource::Fill(uint64_t absolute) {
    // 逐块检查点：不能用「每 N 字节检查一次」那种按商判据，样本/块边界会跳过它。
    if (budget_ != nullptr) {
        if (budget_->Checkpoint()) {
            error_ = BudgetStopMessage(budget_, "样本扫描");
            return false;
        }
    } else if (cancel_ != nullptr && cancel_->load()) {
        error_ = "样本扫描被取消";
        return false;
    }

    const uint64_t window_end = window_.file_offset + window_.size;
    if (absolute >= window_end) {
        error_ = "读取位置越过扫描窗口末尾";
        return false;
    }
    const uint64_t remaining = window_end - absolute;
    const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(chunk_bytes_, remaining));
    if (want > max_buffer_bytes_) {
        error_ = "单次块读取超过有界缓存上限（" + std::to_string(want) + " 字节）";
        return false;
    }

    buffer_.resize(want);
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(absolute), std::ios::beg);
    if (!in_) {
        error_ = "分块扫描定位失败";
        return false;
    }
    in_.read(reinterpret_cast<char*>(buffer_.data()), static_cast<std::streamsize>(want));
    if (in_.gcount() != static_cast<std::streamsize>(want)) {
        error_ = "分块扫描读取不完整（offset=" + std::to_string(absolute) + "）";
        return false;
    }
    buffer_start_ = absolute;
    buffer_len_ = want;
    bytes_fetched_ += want;
    ++chunks_loaded_;
    return true;
}

bool ChunkedByteSource::Read(uint64_t relative, uint8_t* dst, std::size_t len) {
    if (!error_.empty())
        return false;
    if (len == 0)
        return true;
    if (relative > window_.size || len > window_.size - relative) {
        error_ = "扫描窗口越界";
        return false;
    }
    if (len > max_buffer_bytes_) {
        error_ = "单次读取超过有界缓存上限";
        return false;
    }

    std::size_t done = 0;
    while (done < len) {
        const uint64_t absolute = window_.file_offset + relative + done;
        if (buffer_len_ == 0 || absolute < buffer_start_ || absolute >= buffer_start_ + buffer_len_) {
            if (!Fill(absolute))
                return false;
        }
        const uint64_t available = (buffer_start_ + buffer_len_) - absolute;
        const std::size_t take =
            static_cast<std::size_t>(std::min<uint64_t>(available, static_cast<uint64_t>(len - done)));
        std::memcpy(dst + done, buffer_.data() + static_cast<std::size_t>(absolute - buffer_start_), take);
        done += take;
    }
    return true;
}

const std::string& ChunkedByteSource::Error() const {
    return error_;
}

uint64_t ChunkedByteSource::BytesFetched() const {
    return bytes_fetched_;
}

uint64_t ChunkedByteSource::ChunksLoaded() const {
    return chunks_loaded_;
}

// ---------------- RepairSampleLedger ----------------

RepairSampleLedger::RepairSampleLedger(Limits limits) : limits_(limits) {
}

void RepairSampleLedger::Add(const model::RecoveredSample& sample) {
    ++total_;
    total_bytes_ += sample.size;
    if (sample.stream_index >= 0) {
        ++by_track_[sample.stream_index];
        ++by_track_count_;
    }
    if (sample.segment_index.has_value()) {
        ++by_segment_[*sample.segment_index];
        ++by_segment_count_;
    }
    if (sample.confidence == model::SampleConfidence::Verified)
        ++verified_samples_;
    if (sample.confidence == model::SampleConfidence::Inferred ||
        sample.timestamp_evidence == model::SampleTimestampEvidence::Inferred)
        ++inferred_samples_;
    if (sample.decode_status == model::SampleDecodeStatus::Ok)
        ++decoded_ok_samples_;
    if (!sample.damage_flags.empty())
        ++damaged_samples_;

    const bool count_ok = samples_.size() < limits_.max_detail_samples;
    const uint64_t already_cached = std::min(cached_bytes_, limits_.max_detail_bytes);
    const bool bytes_ok = sample.size <= limits_.max_detail_bytes - already_cached;
    if (count_ok && bytes_ok) {
        samples_.push_back(sample);
        cached_bytes_ += sample.size;
        return;
    }

    ++dropped_detail_;
    if (!detail_truncated_) {
        detail_truncated_ = true;
        limitations_.push_back("样本明细缓存达到条数/字节上限，仅保留前 " + std::to_string(samples_.size()) +
                               " 条明细；逐轨/逐区间聚合计数仍为全量实测值");
    }
}

uint64_t RepairSampleLedger::CountForTrack(int stream_index) const {
    const auto it = by_track_.find(stream_index);
    return it == by_track_.end() ? 0 : it->second;
}

uint64_t RepairSampleLedger::CountForSegment(uint32_t segment_index) const {
    const auto it = by_segment_.find(segment_index);
    return it == by_segment_.end() ? 0 : it->second;
}

model::RepairSampleLedgerSummary RepairSampleLedger::Summary() const {
    model::RepairSampleLedgerSummary summary;
    summary.available = true;
    summary.total_samples = total_;
    summary.total_bytes = total_bytes_;
    summary.retained_detail = samples_.size();
    summary.dropped_detail = dropped_detail_;
    summary.detail_truncated = detail_truncated_;
    summary.by_track_count = by_track_count_;
    summary.by_segment_count = by_segment_count_;
    summary.verified_samples = verified_samples_;
    summary.inferred_samples = inferred_samples_;
    summary.decoded_ok_samples = decoded_ok_samples_;
    summary.damaged_samples = damaged_samples_;
    summary.limitations = limitations_;
    return summary;
}

std::string SampleSequenceDigest(const std::vector<model::RecoveredSample>& samples) {
    DigestBuilder digest;
    digest.Mix(samples.size());
    for (const auto& sample : samples) {
        digest.MixOptionalUint(sample.source_offset);
        digest.Mix(sample.size);
        digest.Mix(sample.track_id);
        digest.Mix(static_cast<uint64_t>(static_cast<int64_t>(sample.stream_index)));
        digest.MixString(sample.config_id);
        digest.MixOptionalUint(sample.decode_ordinal);
        digest.MixOptionalUint(sample.display_ordinal);
        digest.Mix(sample.is_rap ? 1u : 0u);
        digest.MixString(sample.rap_evidence);
        digest.MixOptionalDouble(sample.dts_seconds);
        digest.MixOptionalDouble(sample.pts_seconds);
        digest.MixOptionalDouble(sample.duration_seconds);
        digest.MixString(model::StableCode(sample.timestamp_evidence));
        digest.MixString(sample.inference_basis);
        digest.MixString(model::StableCode(sample.confidence));
        digest.MixString(model::StableCode(sample.decode_status));
        digest.MixString(sample.decode_note);
        for (const auto& flag : sample.damage_flags)
            digest.MixString(flag);
        if (sample.segment_index.has_value())
            digest.MixOptionalUint(static_cast<uint64_t>(*sample.segment_index));
        else
            digest.MixOptionalUint(std::nullopt);
        digest.MixOptionalUint(sample.normalized_payload_hash);
    }
    return digest.Hex();
}

RepairSampleUpperLayerView CheckUpperLayerConservation(const RepairSampleLedger& ledger,
                                                       const std::vector<model::RepairRecoveredSegment>& segments,
                                                       const std::vector<model::RepairTrackConservation>& tracks,
                                                       uint64_t reported_total) {
    RepairSampleUpperLayerView view;
    view.expected_total = ledger.Total();
    if (reported_total != view.expected_total) {
        view.problems.push_back("执行层写入包数 " + std::to_string(reported_total) + " 与样本层总数 " +
                                std::to_string(view.expected_total) + " 不一致");
    }

    for (const auto& segment : segments) {
        const uint64_t counted = ledger.CountForSegment(segment.index);
        view.expected_segment_total += counted;
        if (segment.packets_written != counted) {
            view.problems.push_back("恢复区间 " + std::to_string(segment.index) + " 写入 " +
                                    std::to_string(segment.packets_written) + " 个包，样本层只记到 " +
                                    std::to_string(counted) + " 个");
        }
    }
    if (!segments.empty() && ledger.Summary().by_segment_count == 0) {
        view.problems.push_back("存在恢复区间，但样本层没有任何带区间归属的样本（不得谎报有恢复内容）");
    }

    for (const auto& track : tracks) {
        const uint64_t counted = track.output_stream_index >= 0 ? ledger.CountForTrack(track.output_stream_index) : 0;
        view.expected_track_total += counted;
        if (track.packets_written != counted) {
            view.problems.push_back("轨道 " + std::to_string(track.track_id) + " 写入 " +
                                    std::to_string(track.packets_written) + " 个包，样本层只记到 " +
                                    std::to_string(counted) + " 个");
        }
    }
    view.consistent = view.problems.empty();
    return view;
}

} // namespace repair
} // namespace videoeye
