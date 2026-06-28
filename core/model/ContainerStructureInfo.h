#pragma once

#include <QString>
#include <QVector>
#include <QMap>
#include <QMetaType>
#include <cstdint>

#include "core/model/Mp4BoxInfo.h"
#include "core/model/EbmlInfo.h"

namespace videoeye {
namespace model {

/// 容器格式枚举
enum class ContainerFormat {
    Unknown,
    MP4,
    MOV,
    MKV,
    WebM,
    AVI,
    FLV,
    MPEG_TS,
    ASF,
    OGG,
    FFmpeg_Generic
};

/// 通用容器结构树节点
struct ContainerElement {
    QString name;           // 元素名 (如 "moov", "RIFF LIST", "FLV Tag")
    QString type;           // 类型标识 (如 "Box", "EBML", "Chunk", "Tag")
    uint64_t size = 0;
    uint64_t offset = 0;
    int depth = 0;
    QString value;          // 叶子节点值
    QString extra;          // 额外信息
    QVector<ContainerElement> children;
};

/// 通用流信息
struct ContainerStreamInfo {
    int index = 0;
    QString type;           // "video", "audio", "subtitle", "data"
    QString codec;
    QString details;        // 分辨率/采样率等
};

/// 统一容器结构分析结果
struct ContainerStructureResult {
    ContainerFormat format = ContainerFormat::Unknown;
    QString format_name;        // "MP4", "MKV", "AVI" 等
    QString file_path;
    QVector<ContainerElement> element_tree;     // 通用结构树
    QVector<ContainerStreamInfo> streams;       // 流信息
    QMap<QString, QString> metadata;            // 元数据键值对
    QString summary;                            // 概要文本
    bool valid = false;
    QString error_message;

    // 保留原有详细结果 (MP4/MKV 专用, 用于显示详细表格)
    Mp4BoxAnalysisResult mp4_detail;
    EbmlAnalysisResult ebml_detail;
};

} // namespace model
} // namespace videoeye

Q_DECLARE_METATYPE(videoeye::model::ContainerFormat)
Q_DECLARE_METATYPE(videoeye::model::ContainerElement)
Q_DECLARE_METATYPE(videoeye::model::ContainerStreamInfo)
Q_DECLARE_METATYPE(videoeye::model::ContainerStructureResult)
