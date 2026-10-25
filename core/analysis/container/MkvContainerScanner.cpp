#include "core/analysis/container/MkvContainerScanner.h"

#include "core/analysis/detail/SeqFileReader.h"
#include "infrastructure/concurrency/Cancellation.h"
#include "infrastructure/logging/Logger.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace videoeye {

namespace {

// ---- EBML 元素 ID（保留标记位，与 EbmlAnalyzer::ElementName 的键一致）----
constexpr uint64_t kIdEbmlHeader = 0x1A45DFA3;
constexpr uint64_t kIdDocType = 0x4282;
constexpr uint64_t kIdSegment = 0x18538067;
constexpr uint64_t kIdSeekHead = 0x114D9B74;
constexpr uint64_t kIdInfo = 0x1549A966;
constexpr uint64_t kIdTimestampScale = 0x2AD7B1;
constexpr uint64_t kIdTracks = 0x1654AE6B;
constexpr uint64_t kIdTrackEntry = 0xAE;
constexpr uint64_t kIdTrackType = 0x83;
constexpr uint64_t kIdCodecId = 0x86;
constexpr uint64_t kIdCodecPrivate = 0x63A2;
constexpr uint64_t kIdCluster = 0x1F43B675;
constexpr uint64_t kIdClusterTimecode = 0xE7;
constexpr uint64_t kIdCues = 0x1C53BB6B;
constexpr uint64_t kIdTags = 0x1254C367;
constexpr uint64_t kIdChapters = 0x1043A770;
constexpr uint64_t kIdAttachments = 0x1941A469;

// Segment 的一级子元素。unknown-size Cluster 没有长度字段，只能靠"下一个
// 一级元素"来收尾，所以这张名单是解析边界的唯一依据。
bool IsLevel1Id(uint64_t id) {
    switch (id) {
    case kIdSeekHead:
    case kIdInfo:
    case kIdTracks:
    case kIdCluster:
    case kIdCues:
    case kIdTags:
    case kIdChapters:
    case kIdAttachments:
        return true;
    default:
        return false;
    }
}

struct ElementHeader {
    uint64_t id = 0;
    int64_t payload = 0; // 载荷起始偏移
    uint64_t size = 0;   // 载荷长度（unknown_size 时为 0）
    bool unknown_size = false;
    bool ok = false;
};

std::string ReadAt(SeqFileReader& file, int64_t pos, int64_t n) {
    if (n <= 0)
        return {};
    if (!file.Seek(pos))
        return {};
    return file.Read(n);
}

// 一次只读 12 字节：EBML ID 最长 4 字节 + size VINT 最长 8 字节。
// 不缓存整块是刻意的 —— 顺序扫描的每个元素都要跳过成千上万字节载荷，
// 预读窗口在跨越 Cluster 时全是浪费。
ElementHeader ReadHeader(SeqFileReader& file, int64_t pos, int64_t limit) {
    ElementHeader h;
    if (pos < 0 || pos + 2 > limit)
        return h;

    const int64_t avail = std::min<int64_t>(limit - pos, 12);
    const std::string buf = ReadAt(file, pos, avail);
    if (buf.size() < 2)
        return h;

    // ---- ID：1~4 字节，保留标记位 ----
    const uint8_t first = static_cast<uint8_t>(buf[0]);
    if (first == 0)
        return h; // 推不出宽度
    int id_width = 1;
    uint8_t mask = 0x80;
    while (mask != 0 && (first & mask) == 0) {
        ++id_width;
        mask = static_cast<uint8_t>(mask >> 1);
    }
    if (id_width > 4 || static_cast<size_t>(id_width) >= buf.size())
        return h;
    uint64_t id = first;
    for (int i = 1; i < id_width; ++i) {
        id = (id << 8) | static_cast<uint8_t>(buf[static_cast<size_t>(i)]);
    }

    // ---- size：1~8 字节 VINT，值全 1 = 长度未知 ----
    size_t off = static_cast<size_t>(id_width);
    const uint8_t s0 = static_cast<uint8_t>(buf[off]);
    if (s0 == 0)
        return h;
    int s_width = 1;
    mask = 0x80;
    while (mask != 0 && (s0 & mask) == 0) {
        ++s_width;
        mask = static_cast<uint8_t>(mask >> 1);
    }
    if (s_width > 8 || off + static_cast<size_t>(s_width) > buf.size())
        return h;

    uint64_t size = s0 & (0xFF >> s_width);
    for (int i = 1; i < s_width; ++i) {
        size = (size << 8) | static_cast<uint8_t>(buf[off + static_cast<size_t>(i)]);
    }
    // 全 1 即"未知长度"。8 字节 VINT 的载荷位是 56 位，1ULL<<56-1 正好是全 1，
    // 与宽度无关地统一处理（EbmlAnalyzer 里那段注释的同一个坑）。
    const uint64_t unknown_size = (1ULL << (7 * s_width)) - 1;

    h.id = id;
    h.payload = pos + static_cast<int64_t>(id_width + s_width);
    h.unknown_size = (size == unknown_size);
    h.size = h.unknown_size ? 0 : size;
    h.ok = true;
    return h;
}

uint64_t ReadBeUInt(SeqFileReader& file, int64_t pos, uint64_t size) {
    if (size == 0 || size > 8)
        return 0;
    const std::string bytes = ReadAt(file, pos, static_cast<int64_t>(size));
    uint64_t value = 0;
    for (const char c : bytes) {
        value = (value << 8) | static_cast<uint8_t>(c);
    }
    return value;
}

std::string ReadShortString(SeqFileReader& file, int64_t pos, uint64_t size) {
    constexpr uint64_t kCap = 256; // CodecID / DocType 都是短串
    if (size == 0)
        return {};
    const std::string bytes = ReadAt(file, pos, static_cast<int64_t>(std::min<uint64_t>(size, kCap)));
    std::string out;
    for (const char c : bytes) {
        if (c == '\0')
            break;
        out += c;
    }
    return out;
}

// 缺了 CodecPrivate 就一定解不出来的编解码。
// 这里刻意收窄（宁可漏报不误报）：
//   * AVC / HEVC：avcC / hvcC 里带 SPS/PPS，Matroska 不允许靠带内参数集补；
//   * AAC：AudioSpecificConfig 决定 object type / 采样率 / 声道；
//   * Vorbis：需要三段头（识别头 + 注释 + 码本）。
// 不纳入：VP8/VP9（无 CodecPrivate 概念）、AV1（av1C 缺失时仍可从帧内同步头解）、
// Opus（缺 OpusHead 时解码器可按默认 48k/立体声兜底）、MP3/AC-3 等纯帧同步编码。
bool RequiresCodecPrivate(const std::string& codec_id) {
    if (codec_id == "V_MPEG4/ISO/AVC" || codec_id == "V_MPEGH/ISO/HEVC")
        return true;
    if (codec_id.rfind("A_AAC", 0) == 0)
        return true;
    if (codec_id == "A_VORBIS")
        return true;
    return false;
}

// 单位：Timecode 以 TimestampScale 为刻度（默认 1 ms）。
double TicksToSeconds(int64_t ticks, uint64_t timestamp_scale) {
    const double ns_per_tick = (timestamp_scale == 0) ? 1000000.0 : static_cast<double>(timestamp_scale);
    return static_cast<double>(ticks) * ns_per_tick / 1e9;
}

// Info 段：只关心 TimestampScale（Cluster Timecode 的单位由它决定）
void ScanInfo(SeqFileReader& file, int64_t begin, int64_t end, uint64_t& timestamp_scale) {
    int64_t pos = begin;
    while (pos + 2 <= end) {
        const ElementHeader h = ReadHeader(file, pos, end);
        if (!h.ok || h.unknown_size)
            return;
        if (h.id == kIdTimestampScale) {
            const uint64_t value = ReadBeUInt(file, h.payload, h.size);
            if (value > 0)
                timestamp_scale = value;
            return;
        }
        pos = h.payload + static_cast<int64_t>(h.size);
    }
}

void ScanTrackEntry(SeqFileReader& file, int64_t begin, int64_t end, model::MkvDiagnostics& diag) {
    int track_type = 0;
    bool has_codec_private = false;
    std::string codec_id;

    int64_t pos = begin;
    while (pos + 2 <= end) {
        const ElementHeader h = ReadHeader(file, pos, end);
        if (!h.ok || h.unknown_size)
            break;
        if (h.id == kIdTrackType) {
            track_type = static_cast<int>(ReadBeUInt(file, h.payload, h.size));
        } else if (h.id == kIdCodecId) {
            codec_id = ReadShortString(file, h.payload, h.size);
        } else if (h.id == kIdCodecPrivate) {
            // 只判有无，不读载荷（avcC 几百字节、Vorbis 码本能到几十 KB）
            has_codec_private = h.size > 0;
        }
        pos = h.payload + static_cast<int64_t>(h.size);
    }

    if (track_type == 1) {
        ++diag.video_tracks;
    } else if (track_type == 2) {
        ++diag.audio_tracks;
    }

    if (RequiresCodecPrivate(codec_id) && !has_codec_private) {
        ++diag.tracks_missing_codec_private;
        if (diag.missing_codec_private_codecs.empty()) {
            diag.missing_codec_private_codecs = codec_id;
        } else if (diag.missing_codec_private_codecs.find(codec_id) == std::string::npos) {
            diag.missing_codec_private_codecs += ", " + codec_id;
        }
    }
}

void ScanTracks(SeqFileReader& file, int64_t begin, int64_t end, model::MkvDiagnostics& diag) {
    int64_t pos = begin;
    while (pos + 2 <= end) {
        const ElementHeader h = ReadHeader(file, pos, end);
        if (!h.ok || h.unknown_size)
            return;
        if (h.id == kIdTrackEntry) {
            ScanTrackEntry(file, h.payload, std::min<int64_t>(end, h.payload + static_cast<int64_t>(h.size)), diag);
        }
        pos = h.payload + static_cast<int64_t>(h.size);
    }
}

} // namespace

bool MkvContainerScanner::Analyze(const std::string& file_path, model::MkvDiagnostics& diagnostics,
                                  const std::atomic<bool>* cancel, const MkvScanOptions& options) {
    diagnostics = model::MkvDiagnostics{};

    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        LOG_WARN("MKV 体检：无法打开文件 " + file_path);
        return false;
    }
    const int64_t file_size = file.Size();
    if (file_size < 8)
        return false;

    const std::string magic = ReadAt(file, 0, 4);
    if (magic.size() < 4 || static_cast<uint8_t>(magic[0]) != 0x1A || static_cast<uint8_t>(magic[1]) != 0x45 ||
        static_cast<uint8_t>(magic[2]) != 0xDF || static_cast<uint8_t>(magic[3]) != 0xA3) {
        return false;
    }

    // ---- 顶层：EBML 头（取 DocType）+ Segment 边界 ----
    int64_t segment_begin = -1;
    int64_t segment_end = file_size;
    int64_t pos = 0;
    while (pos + 2 <= file_size) {
        if (infrastructure::Checkpoint(cancel))
            return false;
        const ElementHeader h = ReadHeader(file, pos, file_size);
        if (!h.ok)
            break;

        if (h.id == kIdEbmlHeader) {
            const int64_t header_end =
                h.unknown_size ? file_size : std::min<int64_t>(file_size, h.payload + static_cast<int64_t>(h.size));
            int64_t child = h.payload;
            while (child + 2 <= header_end) {
                const ElementHeader c = ReadHeader(file, child, header_end);
                if (!c.ok || c.unknown_size)
                    break;
                if (c.id == kIdDocType) {
                    diagnostics.doc_type = ReadShortString(file, c.payload, c.size);
                    break;
                }
                child = c.payload + static_cast<int64_t>(c.size);
            }
            if (h.unknown_size)
                break;
            pos = h.payload + static_cast<int64_t>(h.size);
            continue;
        }

        if (h.id == kIdSegment) {
            segment_begin = h.payload;
            segment_end =
                h.unknown_size ? file_size : std::min<int64_t>(file_size, h.payload + static_cast<int64_t>(h.size));
            if (h.unknown_size)
                ++diagnostics.unknown_size_elements;
            break;
        }

        if (h.unknown_size)
            break;
        pos = h.payload + static_cast<int64_t>(h.size);
    }
    if (segment_begin < 0 || segment_end <= segment_begin)
        return false;

    // ---- Segment 的一级元素顺序扫描 ----
    uint64_t elements = 0;
    bool budget_hit = false;
    uint64_t timestamp_scale = 1000000; // 默认 1 ms
    int64_t prev_cluster_ticks = 0;
    bool have_prev_cluster = false;

    pos = segment_begin;
    while (pos + 2 <= segment_end) {
        if (infrastructure::Checkpoint(cancel))
            return false;
        if (++elements > options.max_elements) {
            budget_hit = true;
            break;
        }

        const ElementHeader h = ReadHeader(file, pos, segment_end);
        if (!h.ok) {
            diagnostics.truncated = true;
            break;
        }
        if (h.unknown_size)
            ++diagnostics.unknown_size_elements;

        const int64_t payload_end =
            h.unknown_size ? segment_end : std::min<int64_t>(segment_end, h.payload + static_cast<int64_t>(h.size));

        if (h.id == kIdCluster) {
            ++diagnostics.clusters;
            // Cluster 的第一个子元素几乎总是 Timecode；读到就够，其余载荷（Block）整体跳过。
            // unknown-size Cluster 没有长度，只能顺着子元素往前挪，直到撞上下一个一级元素。
            int64_t next_pos = payload_end;
            int64_t child_pos = h.payload;
            int64_t cluster_ticks = 0;
            bool got_timecode = false;
            while (child_pos + 2 <= (h.unknown_size ? segment_end : payload_end)) {
                if (++elements > options.max_elements) {
                    budget_hit = true;
                    break;
                }
                const int64_t child_limit = h.unknown_size ? segment_end : payload_end;
                const ElementHeader child = ReadHeader(file, child_pos, child_limit);
                if (!child.ok) {
                    diagnostics.truncated = true;
                    next_pos = child_limit;
                    break;
                }
                if (h.unknown_size && IsLevel1Id(child.id)) {
                    next_pos = child_pos; // 本 Cluster 到此结束，下一个一级元素从这儿开始
                    break;
                }
                if (child.unknown_size) {
                    // 子元素也未知长度：边界已不可靠，本 Cluster 后面不再解析
                    ++diagnostics.unknown_size_elements;
                    next_pos = h.unknown_size ? segment_end : payload_end;
                    break;
                }
                if (child.id == kIdClusterTimecode) {
                    cluster_ticks = static_cast<int64_t>(ReadBeUInt(file, child.payload, child.size));
                    got_timecode = true;
                    if (!h.unknown_size)
                        break; // 已知长度：载荷整体跳过，无需再往下走
                }
                child_pos = child.payload + static_cast<int64_t>(child.size);
                if (!h.unknown_size)
                    next_pos = payload_end;
            }
            // 预算用完时不要继续累加统计，否则会把"没扫完"当成"没有回退"
            if (budget_hit)
                break;

            if (got_timecode) {
                ++diagnostics.clusters_with_timecode;
                if (have_prev_cluster && cluster_ticks < prev_cluster_ticks)
                    ++diagnostics.cluster_timestamp_backwards;
                prev_cluster_ticks = cluster_ticks;
                have_prev_cluster = true;
                diagnostics.last_cluster_seconds = TicksToSeconds(cluster_ticks, timestamp_scale);
            }
            pos = (next_pos > pos) ? next_pos : payload_end;
            continue;
        }

        switch (h.id) {
        case kIdSeekHead:
            diagnostics.has_seek_head = true;
            break;
        case kIdCues:
            diagnostics.has_cues = true;
            break;
        case kIdTags:
            diagnostics.has_tags = true;
            break;
        case kIdChapters:
            diagnostics.has_chapters = true;
            break;
        case kIdInfo:
            if (!h.unknown_size)
                ScanInfo(file, h.payload, payload_end, timestamp_scale);
            break;
        case kIdTracks:
            if (!h.unknown_size)
                ScanTracks(file, h.payload, payload_end, diagnostics);
            break;
        default:
            break;
        }

        pos = (payload_end > pos) ? payload_end : pos + 2;
    }

    diagnostics.scan_complete = !budget_hit;
    diagnostics.analyzed = true;
    LOG_INFO("MKV 体检完成: " + diagnostics.doc_type + " | " + std::to_string(diagnostics.clusters) + " Cluster | " +
             std::to_string(diagnostics.cluster_timestamp_backwards) + " 处时间戳回退" +
             (diagnostics.has_cues ? " | 有 Cues" : " | 无 Cues"));
    return true;
}

} // namespace videoeye
