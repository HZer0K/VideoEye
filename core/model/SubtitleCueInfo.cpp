#include "core/model/SubtitleCueInfo.h"

#include <cmath>

namespace videoeye {
namespace model {

double SubtitleCue::CharsPerSecond() const {
    const double dur = Duration();
    if (dur <= 0.0) return -1.0;
    return static_cast<double>(char_count) / dur;
}

int SubtitleAnalysisResult::CountIssues(SubtitleIssueType type) const {
    int count = 0;
    for (const SubtitleIssue& issue : issues) {
        if (issue.type == type) ++count;
    }
    return count;
}

int SubtitleAnalysisResult::CountWarnings() const {
    int count = 0;
    for (const SubtitleIssue& issue : issues) {
        if (issue.severity == IssueSeverity::Warning || issue.severity == IssueSeverity::Error ||
            issue.severity == IssueSeverity::Critical) {
            ++count;
        }
    }
    return count;
}

const char* ToString(SubtitleFormat format) {
    switch (format) {
        case SubtitleFormat::Srt:         return "SubRip (SRT)";
        case SubtitleFormat::Ssa:         return "SubStation Alpha (SSA)";
        case SubtitleFormat::Ass:         return "Advanced SSA (ASS)";
        case SubtitleFormat::WebVtt:      return "WebVTT";
        case SubtitleFormat::MovText:     return "MOV/MP4 文本 (tx3g)";
        case SubtitleFormat::Cea608:      return "CEA-608";
        case SubtitleFormat::Cea708:      return "CEA-708";
        case SubtitleFormat::DvbSubtitle: return "DVB 图形字幕";
        case SubtitleFormat::DvdSubtitle: return "DVD/VOBSUB 图形字幕";
        case SubtitleFormat::HdmvPgs:     return "Blu-ray PGS";
        case SubtitleFormat::Xsub:        return "XSUB";
        case SubtitleFormat::Teletext:    return "Teletext";
        case SubtitleFormat::OtherText:   return "其它文本字幕";
        case SubtitleFormat::OtherBitmap: return "其它图形字幕";
        case SubtitleFormat::Unknown:
        default:                          return "未知";
    }
}

const char* ToString(SubtitleKind kind) {
    switch (kind) {
        case SubtitleKind::Text:          return "文本";
        case SubtitleKind::Bitmap:        return "图形";
        case SubtitleKind::ClosedCaption: return "隐藏字幕数据流";
        case SubtitleKind::Unknown:
        default:                          return "未知";
    }
}

const char* ToString(SubtitleIssueType type) {
    switch (type) {
        case SubtitleIssueType::EmptyText:       return "空字幕";
        case SubtitleIssueType::Overlap:         return "与前一条重叠";
        case SubtitleIssueType::TooShort:        return "停留过短";
        case SubtitleIssueType::TooLong:         return "停留过长";
        case SubtitleIssueType::NonMonotonic:    return "时间倒序";
        case SubtitleIssueType::InvalidDuration: return "时长非法";
        case SubtitleIssueType::TooFast:         return "阅读速度过快";
        case SubtitleIssueType::OutOfRange:      return "超出媒体时长";
        case SubtitleIssueType::MissingLanguage: return "缺少语言 tag";
        case SubtitleIssueType::MissingHandler:  return "缺少 handler name";
        default:                                 return "未知问题";
    }
}

const char* SubtitleFormatCode(SubtitleFormat format) {
    switch (format) {
        case SubtitleFormat::Srt:         return "srt";
        case SubtitleFormat::Ssa:         return "ssa";
        case SubtitleFormat::Ass:         return "ass";
        case SubtitleFormat::WebVtt:      return "webvtt";
        case SubtitleFormat::MovText:     return "mov_text";
        case SubtitleFormat::Cea608:      return "cea608";
        case SubtitleFormat::Cea708:      return "cea708";
        case SubtitleFormat::DvbSubtitle: return "dvb_subtitle";
        case SubtitleFormat::DvdSubtitle: return "dvd_subtitle";
        case SubtitleFormat::HdmvPgs:     return "hdmv_pgs";
        case SubtitleFormat::Xsub:        return "xsub";
        case SubtitleFormat::Teletext:    return "teletext";
        case SubtitleFormat::OtherText:   return "other_text";
        case SubtitleFormat::OtherBitmap: return "other_bitmap";
        case SubtitleFormat::Unknown:
        default:                          return "unknown";
    }
}

const char* SubtitleIssueCode(SubtitleIssueType type) {
    switch (type) {
        case SubtitleIssueType::EmptyText:       return "empty_text";
        case SubtitleIssueType::Overlap:         return "overlap";
        case SubtitleIssueType::TooShort:        return "too_short";
        case SubtitleIssueType::TooLong:         return "too_long";
        case SubtitleIssueType::NonMonotonic:    return "non_monotonic";
        case SubtitleIssueType::InvalidDuration: return "invalid_duration";
        case SubtitleIssueType::TooFast:         return "too_fast";
        case SubtitleIssueType::OutOfRange:      return "out_of_range";
        case SubtitleIssueType::MissingLanguage: return "missing_language";
        case SubtitleIssueType::MissingHandler:  return "missing_handler";
        default:                                 return "unknown";
    }
}

}  // namespace model
}  // namespace videoeye
