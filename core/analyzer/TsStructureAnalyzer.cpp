#include "TsStructureAnalyzer.h"
#include <QFile>
#include <QByteArray>
#include <QMap>

namespace videoeye {
namespace analyzer {

static QString StreamTypeToName(uint8_t stream_type) {
    switch (stream_type) {
    case 0x01: return "MPEG-1 Video";
    case 0x02: return "MPEG-2 Video";
    case 0x03: return "MPEG-1 Audio";
    case 0x04: return "MPEG-2 Audio";
    case 0x0F: return "AAC Audio";
    case 0x10: return "MPEG-4 Video";
    case 0x1B: return "H.264 Video";
    case 0x20: return "H.265 Video";
    case 0x24: return "H.266 Video";
    case 0x81: return "AC-3 Audio";
    case 0x87: return "E-AC-3 Audio";
    case 0xA1: return "E-AC-3 Audio (ATSC)";
    case 0x82: return "DTS Audio";
    default:   return QString("StreamType 0x%1").arg(stream_type, 2, 16, QChar('0'));
    }
}

bool TsStructureAnalyzer::Analyze(const QString& file_path, model::ContainerStructureResult& result) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::MPEG_TS;
    result.format_name = "MPEG-TS";
    result.file_path = file_path;

    // 查找 sync byte
    QByteArray sync_search = file.read(1024);
    int sync_offset = -1;
    for (int i = 0; i < sync_search.size(); ++i) {
        if (static_cast<uint8_t>(sync_search[i]) == 0x47) {
            if (i + 188 < sync_search.size() &&
                static_cast<uint8_t>(sync_search[i + 188]) == 0x47) {
                sync_offset = i;
                break;
            }
        }
    }
    if (sync_offset < 0) {
        result.error_message = "未找到 TS 同步字节";
        return false;
    }

    file.seek(sync_offset);

    // 创建根元素
    model::ContainerElement root;
    root.name = "MPEG-TS Stream";
    root.type = "TS";
    root.size = file.size();
    root.offset = 0;
    root.depth = 0;

    // 扫描前 N 个包, 解析 PAT/PMT
    QMap<uint16_t, int> pid_counts;
    QMap<uint16_t, uint16_t> pat_programs;  // program_number -> PMT_PID
    QMap<uint16_t, QVector<QPair<uint8_t, QString>>> pmt_streams;  // PMT_PID -> streams
    int total_packets = 0;
    const int max_scan_packets = 5000;

    QByteArray pkt_buf(188, Qt::Uninitialized);
    while (total_packets < max_scan_packets && file.read(pkt_buf.data(), 188) == 188) {
        if (static_cast<uint8_t>(pkt_buf[0]) != 0x47) break;

        uint16_t pid = ((static_cast<uint8_t>(pkt_buf[1]) & 0x1F) << 8) |
                        static_cast<uint8_t>(pkt_buf[2]);
        pid_counts[pid]++;

        // PAT: PID 0
        if (pid == 0) {
            int payload_start = 4;
            if (pkt_buf[3] & 0x20) {  // adaptation field
                int adapt_len = static_cast<uint8_t>(pkt_buf[4]);
                payload_start = 5 + adapt_len;
            }
            if (payload_start < 188 && static_cast<uint8_t>(pkt_buf[payload_start]) == 0x00) {
                payload_start++;  // skip pointer
            }
            // table_id should be 0x00
            if (payload_start + 7 < 188) {
                uint16_t section_length = ((static_cast<uint8_t>(pkt_buf[payload_start + 1]) & 0x0F) << 8) |
                                           static_cast<uint8_t>(pkt_buf[payload_start + 2]);
                int pos = payload_start + 8;
                while (pos + 3 < payload_start + 3 + section_length && pos + 3 < 188) {
                    uint16_t program_num = (static_cast<uint8_t>(pkt_buf[pos]) << 8) |
                                            static_cast<uint8_t>(pkt_buf[pos + 1]);
                    uint16_t pmt_pid = ((static_cast<uint8_t>(pkt_buf[pos + 2]) & 0x1F) << 8) |
                                        static_cast<uint8_t>(pkt_buf[pos + 3]);
                    if (program_num != 0) {
                        pat_programs[program_num] = pmt_pid;
                    }
                    pos += 4;
                }
            }
        }

        // PMT
        if (pat_programs.values().contains(pid)) {
            int payload_start = 4;
            if (pkt_buf[3] & 0x20) {
                int adapt_len = static_cast<uint8_t>(pkt_buf[4]);
                payload_start = 5 + adapt_len;
            }
            if (payload_start < 188 && static_cast<uint8_t>(pkt_buf[payload_start]) == 0x00) {
                payload_start++;
            }
            if (payload_start + 11 < 188) {
                uint16_t section_length = ((static_cast<uint8_t>(pkt_buf[payload_start + 1]) & 0x0F) << 8) |
                                           static_cast<uint8_t>(pkt_buf[payload_start + 2]);
                uint16_t program_info_length = ((static_cast<uint8_t>(pkt_buf[payload_start + 10]) & 0x0F) << 8) |
                                                static_cast<uint8_t>(pkt_buf[payload_start + 11]);
                int pos = payload_start + 12 + program_info_length;
                QVector<QPair<uint8_t, QString>> streams;
                while (pos + 4 < payload_start + 3 + section_length && pos + 4 < 188) {
                    uint8_t stream_type = static_cast<uint8_t>(pkt_buf[pos]);
                    uint16_t elem_pid = ((static_cast<uint8_t>(pkt_buf[pos + 1]) & 0x1F) << 8) |
                                         static_cast<uint8_t>(pkt_buf[pos + 2]);
                    uint16_t es_info_length = ((static_cast<uint8_t>(pkt_buf[pos + 3]) & 0x0F) << 8) |
                                               static_cast<uint8_t>(pkt_buf[pos + 4]);
                    streams.append({stream_type, QString("PID %1 - %2").arg(elem_pid).arg(StreamTypeToName(stream_type))});
                    pos += 5 + es_info_length;
                }
                pmt_streams[pid] = streams;
            }
        }

        total_packets++;
    }

    // 构建结构树
    // PAT 信息
    for (auto it = pat_programs.begin(); it != pat_programs.end(); ++it) {
        model::ContainerElement prog;
        prog.name = QString("Program %1").arg(it.key());
        prog.type = "Program";
        prog.depth = 1;
        prog.value = QString("PMT PID=%1").arg(it.value());

        // PMT 流列表
        if (pmt_streams.contains(it.value())) {
            for (const auto& s : pmt_streams[it.value()]) {
                model::ContainerElement stream_elem;
                stream_elem.name = s.second;
                stream_elem.type = "Elementary Stream";
                stream_elem.depth = 2;

                prog.children.append(stream_elem);

                // 添加到流信息
                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                si.codec = StreamTypeToName(s.first);
                // 判断类型
                switch (s.first) {
                case 0x01: case 0x02: case 0x10: case 0x1B: case 0x20: case 0x24:
                    si.type = "video"; break;
                case 0x03: case 0x04: case 0x0F: case 0x81: case 0x87: case 0xA1: case 0x82:
                    si.type = "audio"; break;
                default:
                    si.type = "data"; break;
                }
                result.streams.append(si);
            }
        }
        root.children.append(prog);
    }

    // PID 分布统计
    model::ContainerElement pid_stats;
    pid_stats.name = "PID Distribution";
    pid_stats.type = "Statistics";
    pid_stats.depth = 1;
    pid_stats.value = QString("scanned %1 packets, %2 unique PIDs").arg(total_packets).arg(pid_counts.size());

    int shown = 0;
    for (auto it = pid_counts.begin(); it != pid_counts.end() && shown < 20; ++it, ++shown) {
        model::ContainerElement pid_elem;
        pid_elem.name = QString("PID %1").arg(it.key());
        pid_elem.type = "PID";
        pid_elem.depth = 2;
        pid_elem.value = QString("%1 packets").arg(it.value());
        pid_stats.children.append(pid_elem);
    }
    root.children.append(pid_stats);

    result.element_tree.append(root);
    result.valid = true;
    result.summary = QString("MPEG-TS | %1 包/已扫描 | %2 节目 | %3 流")
                         .arg(total_packets).arg(pat_programs.size()).arg(result.streams.size());

    file.close();
    return true;
}

} // namespace analyzer
} // namespace videoeye
