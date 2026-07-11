#include "AviStructureAnalyzer.h"
#include <QFile>
#include <QDataStream>
#include <QByteArray>

namespace videoeye {
namespace analyzer {

namespace {
// 小端读取辅助 (AVI/RIFF 为小端)
uint16_t readLE16(const QByteArray& d, int off) {
    if (off + 2 > d.size()) return 0;
    return static_cast<uint16_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(d[off + 1])) << 8);
}
uint32_t readLE32(const QByteArray& d, int off) {
    if (off + 4 > d.size()) return 0;
    return static_cast<uint32_t>(static_cast<uint8_t>(d[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[off + 3])) << 24);
}
// WAVE 格式 tag → 可读音频编码名
QString waveFormatName(uint16_t tag) {
    switch (tag) {
        case 0x0001: return "PCM";
        case 0x0002: return "ADPCM";
        case 0x0003: return "IEEE Float";
        case 0x0006: return "A-law";
        case 0x0007: return "mu-law";
        case 0x0011: return "IMA ADPCM";
        case 0x0031: case 0x0032: return "GSM 6.10";
        case 0x0050: return "MPEG";
        case 0x0055: return "MP3";
        case 0x00FF: return "AAC";
        case 0x2000: return "AC-3";
        case 0x2001: return "DTS";
        case 0xF1AC: return "FLAC";
        case 0x674F: case 0x6750: case 0x6751: return "Ogg Vorbis";
        default: return QString("0x%1").arg(tag, 4, 16, QChar('0')).toUpper();
    }
}
} // namespace

bool AviStructureAnalyzer::Analyze(const QString& file_path, model::ContainerStructureResult& result) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error_message = "无法打开文件";
        return false;
    }

    result.format = model::ContainerFormat::AVI;
    result.format_name = "AVI";
    result.file_path = file_path;

    // 读取 RIFF header: "RIFF" + size(4) + "AVI "
    QByteArray header = file.read(12);
    if (header.size() < 12 || header.left(4) != "RIFF" || header.mid(8, 4) != "AVI ") {
        result.error_message = "不是有效的 AVI 文件";
        return false;
    }

    uint32_t riff_size = *reinterpret_cast<const uint32_t*>(header.constData() + 4);

    // 创建根元素
    model::ContainerElement root;
    root.name = "RIFF 'AVI '";
    root.type = "RIFF";
    root.size = riff_size + 8;
    root.offset = 0;
    root.depth = 0;
    root.value = QString("size=%1").arg(riff_size + 8);

    // 递归解析子块
    qint64 end_offset = qMin(static_cast<qint64>(riff_size + 8), file.size());
    ParseChunk(file, end_offset, 1, root, result);

    result.element_tree.append(root);
    result.valid = true;
    result.summary = QString("AVI (RIFF) | 文件大小: %1 字节").arg(file.size());

    file.close();
    return true;
}

bool AviStructureAnalyzer::ParseChunk(QFile& file, qint64 end_offset, int depth,
                                       model::ContainerElement& parent,
                                       model::ContainerStructureResult& result) {
    // 限制递归深度防止栈溢出
    if (depth > 8) return false;

    // 当前 strl 内 strh 记录的流类型/流索引，供随后的 strf 解释格式头
    QString cur_strh_type;   // "vids" / "auds" / ...
    int cur_stream_idx = -1;

    while (file.pos() < end_offset - 8) {
        QByteArray chunk_header = file.read(8);
        if (chunk_header.size() < 8) break;

        QByteArray fourcc = chunk_header.left(4);
        uint32_t chunk_size = *reinterpret_cast<const uint32_t*>(chunk_header.constData() + 4);
        qint64 chunk_offset = file.pos() - 8;

        model::ContainerElement elem;
        elem.name = QString::fromLatin1(fourcc);
        elem.size = chunk_size;
        elem.offset = chunk_offset;
        elem.depth = depth;

        // 判断是否为 LIST 容器
        if (fourcc == "LIST" || fourcc == "RIFF") {
            QByteArray list_type = file.read(4);
            elem.name = QString("%1 '%2'").arg(QString::fromLatin1(fourcc),
                                                QString::fromLatin1(list_type));
            elem.type = "LIST";
            elem.value = QString("size=%1").arg(chunk_size);

            qint64 list_end = file.pos() + chunk_size - 4;
            if (list_end > file.size()) list_end = file.size();

            ParseChunk(file, list_end, depth + 1, elem, result);

            // 确保跳到正确位置 (chunk_size 对齐到偶数)
            qint64 expected_pos = chunk_offset + 8 + chunk_size;
            if (chunk_size % 2 != 0) expected_pos++;
            if (expected_pos <= file.size()) {
                file.seek(expected_pos);
            }
        } else if (fourcc == "strh") {
            // 流头信息: 提取流类型和编解码器
            elem.type = "Stream Header";
            QByteArray data = file.read(chunk_size);
            if (data.size() >= 8) {
                QString fcc_type = QString::fromLatin1(data.left(4));
                QString fcc_handler = QString::fromLatin1(data.mid(4, 4));
                elem.value = QString("type=%1 codec=%2").arg(fcc_type, fcc_handler);

                model::ContainerStreamInfo si;
                si.index = result.streams.size();
                if (fcc_type == "vids") si.type = "video";
                else if (fcc_type == "auds") si.type = "audio";
                else if (fcc_type == "txts" || fcc_type == "subs") si.type = "subtitle";
                else si.type = fcc_type;
                si.codec = fcc_handler.trimmed();
                result.streams.append(si);

                cur_strh_type = fcc_type;
                cur_stream_idx = si.index;
            }
            // 对齐
            qint64 next_pos = chunk_offset + 8 + chunk_size;
            if (chunk_size % 2 != 0) next_pos++;
            if (next_pos <= file.size()) file.seek(next_pos);
        } else if (fourcc == "strf") {
            // 流格式头: 视频=BITMAPINFOHEADER, 音频=WAVEFORMATEX
            elem.type = "Stream Format";
            QByteArray data = file.read(chunk_size);
            if (cur_strh_type == "vids" && data.size() >= 40) {
                uint32_t biWidth  = readLE32(data, 4);
                int32_t  biHeight = static_cast<int32_t>(readLE32(data, 8));
                uint16_t biBitCount = readLE16(data, 14);
                QByteArray comp = data.mid(16, 4);
                QString fourccStr = QString::fromLatin1(comp).trimmed();
                bool rawRgb = (readLE32(data, 16) == 0);
                QString codecName = rawRgb ? QString("RGB") : fourccStr;
                elem.value = QString("%1x%2 %3bit codec=%4")
                                 .arg(biWidth).arg(qAbs(biHeight))
                                 .arg(biBitCount).arg(codecName);
                if (cur_stream_idx >= 0 && cur_stream_idx < result.streams.size()) {
                    auto& s = result.streams[cur_stream_idx];
                    if (!codecName.isEmpty()) s.codec = codecName;
                    s.details = QString("%1x%2, %3-bit")
                                    .arg(biWidth).arg(qAbs(biHeight)).arg(biBitCount);
                }
            } else if (cur_strh_type == "auds" && data.size() >= 16) {
                uint16_t wFormatTag     = readLE16(data, 0);
                uint16_t nChannels      = readLE16(data, 2);
                uint32_t nSamplesPerSec = readLE32(data, 4);
                uint32_t nAvgBytesPerSec = readLE32(data, 8);
                uint16_t wBitsPerSample = readLE16(data, 14);
                QString codecName = waveFormatName(wFormatTag);
                elem.value = QString("%1 %2Hz %3ch %4bit %5kbps")
                                 .arg(codecName).arg(nSamplesPerSec).arg(nChannels)
                                 .arg(wBitsPerSample).arg(nAvgBytesPerSec * 8 / 1000);
                if (cur_stream_idx >= 0 && cur_stream_idx < result.streams.size()) {
                    auto& s = result.streams[cur_stream_idx];
                    s.codec = codecName;
                    s.details = QString("%1 Hz, %2 ch, %3-bit, %4 kbps")
                                    .arg(nSamplesPerSec).arg(nChannels)
                                    .arg(wBitsPerSample).arg(nAvgBytesPerSec * 8 / 1000);
                }
            } else {
                elem.value = QString("size=%1").arg(chunk_size);
            }
            qint64 next_pos = chunk_offset + 8 + chunk_size;
            if (chunk_size % 2 != 0) next_pos++;
            if (next_pos <= file.size()) file.seek(next_pos);
        } else {
            // 普通叶子块
            elem.type = "Chunk";
            elem.value = QString("size=%1").arg(chunk_size);

            // 跳过数据
            qint64 next_pos = chunk_offset + 8 + chunk_size;
            if (chunk_size % 2 != 0) next_pos++;
            if (next_pos <= file.size()) {
                file.seek(next_pos);
            } else {
                file.seek(file.size());
            }
        }

        parent.children.append(elem);
    }
    return true;
}

} // namespace analyzer
} // namespace videoeye
