#pragma once

// AVI / FLV / ASF / OGG / MKV 的容器级体检事实。
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

struct ContainerDiagnosticsInfo {
    AviDiagnostics avi;
    FlvDiagnostics flv;
    AsfDiagnostics asf;
    OggDiagnostics ogg;
    MkvDiagnostics mkv;
};

} // namespace model
} // namespace videoeye
