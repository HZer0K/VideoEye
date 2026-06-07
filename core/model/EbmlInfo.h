#pragma once

#include <QString>
#include <QVector>
#include <QMetaType>
#include <cstdint>

namespace videoeye {
namespace model {

/// EBML 元素树节点 (用于 MKV/WebM 结构分析)
struct EbmlElementNode {
    uint64_t id = 0;
    QString id_hex;
    QString name;
    uint64_t size = 0;
    uint64_t offset = 0;       // 数据区在文件中的偏移
    uint64_t header_size = 0;
    int depth = 0;
    QVector<EbmlElementNode> children;
    QString value;             // 叶子节点的解析值
    QString extra;             // 额外解析信息 (如 Block 解析)

    uint64_t startOffset() const { return offset - header_size; }
};

/// 从 TrackEntry 解析出的轨道信息
struct EbmlTrackInfo {
    int track_number = 0;
    uint64_t track_uid = 0;
    int track_type = 0;        // 1=video, 2=audio, 0x11=subtitle
    QString track_type_name;   // "视频", "音频", "字幕", "其他"
    QString codec_id;          // "V_VP9", "A_OPUS" 等
    QString codec_name;        // 解析后的可读编码名
    QString language;          // "eng", "chi" 等
    QString track_name;        // 轨道名称
    int pixel_width = 0;
    int pixel_height = 0;
    double frame_rate = 0.0;
    double sampling_frequency = 0.0;
    int channels = 0;
    int bit_depth = 0;
    uint64_t default_duration = 0;
    int codec_private_size = 0; // CodecPrivate 大小 (SPS/PPS等)
    bool enabled = true;
    bool default_track = false;
    bool forced = false;
    bool lacing = false;
};

/// 从 CuePoint 解析出的索引条目
struct EbmlCueEntry {
    uint64_t time = 0;          // 时间戳 (TimestampScale 单位)
    int track_number = 0;
    uint64_t cluster_position = 0; // Cluster 的文件偏移
    uint64_t block_number = 0;     // Cluster 内的块序号
};

/// 从 BlockGroup/SimpleBlock 解析出的数据块信息摘要
struct EbmlBlockSummary {
    int track_number = 0;
    int16_t timecode = 0;       // 相对时间偏移
    bool keyframe = false;
    bool discardable = false;
    uint64_t cluster_offset = 0;
    uint64_t block_offset = 0;
    int data_size = 0;
    bool lacing = false;
};

/// EBML/MKV/WebM 结构分析结果
struct EbmlAnalysisResult {
    QString file_path;
    QVector<EbmlElementNode> element_tree;
    QString doc_type;                     // "matroska", "webm"
    int doc_type_version = 0;
    int doc_type_read_version = 0;

    // 解析阶段的统计
    int ebml_version = 0;
    int ebml_read_version = 0;
    int ebml_max_id_length = 0;
    int ebml_max_size_length = 0;

    // 从 Info 提取
    uint64_t timestamp_scale = 1000000;   // 默认 1ms
    double duration_seconds = 0.0;        // 时长 (秒)
    QString title;
    QString muxing_app;
    QString writing_app;
    QString segment_uid;

    // 提取的表格数据
    QVector<EbmlTrackInfo> tracks;        // 轨道表
    QVector<EbmlCueEntry> cues;           // 索引表
    QVector<EbmlBlockSummary> blocks;     // 数据块摘要 (截断: 最多 1000 条)
    int total_clusters = 0;
    int total_blockgroups = 0;
    int total_simpleblocks = 0;

    bool valid = false;
    QString error_message;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::EbmlAnalysisResult)
