#include "AsfStructureAnalyzer.h"
#include <QFile>
#include <QByteArray>
#include <QUuid>

namespace videoeye {
namespace analyzer {

// ASF GUIDs (以文件中的字节序存储: 前3字段小端, 后8字节原序)
static const QByteArray kHeaderObjectGuid =
    QByteArray::fromHex("3026B2758E66CF11A6D900AA0062CE6C");
static const QByteArray kFilePropertiesGuid =
    QByteArray::fromHex("A1DCAB8C47A9CF118EE400C00C205365");
static const QByteArray kStreamPropertiesGuid =
    QByteArray::fromHex("9107DCB7B7A9CF118EE600C00C205365");
static const QByteArray kContentDescriptionGuid =
    QByteArray::fromHex("3326B2758E66CF11A6D900AA0062CE6C");
static const QByteArray kExtContentDescGuid =
    QByteArray::fromHex("40A4D0D207E3D21197F000A0C95EA850");
static const QByteArray kDataObjectGuid =
    QByteArray::fromHex("3626B2758E66CF11A6D900AA0062CE6C");
static const QByteArray kIndexObjectGuid =
    QByteArray::fromHex("90080033B1E5CF1189F400A0C90349CB");
static const QByteArray kVideoStreamGuid =
    QByteArray::fromHex("C0EF19BC4D5BCF11A8FD00805F5C442B");
static const QByteArray kAudioStreamGuid =
    QByteArray::fromHex("409E69F84D5BCF11A8FD00805F5C442B");

namespace {
uint16_t asfLE16(const QByteArray& d, int off) {
    if (off + 2 > d.size()) return 0;
    return static_cast<uint16_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(d[off + 1])) << 8);
}
uint32_t asfLE32(const QByteArray& d, int off) {
    if (off + 4 > d.size()) return 0;
    return static_cast<uint32_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3])) << 24);
}
uint64_t asfLE64(const QByteArray& d, int off) {
    uint64_t v = 0;
    for (int i = 0; i < 8 && off + i < d.size(); ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(d[off + i])) << (i * 8);
    return v;
}
// UTF-16LE 定长字符串 (含结尾 NUL)
QString asfUtf16(const QByteArray& d, int off, int bytes) {
    if (bytes <= 0 || off + bytes > d.size()) return QString();
    QString s = QString::fromUtf16(reinterpret_cast<const char16_t*>(d.constData() + off), bytes / 2);
    return s.remove(QChar('\0')).trimmed();
}
QString waveFormatName(uint16_t tag) {
    switch (tag) {
        case 0x0001: return "PCM";
        case 0x0002: return "ADPCM";
        case 0x0055: return "MP3";
        case 0x0161: return "WMA v2";
        case 0x0162: return "WMA Pro";
        case 0x0163: return "WMA Lossless";
        case 0x00FF: return "AAC";
        case 0x2000: return "AC-3";
        default: return QString("0x%1").arg(tag, 4, 16, QChar('0')).toUpper();
    }
}
} // namespace

static QString GuidToName(const QByteArray& guid) {
    if (guid == kHeaderObjectGuid) return "Header Object";
    if (guid == kFilePropertiesGuid) return "File Properties";
    if (guid == kStreamPropertiesGuid) return "Stream Properties";
    if (guid == kContentDescriptionGuid) return "Content Description";
    if (guid == kExtContentDescGuid) return "Extended Content Description";
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
    uint64_t header_size = asfLE64(size_buf, 0);

    // Number of header objects (4 bytes LE)
    QByteArray count_buf = file.read(4);
    uint32_t num_objects = count_buf.size() >= 4 ? asfLE32(count_buf, 0) : 0;

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

        uint64_t obj_size = asfLE64(obj_size_buf, 0);
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
            if (data.size() >= 64) {
                // Play Duration @40 (8, 100ns), Preroll @56 (8, ms)
                uint64_t play_100ns = asfLE64(data, 40);
                uint64_t preroll_ms = asfLE64(data, 56);
                double duration_sec = play_100ns / 10000000.0 - preroll_ms / 1000.0;
                if (duration_sec < 0) duration_sec = play_100ns / 10000000.0;
                elem.value = QString("duration=%1s").arg(QString::number(duration_sec, 'f', 2));
                result.metadata["duration"] = QString::number(duration_sec, 'f', 2) + "s";
            }
        } else if (obj_guid == kStreamPropertiesGuid) {
            QByteArray data = file.read(qMin(static_cast<qint64>(obj_size - 24), static_cast<qint64>(256)));
            if (data.size() >= 54) {
                QByteArray stream_type_guid = data.left(16);
                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                if (stream_type_guid == kVideoStreamGuid) {
                    si.type = "video";
                    // 视频 type-specific 从 offset 54: width(4),height(4),flags(1),fmtsize(2),BITMAPINFOHEADER
                    uint32_t w = asfLE32(data, 54);
                    uint32_t h = asfLE32(data, 58);
                    // BITMAPINFOHEADER biCompression @ 54+11+16 = 81
                    QString fourcc = QString::fromLatin1(data.mid(81, 4)).trimmed();
                    si.codec = fourcc.isEmpty() ? "Video" : fourcc;
                    si.details = QString("%1x%2").arg(w).arg(h);
                    elem.value = QString("Video %1x%2 %3").arg(w).arg(h).arg(si.codec);
                } else if (stream_type_guid == kAudioStreamGuid) {
                    si.type = "audio";
                    // WAVEFORMATEX 从 offset 54
                    uint16_t tag = asfLE16(data, 54);
                    uint16_t ch = asfLE16(data, 56);
                    uint32_t sr = asfLE32(data, 58);
                    uint16_t bits = asfLE16(data, 68);
                    si.codec = waveFormatName(tag);
                    si.details = QString("%1 Hz, %2 ch, %3-bit").arg(sr).arg(ch).arg(bits);
                    elem.value = QString("Audio %1 %2Hz %3ch").arg(si.codec).arg(sr).arg(ch);
                } else {
                    si.type = "data";
                    si.codec = "ASF Data";
                    elem.value = "Data Stream";
                }
                result.streams.append(si);
            }
        } else if (obj_guid == kContentDescriptionGuid) {
            QByteArray data = file.read(qMin(static_cast<qint64>(obj_size - 24), static_cast<qint64>(64 * 1024)));
            if (data.size() >= 10) {
                int tl = asfLE16(data, 0), al = asfLE16(data, 2), cl = asfLE16(data, 4);
                int dl = asfLE16(data, 6), rl = asfLE16(data, 8);
                int p = 10;
                QString title = asfUtf16(data, p, tl); p += tl;
                QString author = asfUtf16(data, p, al); p += al;
                QString copyright = asfUtf16(data, p, cl); p += cl;
                QString desc = asfUtf16(data, p, dl); p += dl;
                QString rating = asfUtf16(data, p, rl); p += rl;
                if (!title.isEmpty()) result.metadata["title"] = title;
                if (!author.isEmpty()) result.metadata["author"] = author;
                if (!copyright.isEmpty()) result.metadata["copyright"] = copyright;
                if (!desc.isEmpty()) result.metadata["description"] = desc;
                if (!rating.isEmpty()) result.metadata["rating"] = rating;
                elem.value = title.isEmpty() ? "Metadata" : ("Title: " + title);
            } else {
                elem.value = "Metadata";
            }
        } else {
            elem.value = QString("size=%1").arg(obj_size);
        }

        // Ensure we're at the right position for the next object
        qint64 next_pos = obj_start + static_cast<qint64>(obj_size);
        if (obj_size >= 24 && next_pos <= file.size()) {
            file.seek(next_pos);
        } else {
            file.seek(file.size());
            root.children.append(elem);
            break;
        }

        root.children.append(elem);
    }

    // Scan for Data Object and Index Object after header
    file.seek(header_end);
    while (file.pos() < file.size() - 24) {
        QByteArray obj_guid = file.read(16);
        QByteArray obj_size_buf = file.read(8);
        if (obj_guid.size() < 16 || obj_size_buf.size() < 8) break;

        uint64_t obj_size = asfLE64(obj_size_buf, 0);

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
