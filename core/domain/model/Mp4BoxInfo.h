#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// MP4 Box 树节点
struct Mp4BoxNode {
    std::string type;           // 4CC 名称如 "moov", "trak", "stbl", "stts"
    uint64_t size = 0;          // Box 总大小
    uint64_t offset = 0;        // Box 在文件中的偏移
    int depth = 0;              // 树深度
    std::vector<Mp4BoxNode> children;

    // 字段信息 (从自研 utils::IsobmffParser 收集)
    struct Field {
        std::string name;
        std::string value;      // 字符串表示
    };
    std::vector<Field> fields;
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

// stss: Sync Sample (关键帧列表)
struct StssEntry {
    uint32_t sample_number = 0;
};

// 每个 Track 的 Box 表数据
struct TrackBoxTables {
    int track_id = 0;
    std::string track_type;         // "video", "audio", etc.
    std::vector<SttsEntry> stts_entries;
    std::vector<StcoEntry> stco_entries;
    std::vector<Co64Entry> co64_entries;
    std::vector<StscEntry> stsc_entries;
    std::vector<StszEntry> stsz_entries;
    std::vector<StssEntry> stss_entries;  // stss: 关键帧列表
    uint32_t stsz_default_size = 0;
    uint32_t stsz_sample_count = 0;
};

// MP4 Box 分析结果
struct Mp4BoxAnalysisResult {
    std::string file_path;
    std::vector<Mp4BoxNode> box_tree;
    std::vector<TrackBoxTables> track_tables;
    bool valid = false;
    std::string error_message;
};

} // namespace model
} // namespace videoeye
