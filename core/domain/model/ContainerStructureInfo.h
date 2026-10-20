#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/domain/model/ContainerDiagnosticsInfo.h"
#include "core/domain/model/Mp4BoxInfo.h"
#include "core/domain/model/Mp4SampleInfo.h"
#include "core/domain/model/EbmlInfo.h"
#include "core/domain/model/StreamPackageInfo.h"
#include "core/domain/model/TransportStreamInfo.h"

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
    // MPEG-PS（.mpg/.vob/.m2p）：与 TS 同属 MPEG-2 System，但容器结构完全不同
    // （pack header + PES，没有固定包长、没有 PSI）。
    MPEG_PS,
    // MXF（Material Exchange Format）：SMPTE 377M 的 KLV 容器，广电/相机素材常用。
    MXF,
    // 裸流（raw elementary stream）：H.264/HEVC 的 Annex-B、AV1 OBU 等。
    // 没有容器层可解析，结构页直接回退 FFmpeg 通用分析。
    RAW_ES,
    // 流媒体清单（纯文本，魔数识别不出来，只能靠扩展名）：
    // HLS 的 .m3u8 与 DASH 的 .mpd。二者都不是"容器"而是分片索引，
    // 但仍然走 ContainerStructureAnalyzer 的分发，由 StreamingPackage* 系列解析。
    HLS,
    DASH,
    FFmpeg_Generic
};

/// 通用容器结构树节点
struct ContainerElement {
    std::string name;           // 元素名 (如 "moov", "RIFF LIST", "FLV Tag")
    std::string type;           // 类型标识 (如 "Box", "EBML", "Chunk", "Tag")
    uint64_t size = 0;
    uint64_t offset = 0;
    int depth = 0;
    std::string value;          // 叶子节点值
    std::string extra;          // 额外信息
    std::vector<ContainerElement> children;
};

/// 通用流信息
struct ContainerStreamInfo {
    int index = 0;
    std::string type;           // "video", "audio", "subtitle", "data"
    std::string codec;
    std::string details;        // 分辨率/采样率等
    // 同一轨道 stsd 里声明的全部 sample entry（codec 为第一个）。
    // 单描述文件里它只有一项；Dolby Vision 双层等会出现 avc1 + dvhe 这样的组合。
    // 放在末尾：既有调用点大多用 {index, type, codec, details} 聚合初始化。
    std::vector<std::string> codecs;
};

/// 统一容器结构分析结果
struct ContainerStructureResult {
    ContainerFormat format = ContainerFormat::Unknown;
    std::string format_name;        // "MP4", "MKV", "AVI" 等
    std::string file_path;
    std::vector<ContainerElement> element_tree;     // 通用结构树
    std::vector<ContainerStreamInfo> streams;       // 流信息
    std::map<std::string, std::string> metadata;    // 元数据键值对
    std::string summary;                            // 概要文本
    bool valid = false;
    std::string error_message;

    // 保留原有详细结果 (MP4/MKV 专用, 用于显示详细表格)
    Mp4BoxAnalysisResult mp4_detail;
    EbmlAnalysisResult ebml_detail;

    // MP4/fMP4 样本级一致性（sample table / elst / moof-traf-trun / faststart）
    // 由 ContainerStructureAnalyzer 在 MP4 家族时顺带跑一遍（自研 IsobmffParser 解析）
    Mp4SampleTableResult mp4_samples;

    // HLS / DASH 流媒体包（manifest + segment + 多码率 ladder）。
    // 只有 format == HLS / DASH 时才填充，见 core/analysis/streaming/HlsManifestAnalyzer.h。
    StreamingPackageResult streaming_package;

    // MPEG-TS 传输层摘要（失步 / 连续计数 / PCR / 加扰）。
    // 只有 format == MPEG_TS 且解析成功时才填充；全文件扫描（AnalysisEngine）会把
    // 同一份摘要搬进 AnalysisResult::ts_transport，供 container.ts.* 规则使用。
    TransportStreamInfo transport;

    // AVI / FLV / ASF / OGG 的容器级体检事实（索引、时间戳、页/EOS、对象大小等）。
    // 结构与 QC 共用同一份数字，全文件扫描时搬进 AnalysisResult::container_diagnostics。
    ContainerDiagnosticsInfo diagnostics;
};

} // namespace model
} // namespace videoeye
