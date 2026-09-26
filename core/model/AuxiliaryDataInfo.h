#pragma once

// 辅助数据轨（data stream）的数据模型（功能 9）。
//
// 交付审核里"辅助数据"主要指三类东西：
//   1) SCTE-35 cue —— 广告插入点，直播/OTT 交付必查，错了直接漏播或插错位置；
//   2) 时码轨 / KLV / 图文电视这类 data stream —— 单独占一条流，容易被封装环节丢掉；
//   3) metadata tag —— 语言 tag、handler name、编码器签名这类 key-value。
// 本层只做数据建模；SCTE-35 的二进制解析在 core/analyzer/Scte35Analyzer.h，
// FFmpeg 侧的流枚举与 metadata 采集在 core/analyzer/AuxDataAnalyzer.h。
// QC 规则 id 前缀为 "scte35." / "metadata."。
//
// 依赖边界：纯 C++17，不碰 Qt / FFmpeg，SCTE-35 解析可单测。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 一条 metadata（容器级或流级）
struct MetadataTagEntry {
    std::string scope;        // "容器" / "流 0" / "章节 3"
    int stream_index = -1;    // 流级 tag 的流号（容器级为 -1）
    int chapter_index = -1;
    std::string key;
    std::string value;
};

// data stream 的类型
enum class AuxStreamKind {
    Unknown = 0,
    Scte35,     // SCTE-35 cue（广告插入点）
    Timecode,   // MOV/MP4 tmcd 时码轨
    Klv,        // SMPTE 336M KLV（无人机/军工常见）
    Teletext,   // 图文电视
    Vbi,        // VBI / 垂直消隐数据
    SubtitleData,  // 以 data 流承载的字幕（如 CEA-608 in data）
    GenericData,   // 其它私有 data
};

struct AuxDataStream {
    int stream_index = -1;
    int media_type = -1;         // AVMediaType（4 = AVMEDIA_TYPE_DATA，3 = SUBTITLE）
    AuxStreamKind kind = AuxStreamKind::Unknown;
    std::string codec_name;
    std::string codec_tag;
    std::string handler_name;    // MP4 hdlr name（"MetadataHandler" 之类）
    std::string language;
    std::string title;
    int64_t packet_count = 0;
    int64_t byte_count = 0;
    double start_seconds = 0.0;
    double duration_seconds = 0.0;
    std::string note;
};

// SCTE-35 splice_command_type
enum class Scte35Command {
    Unknown = 0,
    SpliceNull,             // 0x00
    SpliceSchedule,         // 0x04
    SpliceInsert,           // 0x05
    TimeSignal,             // 0x06
    BandwidthReservation,   // 0x07
    PrivateCommand,         // 0xFF
};

// segmentation_descriptor 的关键字段
struct Scte35Segmentation {
    bool present = false;
    uint32_t segmentation_event_id = 0;
    bool has_event_id = false;
    bool cancel = false;
    uint32_t type_id = 0;
    double duration_seconds = 0.0;   // segmentation_duration / 90000
    bool has_duration = false;
    uint8_t segment_num = 0;
    uint8_t segments_expected = 0;
    std::string upid_summary;        // UPID 的可读摘要（类型 + 十六进制/ASCII）
    std::string type_name;           // 中文名，如 "广告开始 (Provider Advertisement Start)"
};

// 一条 SCTE-35 cue（splice_info_section）
struct Scte35Cue {
    int index = 0;
    int stream_index = -1;
    double packet_pts_seconds = -1.0;   // 携带该 section 的包时间（秒，-1 = 无）

    uint8_t table_id = 0;
    uint8_t protocol_version = 0;
    Scte35Command command = Scte35Command::Unknown;

    uint32_t event_id = 0;
    bool has_event_id = false;
    bool cancel_indicator = false;
    bool out_of_network = false;       // OUT（进广告）/ IN（回节目）
    bool program_splice = false;
    bool splice_immediate = false;
    bool duration_flag = false;
    bool auto_return = false;

    int64_t pts_adjustment = 0;
    // splice_time：33 bit PTS / 90000，(pts_adjustment + pts_time) 取模 2^33
    double splice_time_seconds = 0.0;
    bool has_splice_time = false;
    double break_duration_seconds = 0.0;
    bool has_duration = false;

    uint16_t unique_program_id = 0;
    bool has_unique_program_id = false;
    uint8_t avail_num = 0;
    uint8_t avails_expected = 0;
    bool has_avail = false;

    uint16_t tier = 0;
    std::vector<Scte35Segmentation> segmentation;   // 通常 0 或 1 条
    uint32_t descriptor_count = 0;

    bool crc_checked = false;
    bool crc_valid = false;
    bool valid = false;
    std::string parse_error;

    // 广告插入点是否"可用"：既要有明确的 splice 时间，也要有 out/in 语义
    bool HasSplicePoint() const { return valid && has_splice_time && !cancel_indicator; }
    // "OUT"(进广告) / "IN"(回节目) / "-"
    const char* NetworkIndicatorText() const;
};

struct AuxiliaryDataResult {
    std::vector<AuxDataStream> streams;
    std::vector<Scte35Cue> cues;
    std::vector<MetadataTagEntry> metadata;

    int scte35_cue_count = 0;
    int scte35_out_count = 0;    // out_of_network = true 的 cue
    int scte35_in_count = 0;
    int parse_error_count = 0;
    bool crc_checked_any = false;
    int crc_invalid_count = 0;

    bool analyzed = false;
    std::string error_message;

    bool HasScte35() const { return scte35_cue_count > 0; }
};

const char* ToString(AuxStreamKind kind);
const char* ToString(Scte35Command command);
const char* Scte35CommandCode(Scte35Command command);
// segmentation_type_id -> 中文名（未收录的返回 nullptr）
const char* Scte35SegmentationTypeName(uint32_t type_id);

}  // namespace model
}  // namespace videoeye
