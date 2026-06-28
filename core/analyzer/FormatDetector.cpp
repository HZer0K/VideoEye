#include "FormatDetector.h"
#include <QFile>
#include <QFileInfo>
#include <QByteArray>

namespace videoeye {
namespace analyzer {

model::ContainerFormat FormatDetector::DetectByMagic(const QString& file_path) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return model::ContainerFormat::Unknown;
    }

    QByteArray header = file.read(64);
    file.close();

    if (header.size() < 4) {
        return model::ContainerFormat::Unknown;
    }

    // MP4/MOV: offset 4-7 = "ftyp"
    if (header.size() >= 8 && header.mid(4, 4) == "ftyp") {
        // 区分 MP4 vs MOV: 检查 ftyp 后的 brand
        QByteArray brand = header.mid(8, 4);
        if (brand == "qt  " || brand == "mdat" || brand.startsWith("qt")) {
            return model::ContainerFormat::MOV;
        }
        return model::ContainerFormat::MP4;
    }

    // EBML (MKV/WebM): \x1A\x45\xDF\xA3
    if (header.size() >= 4 &&
        (unsigned char)header[0] == 0x1A &&
        (unsigned char)header[1] == 0x45 &&
        (unsigned char)header[2] == 0xDF &&
        (unsigned char)header[3] == 0xA3) {
        // 在头部区域搜索 DocType 来区分 MKV vs WebM
        if (header.contains("webm")) {
            return model::ContainerFormat::WebM;
        }
        return model::ContainerFormat::MKV;
    }

    // AVI: "RIFF" + 4 bytes size + "AVI "
    if (header.size() >= 12 &&
        header.left(4) == "RIFF" &&
        header.mid(8, 4) == "AVI ") {
        return model::ContainerFormat::AVI;
    }

    // FLV: "FLV"
    if (header.size() >= 3 && header.left(3) == "FLV") {
        return model::ContainerFormat::FLV;
    }

    // MPEG-TS: sync byte 0x47 at first byte (and possibly at 188, 376...)
    if (header.size() >= 189 &&
        (unsigned char)header[0] == 0x47 &&
        (unsigned char)header[188] == 0x47) {
        return model::ContainerFormat::MPEG_TS;
    }

    // ASF: Header Object GUID = 30 26 B2 75 8E 66 CF 11
    if (header.size() >= 8 &&
        (unsigned char)header[0] == 0x30 &&
        (unsigned char)header[1] == 0x26 &&
        (unsigned char)header[2] == 0xB2 &&
        (unsigned char)header[3] == 0x75 &&
        (unsigned char)header[4] == 0x8E &&
        (unsigned char)header[5] == 0x66 &&
        (unsigned char)header[6] == 0xCF &&
        (unsigned char)header[7] == 0x11) {
        return model::ContainerFormat::ASF;
    }

    // OGG: "OggS"
    if (header.size() >= 4 && header.left(4) == "OggS") {
        return model::ContainerFormat::OGG;
    }

    return model::ContainerFormat::Unknown;
}

model::ContainerFormat FormatDetector::DetectByExtension(const QString& file_path) {
    QString ext = QFileInfo(file_path).suffix().toLower();

    if (ext == "mp4" || ext == "m4v" || ext == "m4a" || ext == "f4v") {
        return model::ContainerFormat::MP4;
    }
    if (ext == "mov") {
        return model::ContainerFormat::MOV;
    }
    if (ext == "mkv") {
        return model::ContainerFormat::MKV;
    }
    if (ext == "webm") {
        return model::ContainerFormat::WebM;
    }
    if (ext == "avi") {
        return model::ContainerFormat::AVI;
    }
    if (ext == "flv" || ext == "f4v") {
        return model::ContainerFormat::FLV;
    }
    if (ext == "ts" || ext == "mts" || ext == "m2ts") {
        return model::ContainerFormat::MPEG_TS;
    }
    if (ext == "asf" || ext == "wmv" || ext == "wma") {
        return model::ContainerFormat::ASF;
    }
    if (ext == "ogg" || ext == "oga" || ext == "ogv" || ext == "opus") {
        return model::ContainerFormat::OGG;
    }

    return model::ContainerFormat::Unknown;
}

model::ContainerFormat FormatDetector::Detect(const QString& file_path) {
    // 优先用魔数检测
    auto fmt = DetectByMagic(file_path);
    if (fmt != model::ContainerFormat::Unknown) {
        return fmt;
    }
    // 回退到扩展名
    return DetectByExtension(file_path);
}

QString FormatDetector::FormatName(model::ContainerFormat fmt) {
    switch (fmt) {
    case model::ContainerFormat::MP4:            return "MP4";
    case model::ContainerFormat::MOV:            return "MOV";
    case model::ContainerFormat::MKV:            return "MKV";
    case model::ContainerFormat::WebM:           return "WebM";
    case model::ContainerFormat::AVI:            return "AVI";
    case model::ContainerFormat::FLV:            return "FLV";
    case model::ContainerFormat::MPEG_TS:        return "MPEG-TS";
    case model::ContainerFormat::ASF:            return "ASF";
    case model::ContainerFormat::OGG:            return "OGG";
    case model::ContainerFormat::FFmpeg_Generic: return "Generic";
    default:                                     return "Unknown";
    }
}

QString FormatDetector::FormatTitle(model::ContainerFormat fmt) {
    switch (fmt) {
    case model::ContainerFormat::MP4:            return "MP4 Box 结构";
    case model::ContainerFormat::MOV:            return "MOV (QuickTime) 结构";
    case model::ContainerFormat::MKV:            return "MKV (EBML) 结构";
    case model::ContainerFormat::WebM:           return "WebM (EBML) 结构";
    case model::ContainerFormat::AVI:            return "AVI (RIFF) 结构";
    case model::ContainerFormat::FLV:            return "FLV 结构";
    case model::ContainerFormat::MPEG_TS:        return "MPEG-TS 结构";
    case model::ContainerFormat::ASF:            return "ASF/WMV 结构";
    case model::ContainerFormat::OGG:            return "OGG 结构";
    case model::ContainerFormat::FFmpeg_Generic: return "FFmpeg 通用结构";
    default:                                     return "文件结构";
    }
}

} // namespace analyzer
} // namespace videoeye
