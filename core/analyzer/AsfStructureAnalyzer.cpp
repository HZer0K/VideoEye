#include "AsfStructureAnalyzer.h"
#include <QFile>
#include <QByteArray>
#include <QUuid>

namespace videoeye {
namespace analyzer {

// ASF GUIDs (little-endian byte order)
static const QByteArray kHeaderObjectGuid =
    QByteArray::fromHex("3026B2758E66CF11A6D900AA0062CE6C");
static const QByteArray kFilePropertiesGuid =
    QByteArray::fromHex("A1DCAB847F97CE11A6D900AA0062CE6C");
static const QByteArray kStreamPropertiesGuid =
    QByteArray::fromHex("9107DCB7B7A9CF118EE600C00C205365");
static const QByteArray kContentDescriptionGuid =
    QByteArray::fromHex("75B22633668E11CFB6D000805F49A3C");
static const QByteArray kDataObjectGuid =
    QByteArray::fromHex("3626B2758E66CF11A6D900AA0062CE6C");
static const QByteArray kIndexObjectGuid =
    QByteArray::fromHex("D6E259D06F97CF11A6D900AA0062CE6C");
static const QByteArray kVideoStreamGuid =
    QByteArray::fromHex("C0EF1FBCA3E311DCA3B900805F49A3C");
static const QByteArray kAudioStreamGuid =
    QByteArray::fromHex("4B1ACBE3100B11D0A3B900805F49A3C");

static QString GuidToName(const QByteArray& guid) {
    if (guid == kHeaderObjectGuid) return "Header Object";
    if (guid == kFilePropertiesGuid) return "File Properties";
    if (guid == kStreamPropertiesGuid) return "Stream Properties";
    if (guid == kContentDescriptionGuid) return "Content Description";
    if (guid == kDataObjectGuid) return "Data Object";
    if (guid == kIndexObjectGuid) return "Index Object";
    return "Unknown Object";
}

bool AsfStructureAnalyzer::Analyze(const QString& file_path, model::ContainerStructureResult& result) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::ASF;
    result.format_name = "ASF";
    result.file_path = file_path;

    // Read top-level Header Object
    QByteArray guid = file.read(16);
    if (guid.size() < 16 || guid != kHeaderObjectGuid) {
        result.error_message = "不是有效的 ASF 文件";
        return false;
    }

    // Size (8 bytes, little-endian)
    QByteArray size_buf = file.read(8);
    if (size_buf.size() < 8) return false;
    uint64_t header_size = 0;
    for (int i = 0; i < 8; ++i) {
        header_size |= static_cast<uint64_t>(static_cast<uint8_t>(size_buf[i])) << (i * 8);
    }

    // Number of header objects (4 bytes LE)
    QByteArray count_buf = file.read(4);
    uint32_t num_objects = 0;
    if (count_buf.size() >= 4) {
        for (int i = 0; i < 4; ++i) {
            num_objects |= static_cast<uint32_t>(static_cast<uint8_t>(count_buf[i])) << (i * 8);
        }
    }

    // Skip 2 reserved bytes
    file.read(2);

    model::ContainerElement root;
    root.name = "ASF Header";
    root.type = "Header";
    root.size = header_size;
    root.offset = 0;
    root.depth = 0;
    root.value = QString("objects=%1").arg(num_objects);

    // Parse child objects
    qint64 header_end = static_cast<qint64>(header_size);
    if (header_end > file.size()) header_end = file.size();

    for (uint32_t i = 0; i < num_objects && file.pos() < header_end - 24; ++i) {
        QByteArray obj_guid = file.read(16);
        QByteArray obj_size_buf = file.read(8);
        if (obj_guid.size() < 16 || obj_size_buf.size() < 8) break;

        uint64_t obj_size = 0;
        for (int j = 0; j < 8; ++j) {
            obj_size |= static_cast<uint64_t>(static_cast<uint8_t>(obj_size_buf[j])) << (j * 8);
        }

        qint64 obj_start = file.pos() - 24;
        QString obj_name = GuidToName(obj_guid);

        model::ContainerElement elem;
        elem.name = obj_name;
        elem.type = "ASF Object";
        elem.size = obj_size;
        elem.offset = obj_start;
        elem.depth = 1;

        // Parse specific objects
        if (obj_guid == kFilePropertiesGuid) {
            QByteArray data = file.read(qMin(static_cast<qint64>(obj_size - 24), static_cast<qint64>(104)));
            if (data.size() >= 96) {
                // Duration is at offset 40 (8 bytes, 100ns units)
                uint64_t duration_100ns = 0;
                for (int j = 0; j < 8; ++j) {
                    duration_100ns |= static_cast<uint64_t>(static_cast<uint8_t>(data[40 + j])) << (j * 8);
                }
                double duration_sec = duration_100ns / 10000000.0;
                elem.value = QString("duration=%.2fs").arg(duration_sec);
                result.metadata["duration"] = QString::number(duration_sec, 'f', 2) + "s";
            }
        } else if (obj_guid == kStreamPropertiesGuid) {
            QByteArray data = file.read(qMin(static_cast<qint64>(obj_size - 24), static_cast<qint64>(78)));
            if (data.size() >= 54) {
                QByteArray stream_type_guid = data.left(16);
                QString stream_type;
                if (stream_type_guid == kVideoStreamGuid) {
                    stream_type = "video";
                    elem.value = "Video Stream";
                } else if (stream_type_guid == kAudioStreamGuid) {
                    stream_type = "audio";
                    elem.value = "Audio Stream";
                } else {
                    stream_type = "data";
                    elem.value = "Data Stream";
                }
                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                si.type = stream_type;
                si.codec = "ASF Codec";
                result.streams.append(si);
            }
        } else if (obj_guid == kContentDescriptionGuid) {
            elem.value = "Metadata";
            // Skip detailed parsing of content description
            file.seek(obj_start + obj_size);
        } else {
            elem.value = QString("size=%1").arg(obj_size);
        }

        // Ensure we're at the right position for the next object
        qint64 next_pos = obj_start + static_cast<qint64>(obj_size);
        if (next_pos <= file.size()) {
            file.seek(next_pos);
        } else {
            file.seek(file.size());
        }

        root.children.append(elem);
    }

    // Scan for Data Object and Index Object after header
    file.seek(header_end);
    while (file.pos() < file.size() - 24) {
        QByteArray obj_guid = file.read(16);
        QByteArray obj_size_buf = file.read(8);
        if (obj_guid.size() < 16 || obj_size_buf.size() < 8) break;

        uint64_t obj_size = 0;
        for (int j = 0; j < 8; ++j) {
            obj_size |= static_cast<uint64_t>(static_cast<uint8_t>(obj_size_buf[j])) << (j * 8);
        }

        model::ContainerElement elem;
        elem.name = GuidToName(obj_guid);
        elem.type = "ASF Object";
        elem.size = obj_size;
        elem.offset = file.pos() - 24;
        elem.depth = 1;
        elem.value = QString("size=%1").arg(obj_size);
        root.children.append(elem);

        if (obj_size < 24) break;
        file.seek(file.pos() - 24 + static_cast<qint64>(obj_size));
    }

    result.element_tree.append(root);
    result.valid = true;
    result.summary = QString("ASF | 文件大小: %1 字节 | 流: %2 个").arg(file.size()).arg(result.streams.size());

    file.close();
    return true;
}

} // namespace analyzer
} // namespace videoeye
