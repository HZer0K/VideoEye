#pragma once

// MP4/fMP4 样本表（sample table）数据契约
//
// 设计约束（与 BitrateGopAnalyzer / AudioQcAnalyzer 一致）:
//   - 只用 std::string / std::vector / 定长整数，不依赖 Qt 与第三方容器库，
//     便于在 tests/unit 里喂合成数据做单测，也便于跨线程按值传递。
//   - 时间戳统一使用「媒体时基(media timescale)下的整数」，UI 自行除以 timescale 显示秒。
//   - 校验逻辑本身放在 analyzer 侧（Mp4SampleTableAnalyzer::Validate），
//     本文件只描述数据结构。

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/Mp4ConsistencyIssue.h"

namespace videoeye {
namespace model {

// 单个样本的异常标记（位掩码，UI 用红/黄着色）
namespace Mp4SampleFlags {
constexpr uint32_t kNone               = 0;
constexpr uint32_t kOffsetOutOfRange   = 1u << 0;  // offset + size 越过文件末尾
constexpr uint32_t kZeroSize           = 1u << 1;  // 样本大小为 0
constexpr uint32_t kNegativeCts        = 1u << 2;  // 合成时间为负（ctts v1 负偏移）
constexpr uint32_t kDtsNotMonotonic    = 1u << 3;  // 解码时间回退
constexpr uint32_t kZeroDuration       = 1u << 4;  // 时长为 0
constexpr uint32_t kCtsOutOfOrder      = 1u << 5;  // 同 chunk 内 CTS 早于上一帧过多
constexpr uint32_t kChunkDiscontinuity = 1u << 6;  // 同 chunk 内偏移不连续（stsz 与 stco/stsc 不符）
}  // namespace Mp4SampleFlags

// 单个样本
struct Mp4Sample {
    uint32_t index = 0;             // 0-based 样本序号
    uint32_t chunk_index = 0;       // 0-based chunk 序号
    uint32_t index_in_chunk = 0;    // chunk 内序号
    uint64_t offset = 0;            // 文件内偏移（stco/co64 + chunk 内累加）
    uint32_t size = 0;              // stsz
    int64_t  dts = 0;               // 解码时间（媒体时基）
    int64_t  cts = 0;               // 合成时间 = dts + ctts（可为负）
    int32_t  cts_delta = 0;         // ctts 偏移（v1 可为负）
    uint32_t duration = 0;          // stts
    bool     keyframe = false;      // stss / 无 stss 时默认全关键帧
    uint32_t description_index = 0; // stsd 索引
    uint32_t flags = 0;             // Mp4SampleFlags 位掩码

    bool HasFlag(uint32_t flag) const { return (flags & flag) != 0; }
    bool IsAnomalous() const { return flags != Mp4SampleFlags::kNone; }

    // 媒体时基 -> 秒（timescale <= 0 时返回 0）
    double DtsSeconds(uint32_t timescale) const {
        return timescale > 0 ? static_cast<double>(dts) / timescale : 0.0;
    }
    double CtsSeconds(uint32_t timescale) const {
        return timescale > 0 ? static_cast<double>(cts) / timescale : 0.0;
    }
    double DurationSeconds(uint32_t timescale) const {
        return timescale > 0 ? static_cast<double>(duration) / timescale : 0.0;
    }
};

// elst（Edit List）条目
struct Mp4EditListEntry {
    uint64_t segment_duration = 0;  // 电影时基
    int64_t  media_time = 0;        // 媒体时基；-1 表示空编辑（empty edit）
    uint16_t media_rate = 1;        // 16.16 定点，整数部分
    bool     is_empty_edit = false; // media_time == -1
};

// 一个 trak 的样本表全貌
struct Mp4TrackSampleTable {
    uint32_t track_id = 0;
    std::string type;              // "video" / "audio" / "hint" / "text" ...
    std::string codec;             // stsd 里的编码四字符码（如 "avc1"）
    uint32_t media_timescale = 0;  // mdhd.timescale
    uint64_t media_duration = 0;   // mdhd.duration（媒体时基）

    // ---- 各表的样本数（用于交叉一致性校验）----
    uint32_t stsz_sample_count = 0;   // stsz.sample_count（权威值）
    uint32_t stts_sample_count = 0;   // stts 能覆盖的样本数
    uint32_t ctts_sample_count = 0;   // ctts 能覆盖的样本数（无 ctts 时为 0）
    uint32_t stss_sync_count = 0;     // stss 条目数
    uint32_t chunk_count = 0;         // stco / co64 条目数
    uint32_t keyframe_count = 0;      // 展开后统计的关键帧数

    // ---- 各表是否存在 ----
    bool has_stts = false;
    bool has_ctts = false;
    bool has_stss = false;
    bool has_stsz = false;
    bool has_stz2 = false;
    bool has_stsc = false;
    bool has_stco = false;
    bool has_co64 = false;
    bool has_elst = false;

    uint64_t max_chunk_offset = 0;   // stco/co64 最大偏移
    uint64_t total_sample_bytes = 0; // 全部样本字节数

    // ---- elst ----
    std::vector<Mp4EditListEntry> edit_list;
    int64_t  first_sample_cts = 0;        // 首个样本的 CTS（媒体时基）
    double   composition_delay_ms = 0.0;  // elst 引入的首帧偏移（毫秒，>0 表示首帧被跳过）

    // ---- 展开后的样本（大文件按上限截断）----
    std::vector<Mp4Sample> samples;
    bool samples_truncated = false;
    bool sample_read_failed = false;   // GetSample 在中途失败（表不自洽的典型症状）
    uint32_t sample_read_error_index = 0;

    // 派生量
    uint32_t SampleCount() const { return stsz_sample_count; }
    double DurationSeconds() const {
        return media_timescale > 0 ? static_cast<double>(media_duration) / media_timescale : 0.0;
    }
    // 含 elst 补偿后的首个样本呈现时间（秒）；无样本时返回 0
    double FirstPresentationSeconds() const {
        if (samples.empty() || media_timescale == 0) return 0.0;
        // elst 的正 media_time 表示跳过媒体开头的若干时间，等价于把呈现时间前移
        return static_cast<double>(samples.front().cts) / media_timescale
               - composition_delay_ms / 1000.0;
    }
    const Mp4Sample* FindSample(uint32_t index) const {
        if (index >= samples.size()) return nullptr;
        return &samples[index];
    }
};

// 一个 moof/traf 的分片信息
struct Mp4FragmentInfo {
    uint32_t moof_index = 0;           // 文件内第几个 moof（0-based）
    uint32_t index = 0;                // traf 全局序号（0-based）
    uint32_t sequence_number = 0;      // mfhd.sequence_number
    uint32_t track_id = 0;             // tfhd.track_ID
    uint64_t offset = 0;               // moof 在文件内的偏移
    uint64_t size = 0;                 // moof 大小
    bool     has_tfdt = false;
    uint64_t base_media_decode_time = 0;  // tfdt
    uint32_t sample_count = 0;            // 本 traf 下所有 trun 的样本数之和
    uint64_t duration = 0;                // 本 traf 下所有 trun 的时长之和（媒体时基）
    uint64_t total_size = 0;              // 本 traf 下所有 trun 的字节数之和
    // tfhd 标志位
    bool base_data_offset_present = false;
    bool default_base_is_moof = false;
    bool sample_description_index_present = false;
    bool default_sample_duration_present = false;
    bool default_sample_size_present = false;
    bool duration_is_empty = false;
    uint64_t base_data_offset = 0;
    // trun
    uint32_t trun_count = 0;
    bool     trun_data_offset_present = false;
    int64_t  trun_data_offset = 0;
};

// MP4/fMP4 样本表分析结果
struct Mp4SampleTableResult {
    std::string file_path;
    bool valid = false;
    std::string error_message;

    uint64_t file_size = 0;
    bool fragmented = false;             // 含 moof
    bool moov_before_mdat = false;       // faststart
    uint64_t moov_offset = 0;
    uint64_t moov_size = 0;
    uint64_t first_mdat_offset = 0;
    uint64_t first_mdat_size = 0;
    std::vector<std::string> top_level_order;  // 顶层 box 出现的顺序（ftyp/moov/mdat/moof...）

    uint32_t movie_timescale = 0;        // mvhd.timescale
    uint32_t sidx_count = 0;
    uint32_t styp_count = 0;

    std::vector<Mp4TrackSampleTable> tracks;
    std::vector<Mp4FragmentInfo> fragments;
    std::vector<Mp4ConsistencyIssue> issues;   // Validate() 产出的全部一致性问题

    // 快启判定: 分片文件没有大 moov，天然可边下边播
    bool IsFastStart() const { return fragmented || moov_before_mdat; }

    int CountIssues(IssueSeverity severity) const {
        int n = 0;
        for (const auto& i : issues) {
            if (i.severity == severity) ++n;
        }
        return n;
    }
    bool HasIssue(const std::string& code) const {
        for (const auto& i : issues) {
            if (i.code == code) return true;
        }
        return false;
    }
    const Mp4ConsistencyIssue* FindIssue(const std::string& code) const {
        for (const auto& i : issues) {
            if (i.code == code) return &i;
        }
        return nullptr;
    }

    const Mp4TrackSampleTable* FindTrack(uint32_t track_id) const {
        for (const auto& t : tracks) {
            if (t.track_id == track_id) return &t;
        }
        return nullptr;
    }
    // 首个指定类型的轨道
    const Mp4TrackSampleTable* FirstTrack(const std::string& type) const {
        for (const auto& t : tracks) {
            if (t.type == type) return &t;
        }
        return nullptr;
    }
    size_t TotalSamples() const {
        size_t n = 0;
        for (const auto& t : tracks) n += t.samples.size();
        return n;
    }
};

}  // namespace model
}  // namespace videoeye
