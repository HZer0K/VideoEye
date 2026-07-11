#include "FlvStructureAnalyzer.h"
#include <QFile>
#include <QByteArray>
#include <QtEndian>
#include <cstring>

namespace videoeye {
namespace analyzer {

namespace {
// FLV 视频 CodecID → 名称
QString flvVideoCodec(int id) {
    switch (id) {
        case 1: return "JPEG";
        case 2: return "Sorenson H.263";
        case 3: return "Screen Video";
        case 4: return "On2 VP6";
        case 5: return "On2 VP6 Alpha";
        case 6: return "Screen Video v2";
        case 7: return "H.264 (AVC)";
        case 12: return "H.265 (HEVC)";
        default: return QString("CodecID %1").arg(id);
    }
}
// FLV 音频 SoundFormat → 名称
QString flvAudioCodec(int fmt) {
    switch (fmt) {
        case 0: return "Linear PCM";
        case 1: return "ADPCM";
        case 2: return "MP3";
        case 3: return "PCM LE";
        case 4: return "Nellymoser 16kHz";
        case 5: return "Nellymoser 8kHz";
        case 6: return "Nellymoser";
        case 7: return "G.711 A-law";
        case 8: return "G.711 mu-law";
        case 10: return "AAC";
        case 11: return "Speex";
        case 14: return "MP3 8kHz";
        default: return QString("SoundFormat %1").arg(fmt);
    }
}
int flvSoundRate(int r) {
    switch (r) { case 0: return 5500; case 1: return 11025; case 2: return 22050; case 3: return 44100; }
    return 0;
}

// ---- 最小 AMF0 解析器：从 Script Tag(onMetadata) 提取 metadata 键值 ----
double readBeDouble(const uint8_t* p) {
    quint64 be = qFromBigEndian<quint64>(p);
    double d; std::memcpy(&d, &be, 8); return d;
}
QString amfNumToStr(double d) {
    if (d == static_cast<double>(static_cast<long long>(d)))
        return QString::number(static_cast<long long>(d));
    return QString::number(d, 'g', 10);
}
// 解析一个 AMF0 值，pos 前进；仅把标量写入 out(key 非空时)
void parseAmf0Value(const QByteArray& b, int& pos, const QString& key, QMap<QString, QString>& out, int depth);
void parseAmf0Properties(const QByteArray& b, int& pos, QMap<QString, QString>& out, int depth) {
    while (pos + 2 <= b.size()) {
        uint16_t klen = qFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t*>(b.constData() + pos));
        pos += 2;
        if (klen == 0) { // 可能是对象结束标记 00 00 09
            if (pos < b.size() && static_cast<uint8_t>(b[pos]) == 0x09) pos++;
            break;
        }
        if (pos + klen > b.size()) break;
        QString k = QString::fromUtf8(b.mid(pos, klen));
        pos += klen;
        parseAmf0Value(b, pos, k, out, depth);
    }
}
void parseAmf0Value(const QByteArray& b, int& pos, const QString& key,
                    QMap<QString, QString>& out, int depth) {
    if (pos >= b.size() || depth > 6) return;
    uint8_t type = static_cast<uint8_t>(b[pos++]);
    switch (type) {
        case 0x00: { // Number
            if (pos + 8 > b.size()) { pos = b.size(); return; }
            double d = readBeDouble(reinterpret_cast<const uint8_t*>(b.constData() + pos));
            pos += 8;
            if (!key.isEmpty()) out[key] = amfNumToStr(d);
            break;
        }
        case 0x01: { // Boolean
            if (pos >= b.size()) return;
            bool v = b[pos++] != 0;
            if (!key.isEmpty()) out[key] = v ? "true" : "false";
            break;
        }
        case 0x02: { // String
            if (pos + 2 > b.size()) { pos = b.size(); return; }
            uint16_t sl = qFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t*>(b.constData() + pos));
            pos += 2;
            if (pos + sl > b.size()) { pos = b.size(); return; }
            QString s = QString::fromUtf8(b.mid(pos, sl));
            pos += sl;
            if (!key.isEmpty()) out[key] = s;
            break;
        }
        case 0x03: // Object
            parseAmf0Properties(b, pos, out, depth + 1);
            break;
        case 0x08: { // ECMA Array (4字节计数 + 属性)
            if (pos + 4 > b.size()) { pos = b.size(); return; }
            pos += 4; // 忽略计数，按属性对读到结束标记
            parseAmf0Properties(b, pos, out, depth + 1);
            break;
        }
        case 0x0A: { // Strict Array
            if (pos + 4 > b.size()) { pos = b.size(); return; }
            uint32_t n = qFromBigEndian<uint32_t>(reinterpret_cast<const uint8_t*>(b.constData() + pos));
            pos += 4;
            for (uint32_t i = 0; i < n && pos < b.size(); ++i)
                parseAmf0Value(b, pos, QString(), out, depth + 1);
            break;
        }
        case 0x0B: // Date: double + int16 tz
            pos += 10; break;
        case 0x05: case 0x06: // Null / Undefined
            break;
        case 0x09: // Object end
            break;
        default:   // 未知类型，无法安全跳过
            pos = b.size(); break;
    }
}
} // namespace

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

    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    if (has_video) {
        model::ContainerStreamInfo vs;
        vs.index = result.streams.size();
        vs.type = "video";
        vs.codec = "FLV Video";
        video_stream_idx = vs.index;
        result.streams.append(vs);
    }
    if (has_audio) {
        model::ContainerStreamInfo as;
        as.index = result.streams.size();
        as.type = "audio";
        as.codec = "FLV Audio";
        audio_stream_idx = as.index;
        result.streams.append(as);
    }

    // 跳过 PreviousTagSize0
    file.seek(header_size);

    // 遍历 Tag 序列 (最多收集前 500 个作为结构概览)
    int tag_count = 0;
    int video_tags = 0, audio_tags = 0, script_tags = 0;
    bool video_codec_found = false, audio_codec_found = false, metadata_found = false;
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

        qint64 data_start = file.pos();

        model::ContainerElement tag;
        tag.offset = data_start - 11;
        tag.size = data_size + 11;
        tag.depth = 1;

        switch (tag_type) {
        case 8: {
            tag.name = "Audio Tag";
            tag.type = "Audio";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            audio_tags++;
            if (!audio_codec_found && data_size >= 1) {
                uint8_t sound = static_cast<uint8_t>(file.read(1).at(0));
                int fmt = (sound >> 4) & 0x0F;
                int rate = (sound >> 2) & 0x03;
                int size = (sound >> 1) & 0x01;
                int chan = sound & 0x01;
                QString codec = flvAudioCodec(fmt);
                tag.value += QString(" | %1 %2Hz %3 %4bit")
                                 .arg(codec).arg(flvSoundRate(rate))
                                 .arg(chan ? "stereo" : "mono").arg(size ? 16 : 8);
                if (audio_stream_idx >= 0) {
                    auto& s = result.streams[audio_stream_idx];
                    s.codec = codec;
                    s.details = QString("%1 Hz, %2, %3-bit")
                                    .arg(flvSoundRate(rate))
                                    .arg(chan ? "stereo" : "mono").arg(size ? 16 : 8);
                }
                audio_codec_found = true;
            }
            break;
        }
        case 9: {
            tag.name = "Video Tag";
            tag.type = "Video";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            video_tags++;
            if (!video_codec_found && data_size >= 1) {
                uint8_t vh = static_cast<uint8_t>(file.read(1).at(0));
                int codec_id = vh & 0x0F;
                QString codec = flvVideoCodec(codec_id);
                tag.value += QString(" | %1").arg(codec);
                if (video_stream_idx >= 0) result.streams[video_stream_idx].codec = codec;
                video_codec_found = true;
            }
            break;
        }
        case 18: {
            tag.name = "Script Tag";
            tag.type = "Script";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            script_tags++;
            if (!metadata_found && data_size > 0 && data_size < 1024 * 1024) {
                QByteArray sdata = file.read(data_size);
                int pos = 0;
                // 第一个 AMF0 值通常是字符串 "onMetadata"
                QMap<QString, QString> ignore;
                parseAmf0Value(sdata, pos, QString(), ignore, 0);
                // 第二个值为 metadata 对象/ECMA 数组
                QMap<QString, QString> meta;
                parseAmf0Value(sdata, pos, QString(), meta, 0);
                for (auto it = meta.begin(); it != meta.end(); ++it) {
                    if (!result.metadata.contains(it.key()))
                        result.metadata[it.key()] = it.value();
                }
                if (!meta.isEmpty()) {
                    tag.value += " | onMetadata";
                    tag.extra = QStringLiteral("%1 项元数据").arg(meta.size());
                }
                metadata_found = true;
            }
            break;
        }
        default:
            tag.name = QString("Tag type=%1").arg(tag_type);
            tag.type = "Unknown";
            tag.value = QString("ts=%1 size=%2").arg(timestamp).arg(data_size);
            break;
        }

        root.children.append(tag);

        // 跳过 tag data + PreviousTagSize (4 bytes)，始终基于 data_start 定位以避免上面读了部分数据
        file.seek(data_start + data_size + 4);
        tag_count++;
    }

    result.element_tree.append(root);
    result.valid = true;
    QString summary = QString("FLV | Video Tags: %1 | Audio Tags: %2 | Script Tags: %3")
                          .arg(video_tags).arg(audio_tags).arg(script_tags);
    if (result.metadata.contains("width") && result.metadata.contains("height"))
        summary += QString(" | %1x%2").arg(result.metadata["width"], result.metadata["height"]);
    if (result.metadata.contains("duration"))
        summary += QString(" | %1s").arg(result.metadata["duration"]);
    result.summary = summary;

    file.close();
    return true;
}

} // namespace analyzer
} // namespace videoeye
