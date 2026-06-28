#include "OggStructureAnalyzer.h"
#include <QFile>
#include <QByteArray>
#include <QMap>

namespace videoeye {
namespace analyzer {

bool OggStructureAnalyzer::Analyze(const QString& file_path, model::ContainerStructureResult& result) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::OGG;
    result.format_name = "OGG";
    result.file_path = file_path;

    model::ContainerElement root;
    root.name = "OGG Stream";
    root.type = "OGG";
    root.size = file.size();
    root.offset = 0;
    root.depth = 0;

    // 跟踪 logical streams
    struct StreamInfo {
        int page_count = 0;
        QString codec_name;
        bool bos_seen = false;
    };
    QMap<uint32_t, StreamInfo> streams;

    int total_pages = 0;
    const int max_pages = 2000;

    while (file.pos() < file.size() - 27 && total_pages < max_pages) {
        // Ogg Page Header: "OggS" (4) + version(1) + type(1) + granule(8) + serial(4) + page_seq(4) + checksum(4) + segments(1)
        QByteArray header = file.read(27);
        if (header.size() < 27) break;

        if (header.left(4) != "OggS") {
            // 尝试重新同步
            QByteArray sync = file.read(1);
            while (!file.atEnd() && sync != "O") {
                sync = file.read(1);
            }
            if (file.atEnd()) break;
            continue;
        }

        uint8_t header_type = static_cast<uint8_t>(header[5]);
        bool is_bos = (header_type & 0x02) != 0;
        bool is_eos = (header_type & 0x04) != 0;

        uint32_t serial = (static_cast<uint8_t>(header[14])) |
                          (static_cast<uint8_t>(header[15]) << 8) |
                          (static_cast<uint8_t>(header[16]) << 16) |
                          (static_cast<uint8_t>(header[17]) << 24);

        uint32_t page_seq = (static_cast<uint8_t>(header[18])) |
                            (static_cast<uint8_t>(header[19]) << 8) |
                            (static_cast<uint8_t>(header[20]) << 16) |
                            (static_cast<uint8_t>(header[21]) << 24);

        uint8_t num_segments = static_cast<uint8_t>(header[26]);

        // 读取 segment table
        QByteArray seg_table = file.read(num_segments);
        if (seg_table.size() < num_segments) break;

        uint32_t page_data_size = 0;
        for (int i = 0; i < num_segments; ++i) {
            page_data_size += static_cast<uint8_t>(seg_table[i]);
        }

        // 读取页面数据 (仅前 64 字节用于 codec 识别)
        qint64 page_data_offset = file.pos();
        QByteArray page_data = file.read(qMin(static_cast<qint64>(page_data_size), static_cast<qint64>(64)));

        // 跳到下一页
        file.seek(page_data_offset + page_data_size);

        // 更新流信息
        auto& si = streams[serial];
        si.page_count++;

        // BOS 页面识别 codec
        if (is_bos && page_data.size() >= 7) {
            if (page_data.mid(1, 6) == "vorbis") {
                si.codec_name = "Vorbis";
            } else if (page_data.startsWith("OpusHead")) {
                si.codec_name = "Opus";
            } else if (page_data.mid(0, 4) == "fLaC" || page_data.startsWith(QByteArray("\x7F" "FLAC", 5))) {
                si.codec_name = "FLAC";
            } else if (page_data.mid(1, 6) == "theora") {
                si.codec_name = "Theora";
            } else if (page_data.mid(0, 5) == "\x80theora") {
                si.codec_name = "Theora";
            } else if (page_data.startsWith("Speex")) {
                si.codec_name = "Speex";
            } else {
                si.codec_name = "Unknown";
            }
        }

        // 创建页面元素 (仅前 200 个页面加入树)
        if (total_pages < 200) {
            model::ContainerElement page;
            page.name = QString("Page #%1").arg(page_seq);
            page.type = "Ogg Page";
            page.size = 27 + num_segments + page_data_size;
            page.offset = page_data_offset - 27 - num_segments;
            page.depth = 1;

            QString flags;
            if (is_bos) flags += "BOS ";
            if (is_eos) flags += "EOS ";
            page.value = QString("serial=%1 %2size=%3")
                             .arg(serial).arg(flags, QString::number(page_data_size));
            root.children.append(page);
        }

        total_pages++;
    }

    // 添加流信息
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        model::ContainerElement stream_elem;
        stream_elem.name = QString("Logical Stream (serial=%1)").arg(it.key());
        stream_elem.type = "Logical Stream";
        stream_elem.depth = 1;
        stream_elem.value = QString("codec=%1 pages=%2").arg(it.value().codec_name).arg(it.value().page_count);

        model::ContainerStreamInfo csi;
        csi.index = result.streams.size();
        csi.codec = it.value().codec_name;
        // 根据 codec 推断类型
        QString codec_lower = it.value().codec_name.toLower();
        if (codec_lower == "vorbis" || codec_lower == "opus" || codec_lower == "flac" || codec_lower == "speex") {
            csi.type = "audio";
        } else if (codec_lower == "theora") {
            csi.type = "video";
        } else {
            csi.type = "data";
        }
        result.streams.append(csi);

        root.children.append(stream_elem);
    }

    result.element_tree.append(root);
    result.valid = true;
    result.summary = QString("OGG | %1 页/已解析 | %2 逻辑流").arg(total_pages).arg(streams.size());

    file.close();
    return true;
}

} // namespace analyzer
} // namespace videoeye
