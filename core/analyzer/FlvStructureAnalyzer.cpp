#include "FlvStructureAnalyzer.h"
#include <QFile>
#include <QByteArray>
#include <QtEndian>

namespace videoeye {
namespace analyzer {

bool FlvStructureAnalyzer::Analyze(const QString& file_path, model::ContainerStructureResult& result) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::FLV;
    result.format_name = "FLV";
    result.file_path = file_path;

    // FLV Header: "FLV" + version(1) + flags(1) + header_size(4)
    QByteArray header = file.read(9);
    if (header.size() < 9 || header.left(3) != "FLV") {
        result.error_message = "不是有效的 FLV 文件";
        return false;
    }

    uint8_t version = static_cast<uint8_t>(header[3]);
    uint8_t flags = static_cast<uint8_t>(header[4]);
    uint32_t header_size = qFromBigEndian<uint32_t>(header.constData() + 5);

    bool has_video = (flags & 0x01) != 0;
    bool has_audio = (flags & 0x04) != 0;

    // 创建根元素
    model::ContainerElement root;
    root.name = "FLV Header";
    root.type = "Header";
    root.size = header_size;
    root.offset = 0;
    root.depth = 0;
    root.value = QString("version=%1 video=%2 audio=%3").arg(version).arg(has_video).arg(has_audio);

    if (has_video) {
        model::ContainerStreamInfo vs;
        vs.index = result.streams.size();
        vs.type = "video";
        vs.codec = "FLV Video";
        result.streams.append(vs);
    }
    if (has_audio) {
        model::ContainerStreamInfo as;
        as.index = result.streams.size();
        as.type = "audio";
        as.codec = "FLV Audio";
        result.streams.append(as);
    }

    // 跳过 PreviousTagSize0
    file.seek(header_size);

    // 遍历 Tag 序列 (最多收集前 500 个作为结构概览)
    int tag_count = 0;
    int video_tags = 0, audio_tags = 0, script_tags = 0;
    const int max_tags = 500;

    while (file.pos() < file.size() - 11 && tag_count < max_tags) {
        QByteArray tag_header = file.read(11);
        if (tag_header.size() < 11) break;

        uint8_t tag_type = static_cast<uint8_t>(tag_header[0]);
        uint32_t data_size = (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[1])) << 16) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[2])) << 8) |
                              static_cast<uint32_t>(static_cast<uint8_t>(tag_header[3]));
        uint32_t timestamp = (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[4])) << 16) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(tag_header[5])) << 8) |
                              static_cast<uint32_t>(static_cast<uint8_t>(tag_header[6]));
        uint8_t ts_ext = static_cast<uint8_t>(tag_header[7]);
        timestamp |= (static_cast<uint32_t>(ts_ext) << 24);

        model::ContainerElement tag;
        tag.offset = file.pos() - 11;
        tag.size = data_size + 11;
        tag.depth = 1;

        switch (tag_type) {
        case 8:
            tag.name = "Audio Tag";
            tag.type = "Audio";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            audio_tags++;
            break;
        case 9:
            tag.name = "Video Tag";
            tag.type = "Video";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            video_tags++;
            break;
        case 18:
            tag.name = "Script Tag";
            tag.type = "Script";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            script_tags++;
            break;
        default:
            tag.name = QString("Tag type=%1").arg(tag_type);
            tag.type = "Unknown";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            break;
        }

        root.children.append(tag);

        // 跳过 tag data + PreviousTagSize (4 bytes)
        file.seek(file.pos() + data_size + 4);
        tag_count++;
    }

    result.element_tree.append(root);
    result.valid = true;
    result.summary = QString("FLV | Video Tags: %1 | Audio Tags: %2 | Script Tags: %3")
                         .arg(video_tags).arg(audio_tags).arg(script_tags);

    file.close();
    return true;
}

} // namespace analyzer
} // namespace videoeye
