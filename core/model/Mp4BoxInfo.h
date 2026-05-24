#pragma once

#include <QString>
#include <QVector>
#include <QMetaType>
#include <cstdint>

namespace videoeye {
namespace model {

// MP4 Box 树节点
struct Mp4BoxNode {
    QString type;           // 4CC 名称如 "moov", "trak", "stbl", "stts"
    uint64_t size = 0;      // Box 总大小
    uint64_t offset = 0;    // Box 在文件中的偏移
    int depth = 0;          // 树深度
    QVector<Mp4BoxNode> children;

    // 字段信息 (从 Bento4 Inspector 的 AddField 收集)
    struct Field {
        QString name;
        QString value;      // 字符串表示
    };
    QVector<Field> fields;
};

// stts: Time-to-Sample
struct SttsEntry {
    uint32_t sample_count = 0;
    uint32_t sample_delta = 0;
};

// stco: Chunk Offset
struct StcoEntry {
    uint32_t chunk_offset = 0;
};

// co64: 64-bit Chunk Offset
struct Co64Entry {
    uint64_t chunk_offset = 0;
};

// stsc: Sample-to-Chunk
struct StscEntry {
    uint32_t first_chunk = 0;
    uint32_t samples_per_chunk = 0;
    uint32_t sample_description_index = 0;
};

// stsz: Sample Size
struct StszEntry {
    uint32_t sample_size = 0;
};

// 每个 Track 的 Box 表数据
struct TrackBoxTables {
    int track_id = 0;
    QString track_type;         // "video", "audio", etc.
    QVector<SttsEntry> stts_entries;
    QVector<StcoEntry> stco_entries;
    QVector<Co64Entry> co64_entries;
    QVector<StscEntry> stsc_entries;
    QVector<StszEntry> stsz_entries;
    uint32_t stsz_default_size = 0;
    uint32_t stsz_sample_count = 0;
};

// MP4 Box 分析结果
struct Mp4BoxAnalysisResult {
    QString file_path;
    QVector<Mp4BoxNode> box_tree;
    QVector<TrackBoxTables> track_tables;
    bool valid = false;
    QString error_message;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::Mp4BoxNode)
Q_DECLARE_METATYPE(videoeye::model::TrackBoxTables)
Q_DECLARE_METATYPE(videoeye::model::Mp4BoxAnalysisResult)
