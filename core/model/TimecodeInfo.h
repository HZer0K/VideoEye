#pragma once

// 时码与章节的数据模型（功能 9）。
//
// 交付规范里"首帧时码"是个硬指标：母版要求 00:59:58:00 起板、广告插入点要报
// SCTE-35 —— 这些都得有精确的时码读数，光看秒数对不上。本层负责：
//   1) SMPTE 12M 时码的表示与换算（含 drop-frame / non-drop-frame）；
//   2) 时码来源：MOV/MP4 的 tmcd track、容器/metadata 里的 timecode tag；
//   3) 章节时间线（越界 / 重叠检查）。
// 解析在 core/analyzer/TimecodeAnalyzer.h，QC 规则 id 前缀 "timecode." / "chapter."。
//
// 依赖边界：纯 C++17，不碰 Qt / FFmpeg，换算逻辑可单测。

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/DiagnosticIssue.h"

namespace videoeye {
namespace model {

// SMPTE 12M 时码。drop_frame 只影响换算，不影响四个字段的语义。
struct Timecode {
    bool valid = false;
    bool drop_frame = false;
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;

    // "HH:MM:SS:FF"（drop frame 用 ';' 分隔，与 SMPTE 写法一致：00:01:02;03）
    std::string ToString() const;
    // 相对 00:00:00:00 的秒数（需要帧率，drop frame 会按 SMPTE 规则换算）
    double ToSeconds(double fps) const;
    int64_t ToFrameCount(double fps) const;
};

// "00:01:02:03" / "00:01:02;03" / "1:02:03.500" → Timecode。
// 支持分隔符 ':' 与 ';'（';' 或 '.' 在帧位前均视为 drop frame 标记），
// 也支持 "HH:MM:SS.mmm"（毫秒）形式 —— 此时按毫秒折算回帧需要 fps，
// 所以毫秒形式只填充时分秒，frames 由调用方按需补齐。
Timecode TimecodeFromString(const std::string& text, double fps = 0.0);

// 帧号 → 时码（frame_count 为 24 小时内的帧序号）
Timecode TimecodeFromFrameCount(int64_t frame_count, double fps, bool drop_frame);

// 帧号的 24 小时上限（用于取模，避免跨天溢出）
int64_t FramesPer24Hours(double fps);

// 时码合法性：字段越界（帧号 >= fps、分秒 >= 60）、drop frame 下跳过了不该出现的帧
bool IsValidTimecode(const Timecode& tc, double fps);

// 29.97 / 59.94 这类帧率通常用 drop frame 时码
bool IsDropFrameRate(double fps);

// 时码来源
enum class TimecodeSource {
    Unknown = 0,
    TimecodeTrack,  // MOV/MP4 的 tmcd track（AV_CODEC_ID_TIMECODE）
    Metadata,       // 容器或流 metadata 里的 timecode tag
    Both,           // 两条路都拿到了（UI 只显示一条，但可交叉核对）
};

// 一条时码轨（同一文件可能既有 tmcd track 又有 metadata tag）
struct TimecodeTrack {
    int stream_index = -1;             // tmcd track 的流号；metadata 来源为 -1
    TimecodeSource source = TimecodeSource::Unknown;
    std::string codec_name;
    std::string metadata_key;          // metadata 来源时的 tag 名（如 "timecode"）
    std::string raw_text;              // metadata 来源时的原始字符串
    double frame_rate = 0.0;           // 换算用的帧率
    bool drop_frame = false;
    int64_t first_frame_count = -1;    // tmcd 首包里的帧序号（-1 = 未取到）
    Timecode first_timecode;           // 首帧时码
    double first_presentation_seconds = 0.0;  // 首帧对应的媒体时间（秒）
    std::string note;
};

// 一个章节（AVFormatContext 的 chapter 列表）
struct ChapterInfo {
    int index = 0;
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    std::string title;
    std::string language;              // 章节可能带语言 metadata
    int64_t start_timestamp = 0;       // 原始时间戳（流 timebase 单位）
    int64_t end_timestamp = 0;

    double Duration() const { return end_seconds - start_seconds; }
};

enum class ChapterIssueType {
    Overlap = 0,     // 与上一章节重叠
    OutOfRange,      // 超出媒体时长
    NonMonotonic,    // 起点早于上一章节
    ZeroDuration,    // 零时长章节
    MissingTitle,
};

struct ChapterIssue {
    ChapterIssueType type = ChapterIssueType::Overlap;
    IssueSeverity severity = IssueSeverity::Warning;
    int chapter_index = -1;
    double start_seconds = -1.0;
    double end_seconds = -1.0;
    std::string detail;
};

struct TimecodeAnalysisResult {
    std::vector<TimecodeTrack> tracks;
    // 主时码：优先 tmcd track（更权威），没有则取 metadata
    Timecode primary;
    bool has_primary = false;
    double primary_frame_rate = 0.0;
    bool primary_drop_frame = false;

    std::vector<ChapterInfo> chapters;
    std::vector<ChapterIssue> chapter_issues;
    double media_duration_seconds = 0.0;

    bool analyzed = false;
    std::string error_message;

    int CountChapterIssues(ChapterIssueType type) const;
};

const char* ToString(TimecodeSource source);
const char* ToString(ChapterIssueType type);
const char* ChapterIssueCode(ChapterIssueType type);

}  // namespace model
}  // namespace videoeye
