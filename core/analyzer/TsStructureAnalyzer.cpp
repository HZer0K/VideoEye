#include "TsStructureAnalyzer.h"
#include <QFile>
#include <QByteArray>
#include <QMap>
#include <QVector>

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

// PES stream_id → 可读名
static QString StreamIdName(uint8_t sid) {
    if (sid >= 0xC0 && sid <= 0xDF) return QString("Audio (0x%1)").arg(sid, 2, 16, QChar('0'));
    if (sid >= 0xE0 && sid <= 0xEF) return QString("Video (0x%1)").arg(sid, 2, 16, QChar('0'));
    switch (sid) {
    case 0xBC: return "Program Stream Map";
    case 0xBD: return "Private Stream 1";
    case 0xBE: return "Padding Stream";
    case 0xBF: return "Private Stream 2";
    case 0xF0: return "ECM";
    case 0xF1: return "EMM";
    case 0xFD: return "Extended Stream";
    default:   return QString("stream_id 0x%1").arg(sid, 2, 16, QChar('0'));
    }
}

namespace {
struct EsInfo { uint8_t stream_type = 0; uint16_t elem_pid = 0; };
struct PesEntry {
    uint8_t stream_id = 0;
    bool has_pts = false; uint64_t pts = 0;
    bool has_dts = false; uint64_t dts = 0;
    qint64 file_offset = 0;
};

// 从 5 字节读 33-bit 时间戳 (PTS/DTS)
uint64_t readTimestamp(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0] & 0x0E) << 29) |
           (static_cast<uint64_t>(p[1]) << 22) |
           (static_cast<uint64_t>(p[2] & 0xFE) << 14) |
           (static_cast<uint64_t>(p[3]) << 7) |
           (static_cast<uint64_t>(p[4]) >> 1);
}
QString tsToStr(uint64_t ts) {
    // 90 kHz 时钟
    return QString::number(ts / 90000.0, 'f', 3) + "s";
}
} // namespace

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

    // 扫描前 N 个包, 解析 PAT/PMT/PES
    QMap<uint16_t, int> pid_counts;
    QMap<uint16_t, uint16_t> pat_programs;            // program_number -> PMT_PID
    QMap<uint16_t, QVector<EsInfo>> pmt_streams;      // PMT_PID -> ES 列表
    QMap<uint16_t, QVector<PesEntry>> pes_by_pid;     // ES_PID -> PES 采样
    const int kMaxPesPerPid = 8;
    int total_packets = 0;
    int total_pes = 0;
    const int max_scan_packets = 5000;

    QByteArray pkt_buf(188, Qt::Uninitialized);
    while (total_packets < max_scan_packets && file.read(pkt_buf.data(), 188) == 188) {
        if (static_cast<uint8_t>(pkt_buf[0]) != 0x47) break;

        qint64 pkt_offset = file.pos() - 188;
        bool pusi = (static_cast<uint8_t>(pkt_buf[1]) & 0x40) != 0;
        uint16_t pid = ((static_cast<uint8_t>(pkt_buf[1]) & 0x1F) << 8) |
                        static_cast<uint8_t>(pkt_buf[2]);
        pid_counts[pid]++;

        int payload_start = 4;
        if (pkt_buf[3] & 0x20) {  // adaptation field
            int adapt_len = static_cast<uint8_t>(pkt_buf[4]);
            payload_start = 5 + adapt_len;
        }

        // PAT: PID 0
        if (pid == 0) {
            int ps = payload_start;
            if (ps < 188 && static_cast<uint8_t>(pkt_buf[ps]) == 0x00) ps++;  // pointer field
            if (ps + 7 < 188) {
                uint16_t section_length = ((static_cast<uint8_t>(pkt_buf[ps + 1]) & 0x0F) << 8) |
                                           static_cast<uint8_t>(pkt_buf[ps + 2]);
                int pos = ps + 8;
                while (pos + 3 < ps + 3 + section_length && pos + 3 < 188) {
                    uint16_t program_num = (static_cast<uint8_t>(pkt_buf[pos]) << 8) |
                                            static_cast<uint8_t>(pkt_buf[pos + 1]);
                    uint16_t pmt_pid = ((static_cast<uint8_t>(pkt_buf[pos + 2]) & 0x1F) << 8) |
                                        static_cast<uint8_t>(pkt_buf[pos + 3]);
                    if (program_num != 0) pat_programs[program_num] = pmt_pid;
                    pos += 4;
                }
            }
        }

        // PMT
        if (pat_programs.values().contains(pid)) {
            int ps = payload_start;
            if (ps < 188 && static_cast<uint8_t>(pkt_buf[ps]) == 0x00) ps++;
            if (ps + 11 < 188) {
                uint16_t section_length = ((static_cast<uint8_t>(pkt_buf[ps + 1]) & 0x0F) << 8) |
                                           static_cast<uint8_t>(pkt_buf[ps + 2]);
                uint16_t program_info_length = ((static_cast<uint8_t>(pkt_buf[ps + 10]) & 0x0F) << 8) |
                                                static_cast<uint8_t>(pkt_buf[ps + 11]);
                int pos = ps + 12 + program_info_length;
                QVector<EsInfo> streams;
                while (pos + 4 < ps + 3 + section_length && pos + 4 < 188) {
                    EsInfo es;
                    es.stream_type = static_cast<uint8_t>(pkt_buf[pos]);
                    es.elem_pid = ((static_cast<uint8_t>(pkt_buf[pos + 1]) & 0x1F) << 8) |
                                   static_cast<uint8_t>(pkt_buf[pos + 2]);
                    uint16_t es_info_length = ((static_cast<uint8_t>(pkt_buf[pos + 3]) & 0x0F) << 8) |
                                               static_cast<uint8_t>(pkt_buf[pos + 4]);
                    streams.append(es);
                    pos += 5 + es_info_length;
                }
                pmt_streams[pid] = streams;
            }
        }

        // PES: 载荷起始 + PES 起始码 00 00 01
        if (pusi && payload_start + 8 < 188 &&
            static_cast<uint8_t>(pkt_buf[payload_start]) == 0x00 &&
            static_cast<uint8_t>(pkt_buf[payload_start + 1]) == 0x00 &&
            static_cast<uint8_t>(pkt_buf[payload_start + 2]) == 0x01 &&
            pes_by_pid[pid].size() < kMaxPesPerPid) {
            PesEntry pe;
            pe.stream_id = static_cast<uint8_t>(pkt_buf[payload_start + 3]);
            pe.file_offset = pkt_offset;
            // 含可选 PES 头的 stream_id (排除 padding/map/private_2/ECM/EMM 等)
            uint8_t sid = pe.stream_id;
            bool has_ext = !(sid == 0xBC || sid == 0xBE || sid == 0xBF ||
                             sid == 0xF0 || sid == 0xF1 || sid == 0xF2 || sid == 0xF8 || sid == 0xFF);
            if (has_ext && payload_start + 13 < 188) {
                uint8_t pts_dts_flags = (static_cast<uint8_t>(pkt_buf[payload_start + 7]) >> 6) & 0x03;
                const uint8_t* opt = reinterpret_cast<const uint8_t*>(pkt_buf.constData()) + payload_start + 9;
                if (pts_dts_flags & 0x02) {  // PTS
                    pe.has_pts = true;
                    pe.pts = readTimestamp(opt);
                }
                if (pts_dts_flags == 0x03 && payload_start + 18 < 188) {  // PTS + DTS
                    pe.has_dts = true;
                    pe.dts = readTimestamp(opt + 5);
                }
            }
            pes_by_pid[pid].append(pe);
            total_pes++;
        }

        total_packets++;
    }

    // 构建结构树: PAT → Program → Elementary Stream → PES 采样
    for (auto it = pat_programs.begin(); it != pat_programs.end(); ++it) {
        model::ContainerElement prog;
        prog.name = QString("Program %1").arg(it.key());
        prog.type = "Program";
        prog.depth = 1;
        prog.value = QString("PMT PID=%1").arg(it.value());

        if (pmt_streams.contains(it.value())) {
            for (const auto& es : pmt_streams[it.value()]) {
                model::ContainerElement stream_elem;
                stream_elem.name = QString("PID %1 - %2").arg(es.elem_pid).arg(StreamTypeToName(es.stream_type));
                stream_elem.type = "Elementary Stream";
                stream_elem.depth = 2;

                // 附加 PES 采样
                if (pes_by_pid.contains(es.elem_pid)) {
                    const auto& list = pes_by_pid[es.elem_pid];
                    stream_elem.value = QString("%1 PES 采样").arg(list.size());
                    for (const auto& pe : list) {
                        model::ContainerElement pes_elem;
                        pes_elem.name = StreamIdName(pe.stream_id);
                        pes_elem.type = "PES";
                        pes_elem.depth = 3;
                        pes_elem.offset = static_cast<uint64_t>(pe.file_offset);
                        QString v;
                        if (pe.has_pts) v += "PTS=" + tsToStr(pe.pts);
                        if (pe.has_dts) v += (v.isEmpty() ? "" : " ") + QString("DTS=") + tsToStr(pe.dts);
                        if (v.isEmpty()) v = "(无时间戳)";
                        pes_elem.value = v;
                        stream_elem.children.append(pes_elem);
                    }
                }
                prog.children.append(stream_elem);

                // 添加到流信息
                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                si.codec = StreamTypeToName(es.stream_type);
                switch (es.stream_type) {
                case 0x01: case 0x02: case 0x10: case 0x1B: case 0x20: case 0x24:
                    si.type = "video"; break;
                case 0x03: case 0x04: case 0x0F: case 0x81: case 0x87: case 0xA1: case 0x82:
                    si.type = "audio"; break;
                default:
                    si.type = "data"; break;
                }
                si.details = QString("PID %1").arg(es.elem_pid);
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
    result.summary = QString("MPEG-TS | %1 包/已扫描 | %2 节目 | %3 流 | %4 PES 采样")
                         .arg(total_packets).arg(pat_programs.size())
                         .arg(result.streams.size()).arg(total_pes);

    file.close();
    return true;
}

} // namespace analyzer
} // namespace videoeye
