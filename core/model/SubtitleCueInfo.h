#pragma once

// 字幕轨的数据模型（功能 9）。
//
// 交付审核里字幕是"最容易被漏检、但一眼就能被客户看见"的部分：
//   空字幕、重叠、过短/过长、时间倒序 —— 这些用眼睛扫一遍 SRT 看不出来，
//   但播放时就是闪一下或压字。这一层把这些现象统一建模，
//   解析在 core/analyzer/SubtitleAnalyzer.h，QC 规则 id 前缀为 "subtitle."。
//
// 依赖边界：纯 C++17，不碰 Qt / FFmpeg，方便单测直接构造 cue 做校验。

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/DiagnosticIssue.h"

namespace videoeye {
namespace model {

// 字幕格式。文本类可以直接解析出 cue；图形类第一阶段只给流 metadata + 包时间线。
enum class SubtitleFormat {
    Unknown = 0,
    Srt,         // SubRip(*.srt)，Matroska 里也常见
    Ssa,         // SubStation Alpha v4
    Ass,         // Advanced SubStation Alpha
    WebVtt,      // WebVTT(*.vtt)
    MovText,     // MP4/MOV 的 tx3g 文本字幕
    Cea608,      // CEA-608 / EIA-608 closed caption
    Cea708,      // CEA-708 / DTVCC
    DvbSubtitle, // DVB 图形字幕（Bitmap）
    DvdSubtitle, // DVD / VOBSUB 图形字幕
    HdmvPgs,     // Blu-ray PGS 图形字幕
    Xsub,        // DivX XSUB
    Teletext,    // DVB/图文电视字幕
    OtherText,   // 其它可当文本读的字幕
    OtherBitmap, // 其它图形字幕
};

// 字幕承载方式
enum class SubtitleKind {
    Unknown = 0,
    Text,            // 文本字幕（能直接解析文本与时长）
    Bitmap,          // 图形字幕（需解码位图，第二阶段做预览）
    ClosedCaption,   // CEA-608/708 之类的隐藏字幕数据流
};

// 单条 cue 的问题类型。顺序即 UI 表格里的默认顺序。
enum class SubtitleIssueType {
    EmptyText = 0,     // 空字幕（无可见文本）
    Overlap,           // 与上一条 cue 时间重叠
    TooShort,          // 停留时间过短（来不及读）
    TooLong,           // 停留时间过长（通常意味着忘记结束）
    NonMonotonic,      // 时间倒序（开始时间早于上一条）
    InvalidDuration,   // end <= start
    TooFast,           // 阅读速度过快（字符/秒）
    OutOfRange,        // 超出媒体时长
    MissingLanguage,   // 流级：缺少 language tag
    MissingHandler,    // 流级：缺少 handler name（MP4 hdlr）
};

// 一条字幕 problem（QC 报告里以 category=Metadata 呈现）
struct SubtitleIssue {
    SubtitleIssueType type = SubtitleIssueType::EmptyText;
    IssueSeverity severity = IssueSeverity::Warning;
    int stream_index = -1;
    int cue_index = -1;              // 流内 cue 序号（-1 = 流级问题）
    double start_seconds = -1.0;
    double end_seconds = -1.0;
    std::string detail;              // 中文简述，含关键数值

    bool IsStreamLevel() const { return cue_index < 0; }
};

// 一条字幕 cue
struct SubtitleCue {
    int index = 0;                   // 流内序号（0-based，与 issues.cue_index 对应）
    int stream_index = -1;
    int cue_number = -1;             // SRT 里的序号列（-1 = 格式本身没有）
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    double duration_seconds = 0.0;   // end - start（<=0 表示未给出结束时间）

    std::string text;                // 展示用纯文本（已剥离 ASS 覆盖指令 / WebVTT 标签）
    std::string raw_text;            // 原始文本（可含标签）
    std::string language;            // ISO 639 语言 tag（可能为空）
    SubtitleFormat format = SubtitleFormat::Unknown;
    size_t char_count = 0;           // 可见字符数（用于阅读速度）

    bool empty = false;              // 剥离标签后没有任何可见字符
    bool has_issue = false;

    double Duration() const { return end_seconds - start_seconds; }
    // 阅读速度（字符/秒）；时长为 0 时返回 -1（无意义）
    double CharsPerSecond() const;
};

// 一条字幕流的静态摘要 + 解析状态
struct SubtitleStreamInfo {
    int stream_index = -1;
    SubtitleFormat format = SubtitleFormat::Unknown;
    SubtitleKind kind = SubtitleKind::Unknown;
    std::string codec_name;     // avcodec_get_name() 的结果
    std::string codec_tag;      // MP4 的 'tx3g' / 'c608' 等四字符 tag
    std::string language;       // language tag
    std::string title;          // metadata title
    std::string handler_name;   // MP4 hdlr 里的 handler name
    bool default_disposition = false;
    bool forced = false;
    bool hearing_impaired = false;
    bool burned_in_note = false;  // 占位：容器标记了"已烧录"的旁注

    int cue_count = 0;            // 解析出的 cue 数
    int64_t packet_count = 0;
    int64_t byte_count = 0;
    double start_seconds = 0.0;
    double duration_seconds = 0.0;

    // 文本字幕是否已真正解析出 cue（图形字幕为 false，只给 metadata）
    bool parsed = false;
    std::string note;             // 降级/来源说明（如"图形字幕仅输出 metadata"）

    bool IsText() const { return kind == SubtitleKind::Text; }
};

// 一次字幕分析的结果
struct SubtitleAnalysisResult {
    std::vector<SubtitleStreamInfo> streams;
    std::vector<SubtitleCue> cues;
    std::vector<SubtitleIssue> issues;

    int text_stream_count = 0;
    int bitmap_stream_count = 0;
    int total_cue_count = 0;
    bool analyzed = false;
    std::string error_message;

    int CountIssues(SubtitleIssueType type) const;
    bool HasIssue() const { return !issues.empty(); }
    // 只统计 Warning 及以上
    int CountWarnings() const;
};

// 显示用名称（UI 表格 / 报告导出）
const char* ToString(SubtitleFormat format);
const char* ToString(SubtitleIssueType type);
const char* ToString(SubtitleKind kind);

// 稳定英文 id，用于 CSV / JSON 导出，避免中文编码问题
const char* SubtitleFormatCode(SubtitleFormat format);
const char* SubtitleIssueCode(SubtitleIssueType type);

}  // namespace model
}  // namespace videoeye
