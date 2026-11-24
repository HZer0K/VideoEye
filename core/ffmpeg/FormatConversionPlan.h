#pragma once

// 「音视频格式转换」的参数 → argv 翻译层（纯逻辑，不依赖 QWidget）。
//
// 为什么单独一层：这一页要回答的问题全都不是界面问题 ——
//   * 目标容器收不收这种编码（webm 只吃 VP8/VP9/AV1，不接受 H.264）；
//   * 音频容器上还留着"视频编码 = H.265"时该忽略还是该报错；
//   * 流复制（-c copy）遇到目标容器不兼容，是拦下来还是放行；
//   * 只有 mp4 / mov 才该加 -movflags +faststart，加到 .ts 上 ffmpeg 直接报错。
// 这些规则放进 QWidget 就只能靠手点界面验证，所以放在 core 里由单测逐条钉住。
//
// 三条刻意的边界：
//   1. **不查磁盘、不发进程**：输入是否存在、ffmpeg 有没有装，由页面用
//      PreflightCommand / FfmpegToolLocator 各自处理。这里只负责"参数对不对"。
//   2. **源流信息是可选输入**：不给 source 时按"未知"处理，兼容性只能给警告；
//      给了才能把"把 H.264 流复制进 webm"这种必然失败判成错误（见 Compat）。
//   3. **不生成 -y 之外的隐式答应**：覆盖由页面先问用户，命令里统一带 -y，
//      免得 ffmpeg 停在 "File exists. Overwrite?" 上，把 runner 一直挂着。

#include <QString>
#include <QStringList>

#include <vector>

namespace videoeye {
namespace ffmpeg {

/// 目标容器。顺序即界面下拉顺序：先视频容器（常用在前），后音频容器。
enum class ConversionContainer {
    Mp4,
    Mkv,
    Mov,
    Webm,
    Avi,
    Ts,
    Flv,
    Mp3,
    M4a,
    Wav,
    Flac,
    Ogg,
    Aac,
    Gif,
};

enum class ConversionVideoCodec {
    Auto, // 由容器决定（mp4/mkv/mov/ts/flv → H.264，webm → VP9，avi → MPEG-4，gif → 调色板）
    Copy, // 流复制：不重新编码，画质不变、极快，但必须与容器兼容
    H264,
    H265,
    Vp9,
    Av1,
    Mpeg4, // 老 avi 的唯一实用选择（DivX/Xvid 播放器都认）
    None,  // 不要视频轨（纯音频输出或丢弃画面）
};

enum class ConversionAudioCodec {
    Auto, // 由容器决定（mp4/mov/m4a/ts/flv → AAC，webm/ogg → Opus，avi → MP3，wav → PCM，flac → FLAC）
    Copy,
    Aac,
    Mp3,
    Opus,
    Flac,
    Pcm, // pcm_s16le，给 wav 用
    None,
};

/// 质量档。CRF 的具体值按编码器分别映射（见 CrfFor），因为三家的取值范围与
/// "肉眼无差别"的位置都不一样：x264/x265 的 18 和 libvpx 的 18 完全不是一回事。
enum class ConversionQuality {
    High,
    Balanced,
    Small,
    Custom, // 用 profile.crf
};

enum class ConversionScale {
    Keep,
    P1080, // 1920×1080（只缩不放：短边按 -2 自动算，保持偶数）
    P720,
    P480,
};

enum class ConversionFrameRate {
    Keep,
    Fps60,
    Fps30,
    Fps24,
};

/// 源素材的事实（可选）。页面从 FfmpegMediaProbe 的快照里填，未探测时保持
/// known=false —— 这时兼容性只能给警告，不能断言"必然失败"。
struct ConversionSourceFacts {
    bool known = false;
    bool has_video = false;
    bool has_audio = false;
    QString video_codec; // "h264" / "hevc" / "vp9" / "av1" …（ffmpeg 的短名）
    QString audio_codec; // "aac" / "mp3" / "opus" …
};

struct ConversionProfile {
    ConversionContainer container = ConversionContainer::Mp4;
    ConversionVideoCodec video = ConversionVideoCodec::Auto;
    ConversionAudioCodec audio = ConversionAudioCodec::Auto;
    ConversionQuality quality = ConversionQuality::Balanced;
    /// 仅 ConversionQuality::Custom 时生效；其它档位由 CrfFor 决定。
    int crf = 23;
    /// 0 = 自动（交给编码器）。流复制时忽略。
    int audio_bitrate_kbps = 0;
    ConversionScale scale = ConversionScale::Keep;
    ConversionFrameRate fps = ConversionFrameRate::Keep;
};

struct ConversionPlan {
    bool ok = false;
    /// ok=false 时的原因（人话，直接可显示）。
    QString error;
    /// 能跑但用户该知道的事（例如"流复制不重新编码，质量设置不生效"）。
    QStringList warnings;
    /// 完整 argv（不含 ffmpeg 程序本身）。第一个元素之后紧跟 -i 与输入路径。
    QStringList arguments;
    /// 目标扩展名（不含点），页面用它同步默认输出名。
    QString extension;
};

/// 构建一条转换命令。output_path 为空时**仍然**能构建：自动用
/// DefaultConversionOutputPath 推一个默认输出名，便于页面在用户还没填输出时预览命令。
/// 但输入为空、或输出与输入是同一个文件（同一路径写两次会先截断输入，等于把源文件毁了）
/// 一律判 error。
ConversionPlan BuildConversionPlan(const ConversionProfile& profile, const QString& input_path,
                                   const QString& output_path,
                                   const ConversionSourceFacts& source = ConversionSourceFacts());

// ---- 界面用的能力表 ----
// 下拉框该列哪些项、某一项能不能选，全部以这里为准，避免 UI 另写一份规则。

std::vector<ConversionContainer> AllConversionContainers();
/// 该容器要不要视频轨。false 时页面应把视频编码锁成 None。
bool ContainerHasVideo(ConversionContainer container);
/// 该容器能装的视频编码（容器不收的编码不会出现在列表里）。
std::vector<ConversionVideoCodec> SupportedVideoCodecs(ConversionContainer container);
std::vector<ConversionAudioCodec> SupportedAudioCodecs(ConversionContainer container);

QString ContainerExtension(ConversionContainer container); // "mp4"
QString ContainerLabel(ConversionContainer container);     // "MP4（最通用）"
QString VideoCodecLabel(ConversionVideoCodec codec);       // "H.264（兼容性最好）"
QString AudioCodecLabel(ConversionAudioCodec codec);       // "AAC"
/// 实际传给 ffmpeg 的编码器名；Copy / None 返回空。
QString VideoEncoderName(ConversionVideoCodec codec); // "libx264"
QString AudioEncoderName(ConversionAudioCodec codec); // "aac"

/// 把 Auto 落到具体编码（页面用来显示"这一档实际会用什么"）。
/// 视频容器上的 Auto 与音频容器上的 Auto 是两回事，所以必须带容器参数；
/// 音频容器（mp3/wav/…）上的视频 Auto 一律落到 None。
ConversionVideoCodec ResolveVideoCodec(ConversionContainer container, ConversionVideoCodec codec);
ConversionAudioCodec ResolveAudioCodec(ConversionContainer container, ConversionAudioCodec codec);

/// 由输入路径推默认输出路径：同目录、`<基名>.<新扩展名>`，并与输入路径**不同**
/// （同名只改扩展名时补一个 `_converted` 后缀）。输入为空 / 不是本地文件时返回空。
QString DefaultConversionOutputPath(const QString& input_path, ConversionContainer container);

/// 两个路径是否指向同一个文件（Windows 大小写不敏感，比较前统一规范化）。
/// 输入与输出相同必须拒绝：ffmpeg 会先截断输出，而它就是输入。
bool IsSameLocalFile(const QString& a, const QString& b);

} // namespace ffmpeg
} // namespace videoeye
