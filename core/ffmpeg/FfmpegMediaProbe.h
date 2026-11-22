#pragma once

// 「进出的素材到底是什么参数」—— 从原生 ffmpeg 的 stderr 文本里读出来。
//
// 为什么不链 libavformat：core/ffmpeg（ffmpegtools）这个 target 的定位就是"只调原生
// ffmpeg 可执行程序"（README / CMakeLists 里写死的边界）。真为了读一遍流信息就把
// 整层 libav 拉进链接图，等于给这个模块换了个身份。而 `ffmpeg -hide_banner -i <path>`
// 打印的那几行本来就带着全部事实：
//   容器别名 / 时长 / 起始时间 / 总码率
//   每条流：类型 / 编码 / profile / 语言 / 分辨率 / 像素格式 / 帧率 /
//           采样率 / 声道布局 / 采样格式 / 每流码率 / disposition
//
// ⚠️ 两个必须记住的行为：
//   1) `ffmpeg -i <file>` **一定以退出码 1 结束**（"At least one output file must be
//      specified"）。它把要说的都说完了才报这句。调用方**不能**按 FfmpegRunStatus
//      判成败，只能看这里有没有解析出 Input 块。
//   2) -i 前面会打印 banner（版本 / 编译选项），-hide_banner 可省掉；但即使带着，
//      这里也逐行按前缀匹配，不会被 banner 干扰。
//
// 解析口径（宁可缺一格，不许编）：
//   * 认不出的字段留空 / -1，界面显示 "—"；
//   * `N/A` 不是 0 —— 把 "N/A kb/s" 记成 0 会让对比图读成"码率归零"；
//   * `Stream #0:0 -> #0:0 (h264 (native) -> h264 (libx264))` 是**流映射行**，不是流
//     定义（含 " -> "），必须跳过，否则一条命令的映射日志会被当成几十条流；
//   * `(default)` / `(attached pic)` 这类尾部括号是 disposition，不是参数；
//   * 封面图（attached pic）在 mp4 里表现为一条 video 流，单独标记 —— 不标的话
//     "视频流 2 → 1" 会被读成"丢了一路视频"。

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace videoeye {
namespace ffmpeg {

enum class MediaStreamKind {
    Video,
    Audio,
    Subtitle,
    Data,
    Attachment,
    Unknown,
};

/// 一条流的事实快照。所有"没读到"一律是空串 / 0 / -1，不做默认值填充。
struct MediaStreamSnapshot {
    int index = -1;      // 全局流号（ffmpeg 的 "0:1" 里的 1；-map 用的就是它）
    int program = 0;     // "1:0" 里的 1；普通文件是 0
    QString input_label; // 原样的 "0:1"
    MediaStreamKind kind = MediaStreamKind::Unknown;
    QString kind_label; // 原样 "Video" / "Audio" / "Subtitle" / "Data" / "Attachment"
    QString codec;      // "h264" / "aac" / "ass"；认不出则空
    QString profile;    // "High" / "LC"；只有一层括号且不是 4CC 标签时才取
    QString language;   // "und" / "eng"；没有语言标记则空
    QString pix_fmt;    // "yuv420p"
    int width = 0;
    int height = 0;
    QString sample_aspect;     // "1:1"
    QString display_aspect;    // "16:9"
    double fps = 0.0;          // 0 = 没读到（不是 0 帧）
    int sample_rate = 0;       // 0 = 没读到
    QString channel_layout;    // "stereo" / "5.1(side)"
    QString sample_fmt;        // "fltp" / "s16"
    qint64 bitrate_bps = -1;   // 每流码率；-1 = 没读到
    QStringList dispositions;  // "default" / "attached pic" / "forced" …
    bool attached_pic = false; // 封面图：是 video 流但不参与"视频路数"对比
    QString raw;               // 原始整行，详情区可原样展示，便于核对

    bool has_size() const {
        return width > 0 && height > 0;
    }
};

/// 一个输入（`Input #0`）的整体事实。
struct MediaProbeSnapshot {
    bool parsed = false;            // 至少认出一个 Input 块
    QString error;                  // parsed=false 时的人话原因
    QString path;                   // 探测的路径（原样，可能是 URL）
    QString container;              // 容器别名表的**第一项**（"mov" / "matroska"）
    QString container_aliases;      // 别名表全文（"mov,mp4,m4a,3gp,3g2,mj2"）
    double duration_seconds = -1.0; // -1 = N/A（实时流 / 未知）
    double start_seconds = 0.0;
    qint64 bitrate_bps = -1;
    qint64 file_size_bytes = -1; // 只有本地文件才有（FillLocalFileFacts 填）
    std::vector<MediaStreamSnapshot> streams;

    int CountStreams(MediaStreamKind kind) const;
    /// 参与音视频对比的流数（排除封面图 —— 它只是元数据里的图）。
    int CountPlayableStreams(MediaStreamKind kind) const;
    /// 第一条某类型的流；没有返回 nullptr。
    const MediaStreamSnapshot* FirstStream(MediaStreamKind kind) const;
};

/// `ffmpeg -hide_banner -nostdin -i <path>` 的参数数组。
/// -nostdin 让它别等着读 stdin（页面里这条命令是"查完就退"的）。
QStringList BuildMediaProbeArguments(const QString& path);

/// 从 stderr 全文解析。path 只用于回填 snapshot.path（不影响解析结果）。
/// 多个 Input 块时**只取第一个** —— 这一层服务的是"这条命令进出的那个素材"，
/// 多输入命令的合并由调用方按输入组逐个探测。
///
/// ⚠️ 只接受 `ffmpeg -i <一个文件>` 的输出。把**转码日志**喂进来不会报错 ——
/// 它里面的第一个 Input 块确实描述了一个文件，但那是"输入"，不是"输出"。
/// 所以调用方必须自己保证：探测一律用 BuildMediaProbeArguments 起一条独立进程
/// （实测踩过：拿转码日志当输出探测，整张图的实测列都在说输入）。
MediaProbeSnapshot ParseMediaProbe(const QString& stderr_text, const QString& path = QString());

/// 本地文件补一项实测大小（URL / 管道 / 不存在的文件一律保持 -1）。
/// 单独成函数是为了让 ParseMediaProbe 保持纯文本、可单测。
void FillLocalFileFacts(MediaProbeSnapshot* snapshot);

// ---- 人读格式（图谱与详情共用一份，别各写一套舍入）----
QString FormatDurationHuman(double seconds);          // 83.45 → "1:23.5"；<0 → "—"
QString FormatBitrateHuman(qint64 bps);               // 5940000 → "5.94 Mbps"；<0 → "—"
QString FormatFpsHuman(double fps);                   // 29.97 → "29.97 fps"；0 → "—"
QString FormatResolutionHuman(int width, int height); // "1920×1080"；0 → "—"
QString MediaStreamKindName(MediaStreamKind kind);    // "视频" / "音频" …
/// 一条流的一行摘要：`h264 High · 1920×1080 · 29.97 fps · 5.87 Mbps`
QString DescribeStreamSummary(const MediaStreamSnapshot& stream);

} // namespace ffmpeg
} // namespace videoeye
