#include "AviStructureAnalyzer.h"
#include <QFile>
#include <QDataStream>
#include <QByteArray>

namespace videoeye {
namespace analyzer {

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
                si.codec = fcc_handler;
                result.streams.append(si);
            }
            // 对齐
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
