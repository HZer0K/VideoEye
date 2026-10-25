#include "core/analysis/container/MpegPsStructureAnalyzer.h"

#include "core/analysis/detail/AnalysisTextUtil.h"
#include "core/analysis/detail/SeqFileReader.h"
#include "infrastructure/concurrency/Cancellation.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace videoeye {

namespace {

uint16_t PsBE16(const std::string& d, size_t off) {
    if (off + 2 > d.size())
        return 0;
    return static_cast<uint16_t>((static_cast<uint8_t>(d[off]) << 8) | static_cast<uint8_t>(d[off + 1]));
}

uint32_t PsBE32(const std::string& d, size_t off) {
    if (off + 4 > d.size())
        return 0;
    return (static_cast<uint32_t>(static_cast<uint8_t>(d[off])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3]));
}

// MPEG-2 PES 头里的 33 位 PTS（5 字节，标记位夹在中间）。
uint64_t PsPts(const std::string& d, size_t off) {
    if (off + 5 > d.size())
        return 0;
    const uint64_t b0 = static_cast<uint8_t>(d[off]);
    const uint64_t b1 = static_cast<uint8_t>(d[off + 1]);
    const uint64_t b2 = static_cast<uint8_t>(d[off + 2]);
    const uint64_t b3 = static_cast<uint8_t>(d[off + 3]);
    const uint64_t b4 = static_cast<uint8_t>(d[off + 4]);
    return ((b0 >> 1) & 0x07) << 30 | (b1 << 22) | ((b2 >> 1) << 15) | (b3 << 7) | (b4 >> 1);
}

// PSM / PMT 的 stream_type → 编码名。只列 PS 里真正会出现的那些；
// 返回空串表示"认识类型但叫不出名字"，调用方回退成 stream_type=0xNN。
std::string PsStreamTypeCodec(uint8_t t) {
    switch (t) {
    case 0x01:
        return "MPEG-1 Video";
    case 0x02:
        return "MPEG-2 Video";
    case 0x03:
        return "MPEG-1 Audio";
    case 0x04:
        return "MPEG-2 Audio";
    case 0x0F:
        return "AAC (ADTS)";
    case 0x10:
        return "MPEG-4 Visual";
    case 0x11:
        return "AAC (LATM)";
    case 0x1B:
        return "H.264 (AVC)";
    case 0x1C:
        return "MPEG-4 Audio";
    case 0x24:
        return "H.265 (HEVC)";
    case 0x42:
        return "AVS";
    case 0x81:
        return "AC-3";
    case 0x82:
        return "DTS";
    case 0x83:
        return "TrueHD";
    case 0x84:
        return "E-AC-3";
    case 0x85:
    case 0x86:
        return "DTS-HD";
    case 0x87:
        return "E-AC-3";
    case 0x8A:
        return "DTS";
    case 0xEA:
        return "VC-1";
    default:
        return std::string();
    }
}

bool PsIsVideoType(uint8_t t) {
    return t == 0x01 || t == 0x02 || t == 0x10 || t == 0x1B || t == 0x24 || t == 0x42 || t == 0xEA;
}

bool PsIsAudioType(uint8_t t) {
    return t == 0x03 || t == 0x04 || t == 0x0F || t == 0x11 || t == 0x1C || (t >= 0x81 && t <= 0x8A);
}

// private_stream_1 的子流号（VOB 的 AC-3 / DTS / LPCM 都走这里）。
std::string PsPrivateSubstreamCodec(uint8_t sub) {
    if (sub >= 0x80 && sub <= 0x87)
        return "AC-3";
    if (sub >= 0x88 && sub <= 0x8F)
        return "DTS";
    if (sub >= 0xA0 && sub <= 0xA7)
        return "LPCM";
    if (sub >= 0xB0 && sub <= 0xBF)
        return "TrueHD/MLP";
    return std::string();
}

struct PsStream {
    uint8_t stream_id = 0;
    std::string type;
    std::string codec;
    uint64_t packets = 0;
    uint64_t payload_bytes = 0;
    bool pts_seen = false;
    uint64_t first_pts = 0;
    uint64_t last_pts = 0;
};

} // namespace

bool MpegPsStructureAnalyzer::Analyze(const std::string& file_path, model::ContainerStructureResult& result,
                                      const std::atomic<bool>* cancel, const MpegPsStructureOptions& options) {
    SeqFileReader file(file_path);
    if (!file.IsOpen()) {
        result.error_message = "无法打开文件";
        return false;
    }

    const int64_t file_size = file.Size();
    result.format = model::ContainerFormat::MPEG_PS;
    result.format_name = "MPEG-PS";
    result.file_path = file_path;

    const int64_t scan_end = options.max_scan_bytes > 0 ? std::min(file_size, options.max_scan_bytes) : file_size;

    model::ContainerElement root;
    root.name = "MPEG-PS Program Stream";
    root.type = "MPEG-PS";
    root.size = static_cast<uint64_t>(file_size);
    root.offset = 0;
    root.depth = 0;

    std::map<uint8_t, PsStream> streams;
    std::vector<model::ContainerElement> pack_nodes;
    std::vector<model::ContainerElement> system_nodes;
    std::vector<std::string> psm_lines;

    uint64_t pack_headers = 0;
    uint64_t pes_packets = 0;
    uint64_t system_headers = 0;
    uint64_t psm_count = 0;
    uint64_t padding_packets = 0;
    uint64_t end_codes = 0;
    uint64_t truncated = 0;
    bool saw_end_code = false;

    constexpr int64_t kChunk = 64 * 1024;
    int64_t pos = 0;
    while (pos + 4 <= scan_end) {
        if (infrastructure::Checkpoint(cancel)) {
            result.error_message = "已取消";
            return false;
        }

        file.Seek(pos);
        const std::string w = file.Read(std::min<int64_t>(kChunk, scan_end - pos));
        if (w.size() < 4)
            break;

        size_t i = 0;
        while (i + 4 <= w.size()) {
            if (!(w[i] == '\0' && w[i + 1] == '\0' && w[i + 2] == '\x01')) {
                ++i;
                continue;
            }
            const int64_t code_pos = pos + static_cast<int64_t>(i);
            const uint8_t code = static_cast<uint8_t>(w[i + 3]);

            if (code == 0xBA) { // Pack Header
                if (i + 14 > w.size())
                    break; // 头跨块，下一轮从起始码重新读
                int64_t consumed = 4;
                bool mpeg2 = false;
                if ((static_cast<uint8_t>(w[i + 4]) & 0xC0) == 0x40) {
                    // MPEG-2: 14 字节固定头 + (第 13 字节低 3 位) 字节 stuffing
                    consumed = 14 + (static_cast<uint8_t>(w[i + 13]) & 0x07);
                    mpeg2 = true;
                } else if ((static_cast<uint8_t>(w[i + 4]) & 0xF0) == 0x20) {
                    consumed = 12; // MPEG-1
                }
                ++pack_headers;
                if (options.build_tree && pack_nodes.size() < std::min<uint32_t>(options.max_tree_elements, 50)) {
                    model::ContainerElement node;
                    node.name = StrCat("Pack Header #%1", pack_headers);
                    node.type = "Pack Header";
                    node.offset = static_cast<uint64_t>(code_pos);
                    node.size = static_cast<uint64_t>(consumed);
                    node.depth = 1;
                    node.value = mpeg2 ? StrCat("MPEG-2 stuffing=%1B", consumed - 14) : std::string("MPEG-1");
                    pack_nodes.push_back(node);
                }
                i += static_cast<size_t>(consumed);
                continue;
            }

            if (code == 0xB9) { // Program End Code
                ++end_codes;
                saw_end_code = true;
                i += 4;
                continue;
            }

            // 其余起始码都带 2 字节长度
            if (i + 6 > w.size())
                break;
            const uint16_t payload_len = PsBE16(w, i + 4);
            const int64_t consumed = 6 + static_cast<int64_t>(payload_len);
            if (code_pos + consumed > scan_end) {
                ++truncated;
                i += w.size() - i;
                continue;
            }

            if (code == 0xBB) { // System Header
                ++system_headers;
                if (options.build_tree && system_nodes.size() < 20) {
                    model::ContainerElement node;
                    node.name = StrCat("System Header #%1", system_headers);
                    node.type = "System Header";
                    node.offset = static_cast<uint64_t>(code_pos);
                    node.size = static_cast<uint64_t>(consumed);
                    node.depth = 1;
                    if (payload_len >= 6 && i + 12 <= w.size()) {
                        node.value =
                            StrCat("rate_bound=%1 streams=%2", PsBE32(w, i + 6) >> 1, static_cast<uint8_t>(w[i + 11]));
                    }
                    system_nodes.push_back(node);
                }
                i += static_cast<size_t>(consumed);
                continue;
            }

            if (code == 0xBC) { // Program Stream Map
                ++psm_count;
                // PSM: current_next/version(1) + reserved/marker(2) +
                //      program_stream_info_length(2) + info +
                //      elementary_stream_map_length(2) + 映射项
                const size_t base = i + 6;
                size_t p = base + 3;
                if (p + 2 <= w.size()) {
                    const uint16_t prog_info_len = PsBE16(w, p);
                    p += 2 + prog_info_len;
                }
                if (p + 2 <= w.size()) {
                    const uint16_t map_len = PsBE16(w, p);
                    p += 2;
                    const size_t map_end = std::min(p + map_len, i + 6 + payload_len);
                    while (p + 4 <= map_end && p + 4 <= w.size()) {
                        const uint8_t stream_type = static_cast<uint8_t>(w[p]);
                        const uint8_t elementary_id = static_cast<uint8_t>(w[p + 1]);
                        const uint16_t es_info_len = PsBE16(w, p + 2);
                        PsStream& s = streams[elementary_id];
                        s.stream_id = elementary_id;
                        const std::string codec = PsStreamTypeCodec(stream_type);
                        s.codec = codec.empty() ? StrCat("stream_type=0x%1", HexFill(stream_type, 2)) : codec;
                        s.type = PsIsVideoType(stream_type) ? "video" : (PsIsAudioType(stream_type) ? "audio" : "data");
                        if (psm_lines.size() < 32) {
                            psm_lines.push_back(StrCat("0x%1 → %2", HexFill(elementary_id, 2), s.codec));
                        }
                        p += 4 + es_info_len;
                    }
                }
                i += static_cast<size_t>(consumed);
                continue;
            }

            if (code >= 0xBD && code <= 0xEF) { // PES（含 private_stream_1）
                ++pes_packets;
                PsStream& s = streams[code];
                s.stream_id = code;
                ++s.packets;
                s.payload_bytes += payload_len;

                if (s.codec.empty()) {
                    if (code == 0xBD) {
                        // private_stream_1：载荷首字节是子流号
                        const size_t sub_off = i + 6;
                        const uint8_t sub = (sub_off < w.size()) ? static_cast<uint8_t>(w[sub_off]) : 0;
                        const std::string sub_codec = PsPrivateSubstreamCodec(sub);
                        s.codec = sub_codec.empty() ? std::string("private_stream_1") : sub_codec;
                        s.type = "audio";
                    } else if (code >= 0xE0) {
                        s.codec = "MPEG Video";
                        s.type = "video";
                    } else if (code >= 0xC0) {
                        s.codec = "MPEG Audio";
                        s.type = "audio";
                    } else {
                        s.codec = "PES";
                        s.type = "data";
                    }
                }

                // 首末 PTS（MPEG-2 PES：'10' 标志 + 可选 PTS 标志）
                if (payload_len >= 8 && i + 14 <= w.size()) {
                    const size_t pes = i + 6;
                    if ((static_cast<uint8_t>(w[pes]) & 0xC0) == 0x80 && (static_cast<uint8_t>(w[pes + 1]) & 0x80) &&
                        static_cast<uint8_t>(w[pes + 2]) >= 5) {
                        const uint64_t pts = PsPts(w, pes + 3);
                        if (!s.pts_seen) {
                            s.first_pts = pts;
                            s.pts_seen = true;
                        }
                        s.last_pts = pts;
                    }
                }
                if (payload_len == 0) {
                    // 未定长 PES（常见于视频）：无法按长度跳，只能继续找下一个起始码
                    i += 6;
                } else {
                    i += static_cast<size_t>(consumed);
                }
                continue;
            }

            if (code == 0xBE) { // Padding
                ++padding_packets;
                i += static_cast<size_t>(consumed);
                continue;
            }

            // 序列头 / GOP / 图像头等基本流起始码：没有容器长度可跳，前进 4 字节继续找
            i += 4;
        }

        // i 是"已处理到的块内偏移"；跨块元素会在上面 break，此时从起始码重新读。
        pos += (i == 0 ? int64_t{4} : static_cast<int64_t>(i));
    }

    // 一个 Pack Header 都没找到 = 不是 PS（扩展名叫 .mpg 但内容是裸 MPEG-1 视频流
    // 很常见）。返回失败让上层回退 FFmpeg 通用分析，而不是给出一份空壳结构。
    if (pack_headers == 0 && psm_count == 0 && pes_packets == 0) {
        result.error_message = "未找到 MPEG-PS Pack Header";
        return false;
    }

    // 流表落地
    if (options.build_tree) {
        model::ContainerElement sys_group;
        sys_group.name = StrCat("System Headers (%1)", system_headers);
        sys_group.type = "Group";
        sys_group.depth = 1;
        sys_group.children = system_nodes;
        if (!system_nodes.empty())
            root.children.push_back(sys_group);

        model::ContainerElement psm_group;
        psm_group.name = StrCat("Program Stream Map (%1)", psm_count);
        psm_group.type = "Group";
        psm_group.depth = 1;
        for (const std::string& line : psm_lines) {
            model::ContainerElement e;
            e.name = line;
            e.type = "Stream Mapping";
            e.depth = 2;
            psm_group.children.push_back(e);
        }
        if (psm_count > 0)
            root.children.push_back(psm_group);

        model::ContainerElement pack_group;
        pack_group.name = StrCat("Pack Headers (%1)", pack_headers);
        pack_group.type = "Group";
        pack_group.depth = 1;
        pack_group.children = pack_nodes;
        if (!pack_nodes.empty())
            root.children.push_back(pack_group);
    }

    model::ContainerElement streams_group;
    streams_group.name = StrCat("Streams (%1)", streams.size());
    streams_group.type = "Group";
    streams_group.depth = 1;

    for (const auto& kv : streams) {
        const PsStream& s = kv.second;
        model::ContainerStreamInfo si;
        si.index = static_cast<int>(result.streams.size());
        si.type = s.type;
        si.codec = s.codec;
        std::vector<std::string> details;
        details.push_back(StrCat("stream_id=0x%1", HexFill(s.stream_id, 2)));
        details.push_back(StrCat("PES=%1", s.packets));
        if (s.pts_seen) {
            details.push_back(StrCat("PTS=%1→%2", s.first_pts, s.last_pts));
        }
        si.details = StrCat("%1 %2 %3", details[0], details[1], s.pts_seen ? details[2] : std::string());
        result.streams.push_back(si);

        if (options.build_tree) {
            model::ContainerElement e;
            e.name = StrCat("Stream 0x%1", HexFill(s.stream_id, 2));
            e.type = "Stream";
            e.depth = 2;
            e.value = StrCat("%1 | PES=%2%3", s.codec, s.packets,
                             s.pts_seen ? StrCat(" | PTS %1→%2", s.first_pts, s.last_pts) : std::string());
            streams_group.children.push_back(e);
        }
    }
    if (options.build_tree) {
        if (!streams_group.children.empty())
            root.children.push_back(streams_group);
        result.element_tree.push_back(root);
    }

    result.metadata["Pack Header"] = std::to_string(pack_headers);
    result.metadata["PES 包"] = std::to_string(pes_packets);
    result.metadata["System Header"] = std::to_string(system_headers);
    result.metadata["PSM"] = std::to_string(psm_count);
    result.metadata["Padding"] = std::to_string(padding_packets);
    if (truncated > 0)
        result.metadata["截断的起始码"] = std::to_string(truncated);
    if (saw_end_code)
        result.metadata["Program End Code"] = "有";
    result.metadata["扫描范围"] = StrCat("%1 / %2 字节", scan_end, file_size);

    result.valid = true;
    result.summary = StrCat("MPEG-PS | Pack=%1 | PES=%2 | 流=%3%4", pack_headers, pes_packets, streams.size(),
                            scan_end < file_size ? "（部分扫描）" : "");
    file.Close();
    return true;
}

} // namespace videoeye
