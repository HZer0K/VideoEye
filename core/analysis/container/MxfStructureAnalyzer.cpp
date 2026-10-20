#include "core/analysis/container/MxfStructureAnalyzer.h"

#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/SeqFileReader.h"
#include "infrastructure/concurrency/Cancellation.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace videoeye {

namespace {

constexpr size_t kKeySize = 16;

uint16_t MxfBE16(const std::string& d, size_t off) {
    if (off + 2 > d.size()) return 0;
    return static_cast<uint16_t>((static_cast<uint8_t>(d[off]) << 8) |
                                 static_cast<uint8_t>(d[off + 1]));
}

uint32_t MxfBE32(const std::string& d, size_t off) {
    if (off + 4 > d.size()) return 0;
    return (static_cast<uint32_t>(static_cast<uint8_t>(d[off])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3]));
}

uint64_t MxfBE64(const std::string& d, size_t off) {
    if (off + 8 > d.size()) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(d[off + i]);
    return v;
}

bool StartsWithMxfKey(const std::string& key) {
    return key.size() >= 4 && static_cast<uint8_t>(key[0]) == 0x06 &&
           static_cast<uint8_t>(key[1]) == 0x0E &&
           static_cast<uint8_t>(key[2]) == 0x2B &&
           static_cast<uint8_t>(key[3]) == 0x34;
}

// Partition Pack：060E2B34.02.05.01.01.0D0102010101.<kind>.<status>.00
bool IsPartitionKey(const std::string& key) {
    return StartsWithMxfKey(key) && static_cast<uint8_t>(key[4]) == 0x02 &&
           static_cast<uint8_t>(key[5]) == 0x05 &&
           static_cast<uint8_t>(key[8]) == 0x0D &&
           static_cast<uint8_t>(key[9]) == 0x01 &&
           static_cast<uint8_t>(key[10]) == 0x02 &&
           static_cast<uint8_t>(key[11]) == 0x01 &&
           static_cast<uint8_t>(key[12]) == 0x01 &&
           static_cast<uint8_t>(key[13]) >= 0x02 &&
           static_cast<uint8_t>(key[13]) <= 0x04;
}

// Primer Pack：06.0E.2B.34.02.05.01.01.0D.01.02.01.01.05.01.00
bool IsPrimerKey(const std::string& key) {
    return StartsWithMxfKey(key) && static_cast<uint8_t>(key[4]) == 0x02 &&
           static_cast<uint8_t>(key[5]) == 0x05 &&
           static_cast<uint8_t>(key[8]) == 0x0D &&
           static_cast<uint8_t>(key[9]) == 0x01 &&
           static_cast<uint8_t>(key[10]) == 0x02 &&
           static_cast<uint8_t>(key[11]) == 0x01 &&
           static_cast<uint8_t>(key[12]) == 0x01 &&
           static_cast<uint8_t>(key[13]) == 0x05;
}

// Index Table Segment：06.0E.2B.34.02.53.01.01.0D.01.02.01.01.05.01.00
bool IsIndexKey(const std::string& key) {
    return StartsWithMxfKey(key) && static_cast<uint8_t>(key[4]) == 0x02 &&
           static_cast<uint8_t>(key[5]) == 0x53 &&
           static_cast<uint8_t>(key[8]) == 0x0D &&
           static_cast<uint8_t>(key[9]) == 0x01 &&
           static_cast<uint8_t>(key[10]) == 0x02 &&
           static_cast<uint8_t>(key[11]) == 0x01;
}

// Essence Element：06.0E.2B.34.01.02.01.01.0D.01.03.01.<track>.<type>.<count>.<size>
bool IsEssenceKey(const std::string& key) {
    return StartsWithMxfKey(key) && static_cast<uint8_t>(key[4]) == 0x01 &&
           static_cast<uint8_t>(key[8]) == 0x0D &&
           static_cast<uint8_t>(key[9]) == 0x01 &&
           static_cast<uint8_t>(key[10]) == 0x03 &&
           static_cast<uint8_t>(key[11]) == 0x01;
}

const char* PartitionKindName(uint8_t kind) {
    switch (kind) {
    case 0x02: return "Header";
    case 0x03: return "Body";
    case 0x04: return "Footer";
    default: return "Unknown";
    }
}

const char* EssenceTypeName(uint8_t type) {
    switch (type) {
    case 0x01: return "video";
    case 0x02: return "audio";
    case 0x03: return "data";
    case 0x04: return "system";
    default: return "unknown";
    }
}

// 从 from 起往后找 SMPTE 键前缀 06 0E 2B 34，返回绝对偏移；找不到返回 -1。
int64_t FindKeyPrefix(SeqFileReader& file, int64_t from, int64_t end) {
    constexpr int64_t kChunk = 64 * 1024;
    while (from + 4 <= end) {
        file.Seek(from);
        const std::string w = file.Read(std::min(kChunk, end - from));
        if (w.size() < 4) return -1;
        const size_t idx = w.find(std::string("\x06\x0E\x2B\x34", 4));
        if (idx != std::string::npos) return from + static_cast<int64_t>(idx);
        if (static_cast<int64_t>(w.size()) < kChunk) return -1;
        from += static_cast<int64_t>(w.size()) - 3;  // 3 字节重叠，防止前缀跨块
    }
    return -1;
}

struct EssenceTrack {
    uint8_t track = 0;
    uint8_t element_type = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
};

}  // namespace

bool MxfStructureAnalyzer::Analyze(const std::string& file_path,
                                   model::ContainerStructureResult& result,
                                   const std::atomic<bool>* cancel,
                                   const MxfStructureOptions& options) {
    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        result.error_message = "无法打开文件";
        return false;
    }

    const int64_t file_size = file.Size();
    result.format = model::ContainerFormat::MXF;
    result.format_name = "MXF";
    result.file_path = file_path;

    const int64_t scan_end =
        options.max_scan_bytes > 0 ? std::min(file_size, options.max_scan_bytes) : file_size;

    model::ContainerElement root;
    root.name = "MXF (KLV)";
    root.type = "MXF";
    root.size = static_cast<uint64_t>(file_size);
    root.offset = 0;
    root.depth = 0;

    std::vector<model::ContainerElement> partition_nodes;
    std::map<uint8_t, EssenceTrack> tracks;
    std::string operational_pattern;
    uint64_t kag_size = 0;

    uint64_t objects = 0;
    uint64_t header_partitions = 0;
    uint64_t body_partitions = 0;
    uint64_t footer_partitions = 0;
    uint64_t primer_packs = 0;
    uint64_t index_segments = 0;
    uint64_t metadata_objects = 0;
    uint64_t essence_objects = 0;
    uint64_t resyncs = 0;
    uint64_t truncated_values = 0;

    int64_t pos = 0;
    while (pos + static_cast<int64_t>(kKeySize) + 1 <= scan_end) {
        if (infrastructure::Checkpoint(cancel)) {
            result.error_message = "已取消";
            return false;
        }
        if (options.max_objects != 0 && objects >= options.max_objects) break;

        file.Seek(pos);
        // 16 字节键 + 最多 8 字节 BER 长度
        const std::string head = file.Read(static_cast<int64_t>(kKeySize) + 8);
        if (head.size() < static_cast<size_t>(kKeySize) + 1) break;

        const std::string key = head.substr(0, kKeySize);
        if (!StartsWithMxfKey(key)) {
            ++resyncs;
            const int64_t found = FindKeyPrefix(file, pos + 1, scan_end);
            if (found < 0) break;
            pos = found;
            continue;
        }

        // BER 长度：短格式 < 0x80；长格式首字节低 7 位是后续字节数（MXF 禁止不定长 0x80）
        const uint8_t ber = static_cast<uint8_t>(head[kKeySize]);
        int len_bytes = 1;
        uint64_t value_len = 0;
        if (ber < 0x80) {
            value_len = ber;
        } else {
            const int n = ber & 0x7F;
            if (n == 0 || n > 8 || static_cast<size_t>(kKeySize + 1 + n) > head.size()) {
                // 长度字段本身坏了：当作 1 字节长度继续，避免整份文件放弃
                value_len = 0;
            } else {
                len_bytes = 1 + n;
                for (int k = 0; k < n; ++k) {
                    value_len = (value_len << 8) | static_cast<uint8_t>(head[kKeySize + 1 + k]);
                }
            }
        }

        const int64_t value_pos = pos + static_cast<int64_t>(kKeySize) + len_bytes;
        if (value_pos > scan_end || value_len > static_cast<uint64_t>(scan_end - value_pos)) {
            ++truncated_values;
            break;
        }

        ++objects;

        if (IsPartitionKey(key)) {
            const uint8_t kind = static_cast<uint8_t>(key[13]);
            if (kind == 0x02) ++header_partitions;
            else if (kind == 0x03) ++body_partitions;
            else ++footer_partitions;

            file.Seek(value_pos);
            const std::string value = file.Read(static_cast<int64_t>(std::min<uint64_t>(value_len, 128)));
            uint16_t major = 0, minor = 0;
            uint64_t this_offset = 0, prev_offset = 0, footer_offset = 0;
            if (value.size() >= 40) {
                major = MxfBE16(value, 0);
                minor = MxfBE16(value, 2);
                kag_size = MxfBE32(value, 4);
                this_offset = MxfBE64(value, 8);
                prev_offset = MxfBE64(value, 16);
                footer_offset = MxfBE64(value, 24);
            }
            if (value.size() >= 80 && operational_pattern.empty()) {
                operational_pattern = BytesToHex(value.substr(64, 16));
            }

            if (options.build_tree && partition_nodes.size() < options.max_tree_elements) {
                model::ContainerElement node;
                node.name = StrCat("%1 Partition", PartitionKindName(kind));
                node.type = "Partition Pack";
                node.offset = static_cast<uint64_t>(pos);
                node.size = static_cast<uint64_t>(kKeySize + len_bytes + value_len);
                node.depth = 1;
                node.value = StrCat("v%1.%2 this=%3 prev=%4 footer=%5", major, minor,
                                    this_offset, prev_offset, footer_offset);
                partition_nodes.push_back(node);
            }
            pos = value_pos + static_cast<int64_t>(value_len);
            continue;
        }

        if (IsPrimerKey(key)) {
            ++primer_packs;
            pos = value_pos + static_cast<int64_t>(value_len);
            continue;
        }

        if (IsIndexKey(key)) {
            ++index_segments;
            pos = value_pos + static_cast<int64_t>(value_len);
            continue;
        }

        if (IsEssenceKey(key)) {
            ++essence_objects;
            const uint8_t track_no = static_cast<uint8_t>(key[12]);
            const uint8_t element_type = static_cast<uint8_t>(key[13]);
            EssenceTrack& t = tracks[track_no];
            t.track = track_no;
            t.element_type = element_type;
            ++t.packets;
            t.bytes += value_len;
            pos = value_pos + static_cast<int64_t>(value_len);
            continue;
        }

        ++metadata_objects;
        pos = value_pos + static_cast<int64_t>(value_len);
    }

    // 一个分区都没有 = 不是 MXF（魔数可能只是碰巧命中）。交给 FFmpeg 通用分析。
    if (header_partitions + body_partitions + footer_partitions == 0) {
        result.error_message = "未找到 MXF Partition Pack";
        return false;
    }

    if (options.build_tree) {
        model::ContainerElement part_group;
        part_group.name = StrCat("Partitions (%1)", partition_nodes.size());
        part_group.type = "Group";
        part_group.depth = 1;
        part_group.children = partition_nodes;
        root.children.push_back(part_group);
    }

    model::ContainerElement track_group;
    track_group.name = StrCat("Essence Tracks (%1)", tracks.size());
    track_group.type = "Group";
    track_group.depth = 1;

    for (const auto& kv : tracks) {
        const EssenceTrack& t = kv.second;
        model::ContainerStreamInfo si;
        si.index = static_cast<int>(result.streams.size());
        si.type = EssenceTypeName(t.element_type);
        si.codec = "MXF essence";
        si.details = StrCat("track=%1 | KLV=%2 | %3 字节", t.track, t.packets, t.bytes);
        result.streams.push_back(si);

        if (options.build_tree && track_group.children.size() < options.max_tree_elements) {
            model::ContainerElement e;
            e.name = StrCat("Track %1", t.track);
            e.type = "Essence Track";
            e.depth = 2;
            e.value = StrCat("%1 | KLV=%2", EssenceTypeName(t.element_type), t.packets);
            track_group.children.push_back(e);
        }
    }
    if (options.build_tree) {
        if (!track_group.children.empty()) root.children.push_back(track_group);
        result.element_tree.push_back(root);
    }

    result.metadata["Partition"] =
        StrCat("%1 (H%2/B%3/F%4)", header_partitions + body_partitions + footer_partitions,
               header_partitions, body_partitions, footer_partitions);
    result.metadata["Primer Pack"] = std::to_string(primer_packs);
    result.metadata["Index Segment"] = std::to_string(index_segments);
    result.metadata["KLV 对象"] = std::to_string(objects);
    result.metadata["Essence KLV"] = std::to_string(essence_objects);
    result.metadata["元数据 KLV"] = std::to_string(metadata_objects);
    if (kag_size != 0) result.metadata["KAG"] = std::to_string(kag_size);
    if (!operational_pattern.empty()) result.metadata["Operational Pattern"] = operational_pattern;
    if (resyncs > 0) result.metadata["重同步次数"] = std::to_string(resyncs);
    if (truncated_values > 0) result.metadata["长度越界 KLV"] = std::to_string(truncated_values);
    result.metadata["扫描范围"] = StrCat("%1 / %2 字节", scan_end, file_size);

    result.valid = true;
    result.summary = StrCat("MXF | Partition H%1/B%2/F%3 | KLV=%4 | 轨道=%5%6",
                            header_partitions, body_partitions, footer_partitions, objects,
                            tracks.size(),
                            scan_end < file_size ? "（部分扫描）" : "");
    file.Close();
    return true;
}

}  // namespace videoeye
