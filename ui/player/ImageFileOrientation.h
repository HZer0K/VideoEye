#pragma once

// 从本地图片文件读"显示方向"（只读文件头，不解码像素）。
//
// 为什么单独成头：单图显示（PlayerPanel）与图片序列预览（ImageSequencePlayer）都要
// 在把帧交给 VideoWidget 之前套一次方向；如果各写一份，"序列里第一张是竖的、翻到
// 下一张就躺下"这种不一致只会出现在其中一条路径上，测不到也看不出来。
//
// EXIF 方向只影响"显示"：解码出来的像素本身不旋转（分析/导出仍拿原始像素）。
// 方向由 core/domain/model/ImageOrientation 自己解析（JPEG/TIFF/PNG/WebP/HEIF），
// 不依赖 Qt 的图片插件 —— 本仓库只部署了 qgif/qico/qjpeg，用 QImageReader 的
// autoTransform 读 TIFF/WebP 方向永远拿不到值，等于假支持。

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstddef>

#include "core/domain/model/ImageOrientation.h"

namespace videoeye {
namespace ui {

// 读方向元数据时最多扫这么多字节：JPEG 的 APP1、PNG 的 eXIf、WebP 的 EXIF 块、
// TIFF 的 IFD0、HEIF 的 irot 都在头部；再大的图也没必要为方向把整个文件读进来。
inline constexpr qint64 kOrientationScanBytes = 8 * 1024 * 1024;

// 读不到（文件打不开 / 格式没有方向信息）时返回 None，调用方据此跳过变换。
inline model::ImageOrientation ReadImageOrientationFromFile(const QString& path) {
    if (path.isEmpty()) {
        return model::ImageOrientation::None;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return model::ImageOrientation::None;
    }
    const QByteArray head = file.read(kOrientationScanBytes);
    return model::ReadImageOrientation(head.constData(), static_cast<std::size_t>(head.size()));
}

} // namespace ui
} // namespace videoeye
