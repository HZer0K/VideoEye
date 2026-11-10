#pragma once

// 统一的「已恢复样本」记录：四层记账里最底层的事实源。
//
// 上层（轨级 RepairTrackConservation / 区间级 RepairRecoveredSegment / 全局级
// RepairCoverage）的样本数必须由这一层聚合而来，不允许两处各自统计后再靠断言对齐。
//
// 这一层是 domain 叶子：只放值类型、枚举、string，不许出现 AVPacket /
// AVCodecParameters / FFmpeg 头，也不许在这里做 IO。证据来源与可信度分开落，
// 推定值永远不得冒充实测值。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 时间戳从哪来。inferred 必须同时给出 inference_basis，否则是不合格的记录。
enum class SampleTimestampEvidence {
    SourceTable,  // moov 的 stts / ctts / stsz，或 trun 原值
    ContainerBox, // tfdt / sidx / elst 等容器级时间盒
    Bitstream,    // VUI timing_info / ADTS 采样率等码流内证据
    Reference,    // 同源参考样片提供的参数（只借配置，不借时基）
    Inferred,     // 推定：按帧时长 / 采样数推导
    Unknown,
};

// 可信度只用于决定是否采信，不是「必须成功 / 必须失败」的硬真值。
enum class SampleConfidence {
    Verified, // 有独立证据交叉核对
    Probable, // 单一证据自洽，未交叉核对
    Inferred, // 推定值，不计入「已验证」的恢复率分子
    Unknown,
};

// 守恒契约：报 Ok 必须有实际解码结果；Failed / NotDecodable 必须有原因。
enum class SampleDecodeStatus {
    NotAttempted,
    Ok,
    Failed,
    NotDecodable,
};

inline const char* StableCode(SampleTimestampEvidence value) {
    switch (value) {
    case SampleTimestampEvidence::SourceTable:
        return "source_table";
    case SampleTimestampEvidence::ContainerBox:
        return "container_box";
    case SampleTimestampEvidence::Bitstream:
        return "bitstream";
    case SampleTimestampEvidence::Reference:
        return "reference";
    case SampleTimestampEvidence::Inferred:
        return "inferred";
    case SampleTimestampEvidence::Unknown:
        break;
    }
    return "unknown";
}

inline const char* StableCode(SampleConfidence value) {
    switch (value) {
    case SampleConfidence::Verified:
        return "verified";
    case SampleConfidence::Probable:
        return "probable";
    case SampleConfidence::Inferred:
        return "inferred";
    case SampleConfidence::Unknown:
        break;
    }
    return "unknown";
}

inline const char* StableCode(SampleDecodeStatus value) {
    switch (value) {
    case SampleDecodeStatus::NotAttempted:
        return "not_attempted";
    case SampleDecodeStatus::Ok:
        return "ok";
    case SampleDecodeStatus::Failed:
        return "failed";
    case SampleDecodeStatus::NotDecodable:
        return "not_decodable";
    }
    return "not_attempted";
}

// 编码配置本体：样本只持 config_id，不复制 avcC / hvcC / esds。
// 参数集变化（SPS/PPS 换版）生成新的 config_id 与新的有效区间。
struct RepairCodecConfig {
    std::string config_id;
    std::string codec;                 // "h264" / "hevc" / "aac" ...
    std::string extradata_digest;      // 参数集内容摘要，用于同源核验
    std::optional<int64_t> valid_from; // 以解码序号表示的有效区间起点
    std::optional<int64_t> valid_to;   // 空 = 到当前扫描终点仍有效
    std::string evidence;              // 配置来源（in-band / 参考样片 / stsd）
};

struct RecoveredSample {
    // 源字节位置。拿不到（例如重编码产物）时留空，绝不填 0 冒充精确位置。
    std::optional<uint64_t> source_offset;
    uint64_t size = 0;
    uint32_t track_id = 0;
    int stream_index = -1;
    std::string config_id;

    // 无显示顺序证据时留空，不填 0 冒充「第一帧」。
    std::optional<uint64_t> decode_ordinal;
    std::optional<uint64_t> display_ordinal;
    bool is_rap = false;
    std::string rap_evidence; // "stss" / "idr_nal" / "trun_sample_flags" ...

    std::optional<double> dts_seconds;
    std::optional<double> pts_seconds;
    std::optional<double> duration_seconds;
    SampleTimestampEvidence timestamp_evidence = SampleTimestampEvidence::Unknown;
    std::string inference_basis; // timestamp_evidence == Inferred 时必填

    SampleConfidence confidence = SampleConfidence::Unknown;
    SampleDecodeStatus decode_status = SampleDecodeStatus::NotAttempted;
    std::string decode_note;               // Failed / NotDecodable 的原因
    std::vector<std::string> damage_flags; // 稳定机读码，如 "truncated_au"

    // 指向 RepairRecoveredSegment.index，供「哪些时间区间」反查。
    std::optional<uint32_t> segment_index;
    // 规范化访问单元载荷摘要（AAC 剥 ADTS 头后），用于逐样本守恒比对。
    std::optional<uint64_t> normalized_payload_hash;
};

// 样本账本对外的只读摘要。逐样本明细可以因为有界缓存而不完整，但聚合计数
// 永远是全量实测值；detail_truncated 必须一起进报告，不能静默抽样。
struct RepairSampleLedgerSummary {
    bool available = false;
    uint64_t total_samples = 0;
    uint64_t total_bytes = 0;
    uint64_t retained_detail = 0;
    uint64_t dropped_detail = 0;
    bool detail_truncated = false;
    uint64_t by_track_count = 0; // 有归属轨的样本数
    uint64_t by_segment_count = 0;
    uint64_t verified_samples = 0;
    uint64_t inferred_samples = 0;
    uint64_t decoded_ok_samples = 0;
    uint64_t damaged_samples = 0;
    std::vector<std::string> limitations;
};

} // namespace model
} // namespace videoeye
