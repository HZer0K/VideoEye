#pragma once

// AVI / FLV / ASF / OGG / MKV / MXF / MPEG-PS 的容器级体检事实。
//
// 为什么单独成一个值对象（与 TransportStreamInfo 同样的理由）：
//   * 容器结构页要的是整棵树，QC 只要十来个计数；
//   * ContainerStructureResult 是展示模型，规则层直接依赖它会把报告口径和界面绑死。
// 各分析器在解析时顺手填这份摘要，结构页与 QC 共用同一组数字。
//
// 口径提醒：scan_complete=false 表示只扫了窗口（结构页有元素上限），
// 此时"没发现缺 EOS / 时间戳回退"不代表整个文件没问题。

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

struct AviDiagnostics {
    bool analyzed = false;
    bool scan_complete = true;
    uint64_t file_size = 0;

    // ---- 索引（idx1 / OpenDML indx / dmlh）----
    bool has_idx1 = false;
    bool has_indx = false;
    bool has_dmlh = false;
    bool has_avix = false; // 存在 OpenDML 扩展 RIFF（'AVIX'）
    uint64_t idx1_entries = 0;
    uint64_t idx1_bad_offsets = 0; // offset + size 越过文件末尾
    uint64_t indx_entries = 0;
    bool indx_truncated = false; // indx 头声明条目数装不下实际块长
    uint64_t dmlh_total_frames = 0;

    // ---- 结构 ----
    bool riff_size_exceeds_file = false;
    uint64_t truncated_chunks = 0;
    int video_streams = 0;
    int audio_streams = 0;
    uint64_t max_video_stream_length = 0; // strh.dwLength（视频轨，单位由 dwScale/dwRate 决定）
    int zero_length_streams = 0;          // strh.dwLength == 0
};

struct FlvDiagnostics {
    bool analyzed = false;
    bool scan_complete = true;

    bool header_has_video = false;
    bool header_has_audio = false;
    bool has_metadata = false;

    uint64_t scanned_tags = 0;
    uint64_t video_tags = 0;
    uint64_t audio_tags = 0;
    uint64_t script_tags = 0;

    // 需要序列头才能解码的编码（H.264 / HEVC / AV1）
    bool video_codec_avc = false;
    bool video_codec_hevc = false;
    bool video_codec_av1 = false;
    bool video_sequence_header_seen = false;

    uint64_t timestamp_backwards = 0; // 同一 tag 类型内时间戳回退次数
    uint64_t truncated_tags = 0;      // tag 数据越过文件末尾
    uint64_t tag_size_mismatch = 0;   // PreviousTagSize 与实际不符
    uint64_t max_timestamp_ms = 0;
};

struct AsfDiagnostics {
    bool analyzed = false;
    bool scan_complete = true;

    bool has_data_object = false;
    bool has_index_object = false;

    uint32_t declared_header_objects = 0;
    uint32_t parsed_header_objects = 0;
    uint64_t invalid_object_sizes = 0; // size < 24 或越过文件末尾
    bool header_size_exceeds_file = false;
    bool duration_invalid = false;
    double duration_seconds = 0.0;
};

struct OggDiagnostics {
    bool analyzed = false;
    bool scan_complete = true;

    uint64_t pages = 0;
    uint64_t logical_streams = 0;
    uint64_t sequence_gaps = 0; // page_seq 不连续或回退
    uint64_t missing_bos = 0;   // 某逻辑流的首个页面不是 BOS
    uint64_t missing_eos = 0;   // 扫到文件尾仍未看到 EOS
    uint64_t truncated_pages = 0;
    uint64_t resyncs = 0; // 非 OggS 处重新同步次数
};

// MKV / WebM 的容器级体检事实（来自 MkvContainerScanner 的元素级顺序扫描）。
//
// 为什么不是直接读 EbmlAnalysisResult：那是结构页用的整棵元素树（受
// kMaxNodes / 内存窗口限制，长片根本装不下）。QC 要的是"整份文件都扫过"的
// 十来个计数，所以另起一遍只读元素头的顺序扫描：不建树、不读载荷，
// 内存 O(1)，能覆盖到片尾。
//
// 口径提醒：scan_complete=false 表示撞到元素预算或中途元素头越界，
// 此时"没有 Cues / 没有时间戳回退"都不代表整个文件没问题。
struct MkvDiagnostics {
    bool analyzed = false;
    bool scan_complete = true;
    bool truncated = false; // 元素头越过 Segment / 文件末尾（文件被截断）
    std::string doc_type;   // "matroska" / "webm"

    // ---- 索引与元数据段 ----
    bool has_seek_head = false;
    bool has_cues = false; // 点播不可 seek 的直接原因
    bool has_tags = false;
    bool has_chapters = false;

    // ---- Cluster 时间轴 ----
    uint64_t clusters = 0;
    uint64_t clusters_with_timecode = 0;
    uint64_t cluster_timestamp_backwards = 0; // 相邻 Cluster 时间戳回退次数
    double last_cluster_seconds = 0.0;        // 最后一个 Cluster 的时间戳（诊断用）

    uint64_t unknown_size_elements = 0; // size 全 1（流式写入未回填长度）

    // ---- 轨道 ----
    uint64_t video_tracks = 0;
    uint64_t audio_tracks = 0;
    // 必须带 CodecPrivate（avcC / hvcC / AudioSpecificConfig / Vorbis 头）才能解码的轨道里，
    // 实际没有 CodecPrivate 的条数。缺了它解码器无法初始化。
    uint64_t tracks_missing_codec_private = 0;
    std::string missing_codec_private_codecs; // 逗号分隔的 CodecID 列表
};

// MXF（SMPTE 377M，KLV 容器）的容器级体检事实。
//
// 事实来自 MxfStructureAnalyzer 的顺序 KLV 扫描：只读 16 字节键与 BER 长度，
// 载荷一律 Seek 跳过，内存 O(1)。所以这些计数覆盖整个扫描窗口，与结构页那棵
// 受 max_tree_elements 约束的元素树不是一回事。
//
// 与 MKV 的差别：MXF 的关键结构（Footer Partition、Random Index Pack）在**文件尾**，
// 只靠从前往后的窗口扫不到。所以分析器另外反向读一段文件尾找 RIP ——
// 这条事实不受窗口影响，`has_rip` 在任何文件大小下都是可信的。
//
// 口径提醒：scan_complete=false 表示扫到窗口/对象预算就停了，
// 此时"没有 Footer / 没有 Index Table"只代表已扫描的部分。
struct MxfDiagnostics {
    bool analyzed = false;
    bool scan_complete = true;
    uint64_t file_size = 0;

    // ---- 分区结构（SMPTE 377M Partition Pack）----
    uint64_t header_partitions = 0;
    uint64_t body_partitions = 0;
    uint64_t footer_partitions = 0;
    // 第一个分区必须是 Header；0 = 一个分区都没认出来。
    uint8_t first_partition_kind = 0; // 0x02 头 / 0x03 体 / 0x04 尾

    // ---- 元数据 ----
    bool has_primer = false; // Header 分区里的 Primer Pack：解引用动态 UL 的前提
    uint64_t primer_packs = 0;
    uint64_t index_segments = 0; // Index Table Segment：精确定位的前提

    // 片尾 Random Index Pack（06.0E.2B.34.02.05.01.01.0D.01.02.01.01.11.01.00）。
    // 有它才能不线性扫描而直接跳到任一分区。
    bool has_rip = false;
    bool rip_malformed = false; // 找到 RIP 键但载荷不足以读完分区对

    // ---- 载荷 ----
    uint64_t essence_objects = 0;
    uint64_t essence_bytes = 0;
    uint64_t metadata_objects = 0;
    uint64_t tracks = 0;

    // ---- 损伤 ----
    uint64_t resyncs = 0;          // 键前缀失配后向前找下一个 SMPTE 键的次数
    uint64_t truncated_values = 0; // KLV 声明长度越过扫描边界
    bool truncated = false;        // 明确越过文件末尾（区别于只是越过窗口）
};

// MPEG-PS（Program Stream，.mpg/.vob/.m2p）的容器级体检事实。
//
// 事实来自 MpegPsStructureAnalyzer 的起始码顺序扫描。PS 没有 PAT/PMT 也没有固定包长，
// 所以"流表对不对"只能靠 PSM 声明与实际 PES 对账。
//
// 口径提醒：时间戳一律优先取 DTS —— 视频 PES 按解码序排列，有 B 帧时相邻 PTS
// 本来就会回退一个 GOP，拿 PTS 判单调必然误报。没有 DTS 的流不计入单调性判定。
struct MpegPsDiagnostics {
    bool analyzed = false;
    bool scan_complete = true;
    uint64_t file_size = 0;

    // ---- 结构计数 ----
    uint64_t pack_headers = 0;
    uint64_t mpeg1_pack_headers = 0;
    uint64_t mpeg2_pack_headers = 0;
    uint64_t system_headers = 0;
    uint64_t psm_count = 0;
    uint64_t pes_packets = 0;
    uint64_t padding_packets = 0;

    // Program End Code（00 00 01 B9）。注意它**不是**缺陷判据：ffmpeg 的 mpeg/vob 复用器
    // 从不写它，DVD 的 VOB 按规范也不写。这里只作为事实记录（结构页 metadata 同源）。
    bool has_end_code = false;
    uint64_t truncated_start_codes = 0; // 起始码声明的长度越过文件末尾

    // ---- 流 ----
    uint64_t streams = 0;
    uint64_t video_streams = 0;
    uint64_t audio_streams = 0;
    // 有 PES 但一个时间戳都没解析出来的流。只有在"同文件里别的流解析出了时间戳"
    // 时才有意义 —— 见规则层门控，否则分不清是流缺时间戳还是解析器没认出来。
    uint64_t streams_with_timestamp = 0;
    uint64_t streams_without_timestamp = 0;
    // 同一流内 DTS 回退次数（DTS 按定义单调非降）。
    uint64_t timestamp_backwards = 0;
    // PSM 声明与实际 PES 对不上：未被 PSM 声明的流 / PSM 声明了却没有 PES 的流。
    uint64_t unmapped_streams = 0;
    uint64_t declared_but_missing_streams = 0;
};

struct ContainerDiagnosticsInfo {
    AviDiagnostics avi;
    FlvDiagnostics flv;
    AsfDiagnostics asf;
    OggDiagnostics ogg;
    MkvDiagnostics mkv;
    MxfDiagnostics mxf;
    MpegPsDiagnostics mpeg_ps;
};

} // namespace model
} // namespace videoeye
