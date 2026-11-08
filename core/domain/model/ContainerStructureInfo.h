#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/domain/io/MediaInputPreflight.h"
#include "core/domain/model/ContainerDiagnosticsInfo.h"
#include "core/domain/model/EbmlInfo.h"
#include "core/domain/model/Mp4BoxInfo.h"
#include "core/domain/model/Mp4SampleInfo.h"
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

/// 结构分析「失败原因」的结构化类别。
///
/// 以前只有一个 error_message 字符串：零字节、路径写错、格式不支持、被取消、
/// FFmpeg 打开失败全都落一句「FFmpeg 无法打开文件」。文本能给人看，但机器无法
/// 用来做结论门禁 —— 「空表/失败到底是哪一种原因」又变成靠猜。
/// 这里把原因单独落成一个键，与 error_message 分开：文案可以改，键不能改。
///
/// ⚠️ 守恒契约（由 test_container_failure_kind 钉死）：
///   valid == true  -> 必须是 None；
///   valid == false -> 必须不是 None，且 error_message 非空。
enum class ContainerFailureKind {
    None,
    EmptyPath,
    NotFound,
    IsDirectory,
    EmptyFile,
    AllocationFailed,
    OpenFailed,
    StreamInfoFailed,
    Cancelled,
    ParseFailed,
};

inline const char* ContainerFailureKindName(ContainerFailureKind kind) {
    switch (kind) {
    case ContainerFailureKind::None:
        return "none";
    case ContainerFailureKind::EmptyPath:
        return "empty_path";
    case ContainerFailureKind::NotFound:
        return "not_found";
    case ContainerFailureKind::IsDirectory:
        return "is_directory";
    case ContainerFailureKind::EmptyFile:
        return "empty_file";
    case ContainerFailureKind::AllocationFailed:
        return "allocation_failed";
    case ContainerFailureKind::OpenFailed:
        return "open_failed";
    case ContainerFailureKind::StreamInfoFailed:
        return "stream_info_failed";
    case ContainerFailureKind::Cancelled:
        return "cancelled";
    case ContainerFailureKind::ParseFailed:
        return "parse_failed";
    }
    return "unknown";
}

// 预检失败 -> 结构分析失败类别。两边共用同一套分类，避免各写一份映射后漂移。
inline ContainerFailureKind ContainerFailureFromPreflight(InputPreflightKind kind) {
    switch (kind) {
    case InputPreflightKind::Ok:
        return ContainerFailureKind::None;
    case InputPreflightKind::EmptyPath:
        return ContainerFailureKind::EmptyPath;
    case InputPreflightKind::NotFound:
        return ContainerFailureKind::NotFound;
    case InputPreflightKind::IsDirectory:
        return ContainerFailureKind::IsDirectory;
    case InputPreflightKind::EmptyFile:
        return ContainerFailureKind::EmptyFile;
    }
    return ContainerFailureKind::ParseFailed;
}

/// 通用容器结构树节点
struct ContainerElement {
    std::string name; // 元素名 (如 "moov", "RIFF LIST", "FLV Tag")
    std::string type; // 类型标识 (如 "Box", "EBML", "Chunk", "Tag")
    uint64_t size = 0;
    uint64_t offset = 0;
    int depth = 0;
    std::string value; // 叶子节点值
    std::string extra; // 额外信息
    std::vector<ContainerElement> children;
};

/// 通用流信息
struct ContainerStreamInfo {
    int index = 0;
    std::string type; // "video", "audio", "subtitle", "data"
    std::string codec;
    std::string details; // 分辨率/采样率等
    // 同一轨道 stsd 里声明的全部 sample entry（codec 为第一个）。
    // 单描述文件里它只有一项；Dolby Vision 双层等会出现 avc1 + dvhe 这样的组合。
    // 放在末尾：既有调用点大多用 {index, type, codec, details} 聚合初始化。
    std::vector<std::string> codecs;
};

/// 统一容器结构分析结果
struct ContainerStructureResult {
    ContainerFormat format = ContainerFormat::Unknown;
    std::string format_name; // "MP4", "MKV", "AVI" 等
    std::string file_path;
    std::vector<ContainerElement> element_tree;  // 通用结构树
    std::vector<ContainerStreamInfo> streams;    // 流信息
    std::map<std::string, std::string> metadata; // 元数据键值对
    std::string summary;                         // 概要文本
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

    // 失败原因的结构化类别（成功时恒 None）。放在末尾：既有调用点大量用聚合初始化，
    // 追加字段不能打乱前面的位置。
    ContainerFailureKind failure_kind = ContainerFailureKind::None;
};

} // namespace model
} // namespace videoeye
