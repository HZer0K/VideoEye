#pragma once

#include <cstddef>
#include <cstdint>

namespace videoeye::model {

// EXIF Orientation（1..8）的语义化表示。
//
// 命名描述的是"显示时要把像素怎么摆"，而不是 EXIF 的原始 1..8 编号 ——
// 调用方（UI）不需要再记一遍编号含义，也避免把 5/7 这类低频值写反。
// 数值仍与 EXIF 一一对应，便于日志里直接打印原始值。
enum class ImageOrientation : std::uint8_t {
    None = 1,             // 1: 原样
    MirrorHorizontal = 2, // 2: 水平镜像（左右翻）
    Rotate180 = 3,        // 3: 旋转 180
    MirrorVertical = 4,   // 4: 垂直镜像（上下翻）
    Transpose = 5,        // 5: 沿主对角线翻转（转置）
    Rotate90Cw = 6,       // 6: 顺时针 90（手机竖拍最常见）
    Transverse = 7,       // 7: 沿副对角线翻转（反转置）
    Rotate270Cw = 8,      // 8: 顺时针 270（= 逆时针 90）
};

// 从文件头字节里读方向；无 EXIF / 容器不认识 / 数据被截断 → ImageOrientation::None。
//
// 支持的容器（都只读元数据，不解码像素）：
//   * JPEG：SOI 之后的 APP1 段，"Exif\0\0" 后的 TIFF 结构
//   * TIFF：原生 II*\0 / MM\0* 头，IFD0 的 0x0112
//   * PNG：eXIf 块（TIFF 结构，规范上不带 "Exif\0\0" 前缀，容忍带了）
//   * WebP：RIFF 里的 EXIF 块（同上，容忍 "Exif\0\0" 前缀）
//   * HEIF / AVIF（ISOBMFF）：meta 里的 irot 属性盒（逆时针角度，单位 90°）
//
// 之所以不用 Qt 的 QImageReader::transformation()：那需要部署对应格式的图片插件
// （本仓库只部署了 qgif/qico/qjpeg），TIFF 没有 qtiff 插件时永远返回单位变换，
// 等于"支持 TIFF"这句话是假的；PNG / WebP 的 EXIF 也取决于插件实现。
// 自己解析只需要一段字节，不依赖插件，且可以脱离文件系统单测。
//
// 只读 IFD0：EXIF 规范要求 Orientation 落在 IFD0，缩略图 IFD1 里的同名标签不算。
ImageOrientation ReadImageOrientation(const std::uint8_t* data, std::size_t size);

// 便捷重载：把裸字节视图交给上面的实现。
inline ImageOrientation ReadImageOrientation(const char* data, std::size_t size) {
    return ReadImageOrientation(reinterpret_cast<const std::uint8_t*>(data), size);
}

} // namespace videoeye::model
