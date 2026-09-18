#pragma once

// MP4/fMP4 容器一致性问题
//
// 与 QC 体系的关系:
//   Mp4SampleTableAnalyzer 负责"发现问题"，QcRuleEngine 负责"决定是否上报、以什么级别上报"。
//   二者通过 code 串起来（规则 id 与 finding code 同名），这样：
//     - 容器页可以直接展示全部 finding（含被规则关掉的 Info 级细节）；
//     - 「诊断与报告」页只会展示规则开启且阈值允许的问题，并能被用户改阈值。

#include <string>

#include "core/model/DiagnosticIssue.h"
#include "core/model/TimeRange.h"

namespace videoeye {
namespace model {

// 一致性问题码（同时用作 QC 规则 id）
namespace Mp4IssueCode {
// 文件级
constexpr const char* kNotFastStart          = "container.mp4.not_faststart";
constexpr const char* kSampleCountMismatch   = "container.mp4.sample_count_mismatch";
constexpr const char* kMissingChunkOffsets   = "container.mp4.missing_chunk_offsets";
constexpr const char* kStcoOverflow          = "container.mp4.stco_overflow";
constexpr const char* kBothStcoAndCo64       = "container.mp4.stco_co64_conflict";
// 样本级
constexpr const char* kChunkOffsetOutOfRange = "container.mp4.chunk_offset_out_of_range";
constexpr const char* kZeroSizeSample        = "container.mp4.zero_size_sample";
constexpr const char* kNegativeCts           = "container.mp4.negative_cts";
constexpr const char* kDtsNotMonotonic       = "container.mp4.dts_not_monotonic";
constexpr const char* kZeroDuration          = "container.mp4.zero_duration_sample";
constexpr const char* kOffsetDiscontinuity   = "container.mp4.sample_offset_gap";
constexpr const char* kFirstSampleNotSync    = "container.mp4.first_sample_not_sync";
constexpr const char* kStssCountMismatch     = "container.mp4.stss_count_mismatch";
// edit list
constexpr const char* kElstFirstFrameShift   = "container.mp4.elst_first_frame_shift";
constexpr const char* kElstEmptyEdit         = "container.mp4.elst_empty_edit";
constexpr const char* kAvStartMismatch       = "container.mp4.av_start_mismatch";
// 分片
constexpr const char* kFragmentSequenceGap   = "container.mp4.fragment_sequence_gap";
constexpr const char* kFragmentTimeGap       = "container.mp4.fragment_time_gap";
constexpr const char* kFragmentTimeOrigin    = "container.mp4.fragment_time_origin";
constexpr const char* kFragmentDataOffset    = "container.mp4.fragment_data_offset";
}  // namespace Mp4IssueCode

struct Mp4ConsistencyIssue {
    std::string code;                 // 见 Mp4IssueCode
    std::string title;
    std::string detail;               // 含实测值
    std::string suggestion;
    IssueSeverity severity = IssueSeverity::Warning;
    IssueCategory category = IssueCategory::Container;
    int track_id = -1;                // 关联轨道（-1 = 文件级）
    int fragment_index = -1;          // 关联分片（-1 = 不适用）
    uint32_t sample_index = 0;        // 首个相关样本（用于 UI 定位）
    bool has_sample_index = false;
    double metric_value = 0.0;
    double threshold = 0.0;
    int occurrence_count = 1;

    // 转成 QC 体系统一的 DiagnosticIssue（规则引擎据此生成报告）
    // seconds >= 0 时把问题定位到该时间点，否则标记为全局问题。
    DiagnosticIssue ToDiagnosticIssue(double seconds = -1.0) const {
        DiagnosticIssue issue;
        issue.rule_id = code;
        issue.title = title;
        issue.detail = detail;
        issue.suggestion = suggestion;
        issue.severity = severity;
        issue.category = category;
        issue.stream_index = -1;
        issue.metric_value = metric_value;
        issue.threshold = threshold;
        issue.occurrence_count = occurrence_count;
        issue.range = (seconds >= 0.0) ? TimeRange::At(seconds) : TimeRange::Global();
        return issue;
    }
};

}  // namespace model
}  // namespace videoeye
