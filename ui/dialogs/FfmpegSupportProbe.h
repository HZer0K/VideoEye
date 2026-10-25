#pragma once

// 从**本进程链接的 FFmpeg 库**里现场枚举"这个 VideoEye 到底能读写什么"。
//
// 为什么不能写死一张清单: 同一份代码在不同机器上链到的 FFmpeg 构建可以完全不同
// (GPL 构建带 libx264、LGPL 构建不带；Windows 预编译包 vs Linux 发行版包同样是两套
// 编解码器表)。写死的清单只会骗人 —— 所以帮助页里每一行都由 av_*_iterate() 问出来。
//
// 与 core/ffmpeg 的 FfmpegCapabilityCache 的区别(别混用):
//   * FfmpegCapabilityCache —— 问**外部 ffmpeg 可执行文件**(`ffmpeg -encoders`)，
//     用来校验命令行工作台里写的参数在这台机器的 ffmpeg 上有没有；
//   * 这里 —— 问**本进程**链接的 av* 库，用来回答"VideoEye 自己能打开/导出什么"。
// 两者来源不同、结论也不同（外部 ffmpeg 可能是 6.1，本进程链的是 8.1）。
//
// 只有这一个 .cpp 碰 libav*，UI 侧拿到的是纯 QString 的结果类型。

#include <QString>
#include <vector>

namespace videoeye {
namespace ui {

// 流的类型。刻意不直接暴露 AVMediaType: 这一层对外给的是 UI 用的枚举，不该把
// libavutil 的头文件传染给每个 include 它的地方。
enum class SupportMediaType {
    Unknown,
    Video,
    Audio,
    Subtitle,
    Data,      // 数据流 (未知用途的私有流)
    Attachment // 附件 (字体 / 封面图)
};

// 一个编解码器（按名字归并: 同名的解码器与编码器合成一行）
struct CodecSupportEntry {
    QString name;      // "h264" / "libx264"
    QString long_name; // "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"
    SupportMediaType type = SupportMediaType::Unknown;
    bool can_decode = false;
    bool can_encode = false;
    bool intra_only = false;   // 只有帧内编码（ProRes / MJPEG 一类）
    bool lossless = false;     // 支持无损
    bool experimental = false; // AV_CODEC_CAP_EXPERIMENTAL: 默认不会被自动选中
    QString wrapper;           // 实现: 空 = FFmpeg 原生; 否则是外部库名 (libx264 / ...)
};

// 一个封装格式（按名字归并: 能读与能写合成一行）
struct ContainerSupportEntry {
    // FFmpeg 的短名本身可能是逗号分隔的别名表（mov 解复用器就是
    // "mov,mp4,m4a,3gp,3g2,mj2"）—— 原样保留，用户搜 "mp4" 时也要能命中它。
    QString name;
    QString long_name;
    QString extensions; // 逗号分隔 ("mp4,m4a,3gp,3g2,mj2")
    QString mime_type;
    bool can_demux = false; // 能读（解复用）
    bool can_mux = false;   // 能写（复用）
};

// 一个协议（file / http / rtmp ...）
struct ProtocolSupportEntry {
    QString name;
    bool can_read = false;
    bool can_write = false;
};

struct HwAccelEntry {
    QString name;          // "cuda" / "d3d11va"
    QString description;   // 中文说明（认识的类型才给，其余回落到原名）
    int decoder_count = 0; // 声明支持该设备类型的解码器数量
};

struct FfmpegBuildInfo {
    QString version;       // av_version_info()
    QString configuration; // 编译时的 configure 行
    QString license;
    // (库名, 版本串): libavutil / libavcodec / libavformat / libswscale / libswresample
    std::vector<std::pair<QString, QString>> libraries;
};

struct FfmpegSupportSnapshot {
    FfmpegBuildInfo build;
    std::vector<CodecSupportEntry> codecs;
    std::vector<ContainerSupportEntry> containers;
    std::vector<ProtocolSupportEntry> protocols;
    std::vector<HwAccelEntry> hwaccels;

    int DemuxerCount() const;
    int MuxerCount() const;
    int DecoderCount() const;
    int EncoderCount() const;
};

/// 类型的显示名（"视频" / "音频" / ...）
QString SupportMediaTypeName(SupportMediaType type);

/// 现场向本进程链接的 FFmpeg 库问一遍能力。只读、无副作用、毫秒级，
/// 可以放心在 UI 线程上直接调（量级: 二百来个编解码器 + 四百来个格式）。
FfmpegSupportSnapshot CollectFfmpegSupport();

} // namespace ui
} // namespace videoeye
