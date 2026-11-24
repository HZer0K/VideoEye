#include "core/ffmpeg/FormatConversionPlan.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace videoeye {
namespace ffmpeg {
namespace {

/// 一个目标容器的能力表。**这是全模块唯一的容器能力事实源**：
/// 页面的下拉列表、兼容性提示、命令拼装都从这里读，避免三处各写一份然后慢慢漂移。
struct ContainerSpec {
    ConversionContainer container;
    const char* extension;
    const char* label;
    bool has_video; // 音频容器为 false
    bool faststart; // 只有 mp4 家族（mp4/mov/m4a）有意义，加到 .ts 上 ffmpeg 直接报错
    std::vector<ConversionVideoCodec> videos;
    std::vector<ConversionAudioCodec> audios;
    /// 能被**流复制**进这个容器的源编码（ffmpeg 短名）。空 = 不限制（matroska 什么都能装）。
    QStringList copy_video;
    QStringList copy_audio;
    ConversionVideoCodec auto_video; // Auto 落到哪一个
    ConversionAudioCodec auto_audio;
};

const std::vector<ContainerSpec>& SpecTable() {
    using V = ConversionVideoCodec;
    using A = ConversionAudioCodec;
    // 顺序即界面下拉顺序。常用容器在前，纯音频随后。
    static const std::vector<ContainerSpec> table = {
        {ConversionContainer::Mp4,
         "mp4",
         "MP4（最通用）",
         true,
         true,
         {V::Auto, V::Copy, V::H264, V::H265, V::Av1, V::None},
         {A::Auto, A::Copy, A::Aac, A::Mp3, A::Flac, A::None},
         {QStringLiteral("h264"), QStringLiteral("hevc"), QStringLiteral("av1"), QStringLiteral("mpeg4"),
          QStringLiteral("mjpeg")},
         {QStringLiteral("aac"), QStringLiteral("mp3"), QStringLiteral("ac3"), QStringLiteral("eac3"),
          QStringLiteral("alac"), QStringLiteral("flac")},
         V::H264,
         A::Aac},

        {ConversionContainer::Mkv,
         "mkv",
         "MKV（什么都能装）",
         true,
         false,
         {V::Auto, V::Copy, V::H264, V::H265, V::Vp9, V::Av1, V::Mpeg4, V::None},
         {A::Auto, A::Copy, A::Aac, A::Mp3, A::Opus, A::Flac, A::Pcm, A::None},
         // matroska 的容器开销小、codec 表几乎无限制，这里不设白名单。
         {},
         {},
         V::H264,
         A::Aac},

        {ConversionContainer::Mov,
         "mov",
         "MOV（苹果生态）",
         true,
         true,
         {V::Auto, V::Copy, V::H264, V::H265, V::None},
         {A::Auto, A::Copy, A::Aac, A::Mp3, A::Pcm, A::None},
         {QStringLiteral("h264"), QStringLiteral("hevc"), QStringLiteral("prores"), QStringLiteral("mjpeg"),
          QStringLiteral("mpeg4")},
         {QStringLiteral("aac"), QStringLiteral("mp3"), QStringLiteral("alac"), QStringLiteral("pcm_s16le"),
          QStringLiteral("pcm_s24le"), QStringLiteral("ac3")},
         V::H264,
         A::Aac},

        {ConversionContainer::Webm,
         "webm",
         "WebM（网页）",
         true,
         false,
         {V::Auto, V::Copy, V::Vp9, V::Av1, V::None},
         {A::Auto, A::Copy, A::Opus, A::None},
         // WebM 的规范只认这三个视频编码 —— H.264 能塞进 .webm 文件，但那已经不是 WebM 了。
         {QStringLiteral("vp8"), QStringLiteral("vp9"), QStringLiteral("av1")},
         {QStringLiteral("opus"), QStringLiteral("vorbis")},
         V::Vp9,
         A::Opus},

        {ConversionContainer::Avi,
         "avi",
         "AVI（老设备）",
         true,
         false,
         {V::Auto, V::Copy, V::Mpeg4, V::H264, V::None},
         {A::Auto, A::Copy, A::Mp3, A::Pcm, A::None},
         {QStringLiteral("mpeg4"), QStringLiteral("h264"), QStringLiteral("mjpeg")},
         {QStringLiteral("mp3"), QStringLiteral("pcm_s16le"), QStringLiteral("ac3")},
         V::Mpeg4,
         A::Mp3},

        {ConversionContainer::Ts,
         "ts",
         "TS（广播 / 录制）",
         true,
         false,
         {V::Auto, V::Copy, V::H264, V::H265, V::None},
         {A::Auto, A::Copy, A::Aac, A::Mp3, A::None},
         {QStringLiteral("h264"), QStringLiteral("hevc"), QStringLiteral("mpeg2video")},
         {QStringLiteral("aac"), QStringLiteral("mp3"), QStringLiteral("ac3")},
         V::H264,
         A::Aac},

        {ConversionContainer::Flv,
         "flv",
         "FLV（旧网页）",
         true,
         false,
         {V::Auto, V::Copy, V::H264, V::None},
         {A::Auto, A::Copy, A::Aac, A::Mp3, A::None},
         {QStringLiteral("h264")},
         {QStringLiteral("aac"), QStringLiteral("mp3")},
         V::H264,
         A::Aac},

        {ConversionContainer::Mp3,
         "mp3",
         "MP3（纯音频）",
         false,
         false,
         {V::Auto, V::None},
         {A::Auto, A::Copy, A::Mp3},
         {},
         {QStringLiteral("mp3")},
         V::None,
         A::Mp3},

        {ConversionContainer::M4a,
         "m4a",
         "M4A（纯音频）",
         false,
         true,
         {V::Auto, V::None},
         {A::Auto, A::Copy, A::Aac, A::None},
         {},
         {QStringLiteral("aac"), QStringLiteral("alac"), QStringLiteral("mp3")},
         V::None,
         A::Aac},

        {ConversionContainer::Wav,
         "wav",
         "WAV（无损 PCM）",
         false,
         false,
         {V::Auto, V::None},
         {A::Auto, A::Copy, A::Pcm},
         {},
         {QStringLiteral("pcm_*")}, // PCM 家族有 pcm_s16le / pcm_s24le / pcm_f32le 等十几个短名
         V::None,
         A::Pcm},

        {ConversionContainer::Flac,
         "flac",
         "FLAC（无损压缩）",
         false,
         false,
         {V::Auto, V::None},
         {A::Auto, A::Copy, A::Flac},
         {},
         {QStringLiteral("flac")},
         V::None,
         A::Flac},

        {ConversionContainer::Ogg,
         "ogg",
         "OGG（开源音频）",
         false,
         false,
         {V::Auto, V::None},
         {A::Auto, A::Copy, A::Opus},
         {},
         {QStringLiteral("opus"), QStringLiteral("vorbis")},
         V::None,
         A::Opus},

        {ConversionContainer::Aac,
         "aac",
         "AAC（裸流）",
         false,
         false,
         {V::Auto, V::None},
         {A::Auto, A::Copy, A::Aac},
         {},
         {QStringLiteral("aac")},
         V::None,
         A::Aac},

        // GIF 是个特例：唯一的"编码参数"是调色板，不是统一枚举能表达的。
        // 这里把选项收窄到 Auto，命令由 BuildConversionPlan 单独拼（见 Gif 分支）。
        {ConversionContainer::Gif, "gif", "GIF（动图）", true, false, {V::Auto}, {A::Auto}, {}, {}, V::None, A::None},
    };
    return table;
}

const ContainerSpec& Spec(ConversionContainer container) {
    const auto& table = SpecTable();
    for (const ContainerSpec& spec : table) {
        if (spec.container == container) {
            return spec;
        }
    }
    return table.front(); // 枚举值与表不一致是编译期问题，运行时退回 MP4 而不是崩
}

template <typename T>
bool Contains(const std::vector<T>& values, T value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

/// accepted 里允许 `pcm_*` 这种前缀写法：PCM 家族短名太多，逐个列举只会漏。
bool CopyCodecAccepted(const QStringList& accepted, const QString& codec) {
    if (accepted.isEmpty()) {
        return true; // 不限制
    }
    const QString name = codec.trimmed().toLower();
    if (name.isEmpty()) {
        return true;
    }
    for (const QString& entry : accepted) {
        if (entry.endsWith(QLatin1Char('*'))) {
            if (name.startsWith(entry.left(entry.size() - 1))) {
                return true;
            }
        } else if (name == entry) {
            return true;
        }
    }
    return false;
}

int CrfCeiling(ConversionVideoCodec codec) {
    switch (codec) {
    case ConversionVideoCodec::H264:
    case ConversionVideoCodec::H265:
        return 51;
    case ConversionVideoCodec::Vp9:
    case ConversionVideoCodec::Av1:
        return 63;
    default:
        return 51;
    }
}

/// 质量档 → CRF。三家的取值范围与"肉眼无差别"的位置都不一样，所以分别映射：
/// x264/x265 的 18 在 libvpx 上属于几乎无损（文件会大得离谱），而 libvpx 的 32
/// 大约等于 x264 的 23。libaom 比 libvpx 更"吃"CRF，同样观感要多给几点。
int CrfFor(ConversionVideoCodec codec, ConversionQuality quality, int custom) {
    if (quality == ConversionQuality::Custom) {
        return qBound(0, custom, CrfCeiling(codec));
    }
    switch (codec) {
    case ConversionVideoCodec::H264:
    case ConversionVideoCodec::H265:
        return quality == ConversionQuality::High ? 18 : (quality == ConversionQuality::Small ? 28 : 23);
    case ConversionVideoCodec::Vp9:
        return quality == ConversionQuality::High ? 24 : (quality == ConversionQuality::Small ? 40 : 32);
    case ConversionVideoCodec::Av1:
        return quality == ConversionQuality::High ? 24 : (quality == ConversionQuality::Small ? 42 : 32);
    default:
        return 23;
    }
}

/// MPEG-4 Part 2 不认 -crf，用 -q:v（2 = 最好，31 = 最差）。
/// Custom 档沿用用户的 crf 数字并夹到 2..31 —— 界面上同一个滑杆，语义按编码器走。
int QScaleFor(ConversionQuality quality, int custom) {
    if (quality == ConversionQuality::Custom) {
        return qBound(2, custom, 31);
    }
    return quality == ConversionQuality::High ? 2 : (quality == ConversionQuality::Small ? 8 : 4);
}

/// 音频码率。0 = 不写 -b:a（flac / pcm 这类无损编码本来就没有码率）。
int AudioBitrateKbps(ConversionAudioCodec codec, int requested) {
    if (requested > 0) {
        return requested;
    }
    switch (codec) {
    case ConversionAudioCodec::Aac:
        return 128;
    case ConversionAudioCodec::Mp3:
        return 192;
    case ConversionAudioCodec::Opus:
        // Opus 在 96k 就已经接近透明，给到 192k 纯属浪费（且老解码器可能不认高码率档）。
        return 96;
    default:
        return 0;
    }
}

int ScaleHeight(ConversionScale scale) {
    switch (scale) {
    case ConversionScale::P1080:
        return 1080;
    case ConversionScale::P720:
        return 720;
    case ConversionScale::P480:
        return 480;
    case ConversionScale::Keep:
        break;
    }
    return 0;
}

int FrameRateValue(ConversionFrameRate fps) {
    switch (fps) {
    case ConversionFrameRate::Fps60:
        return 60;
    case ConversionFrameRate::Fps30:
        return 30;
    case ConversionFrameRate::Fps24:
        return 24;
    case ConversionFrameRate::Keep:
        break;
    }
    return 0;
}

/// 路径里像不像"磁盘上的本地文件"。`C:\a` 里的 `C:` 不算协议，所以只看 `://`
/// 与 ffmpeg 的几个伪协议前缀（与 FfmpegCommandPreflight 的口径保持一致）。
bool LooksLikeLocalPath(const QString& path) {
    if (path.contains(QLatin1String("://"))) {
        return false;
    }
    const QString lower = path.toLower();
    for (const char* prefix : {"pipe:", "concat:", "subfile:", "lavfi:", "anullsrc=", "color=", "testsrc"}) {
        if (lower.startsWith(QLatin1String(prefix))) {
            return false;
        }
    }
    return true;
}

} // namespace

// ===================== 能力表 =====================

std::vector<ConversionContainer> AllConversionContainers() {
    std::vector<ConversionContainer> containers;
    for (const ContainerSpec& spec : SpecTable()) {
        containers.push_back(spec.container);
    }
    return containers;
}

bool ContainerHasVideo(ConversionContainer container) {
    return Spec(container).has_video;
}

std::vector<ConversionVideoCodec> SupportedVideoCodecs(ConversionContainer container) {
    return Spec(container).videos;
}

std::vector<ConversionAudioCodec> SupportedAudioCodecs(ConversionContainer container) {
    return Spec(container).audios;
}

QString ContainerExtension(ConversionContainer container) {
    return QString::fromLatin1(Spec(container).extension);
}

QString ContainerLabel(ConversionContainer container) {
    return QString::fromUtf8(Spec(container).label);
}

QString VideoCodecLabel(ConversionVideoCodec codec) {
    switch (codec) {
    case ConversionVideoCodec::Auto:
        return QStringLiteral("自动（按容器推荐）");
    case ConversionVideoCodec::Copy:
        return QStringLiteral("流复制（不重编码）");
    case ConversionVideoCodec::H264:
        return QStringLiteral("H.264");
    case ConversionVideoCodec::H265:
        return QStringLiteral("H.265 / HEVC");
    case ConversionVideoCodec::Vp9:
        return QStringLiteral("VP9");
    case ConversionVideoCodec::Av1:
        return QStringLiteral("AV1（慢）");
    case ConversionVideoCodec::Mpeg4:
        return QStringLiteral("MPEG-4");
    case ConversionVideoCodec::None:
        return QStringLiteral("不要视频");
    }
    return QString();
}

QString AudioCodecLabel(ConversionAudioCodec codec) {
    switch (codec) {
    case ConversionAudioCodec::Auto:
        return QStringLiteral("自动（按容器推荐）");
    case ConversionAudioCodec::Copy:
        return QStringLiteral("流复制（不重编码）");
    case ConversionAudioCodec::Aac:
        return QStringLiteral("AAC");
    case ConversionAudioCodec::Mp3:
        return QStringLiteral("MP3");
    case ConversionAudioCodec::Opus:
        return QStringLiteral("Opus");
    case ConversionAudioCodec::Flac:
        return QStringLiteral("FLAC（无损）");
    case ConversionAudioCodec::Pcm:
        return QStringLiteral("PCM（未压缩）");
    case ConversionAudioCodec::None:
        return QStringLiteral("不要音频");
    }
    return QString();
}

QString VideoEncoderName(ConversionVideoCodec codec) {
    switch (codec) {
    case ConversionVideoCodec::H264:
        return QStringLiteral("libx264");
    case ConversionVideoCodec::H265:
        return QStringLiteral("libx265");
    case ConversionVideoCodec::Vp9:
        return QStringLiteral("libvpx-vp9");
    case ConversionVideoCodec::Av1:
        return QStringLiteral("libaom-av1");
    case ConversionVideoCodec::Mpeg4:
        return QStringLiteral("mpeg4");
    default:
        return QString();
    }
}

QString AudioEncoderName(ConversionAudioCodec codec) {
    switch (codec) {
    case ConversionAudioCodec::Aac:
        return QStringLiteral("aac");
    case ConversionAudioCodec::Mp3:
        return QStringLiteral("libmp3lame");
    case ConversionAudioCodec::Opus:
        return QStringLiteral("libopus");
    case ConversionAudioCodec::Flac:
        return QStringLiteral("flac");
    case ConversionAudioCodec::Pcm:
        return QStringLiteral("pcm_s16le");
    default:
        return QString();
    }
}

ConversionVideoCodec ResolveVideoCodec(ConversionContainer container, ConversionVideoCodec codec) {
    if (codec != ConversionVideoCodec::Auto) {
        return codec;
    }
    const ContainerSpec& spec = Spec(container);
    // 纯音频容器上的"自动"就是"不要视频" —— 让调用方不必再判一次 has_video。
    return spec.has_video ? spec.auto_video : ConversionVideoCodec::None;
}

ConversionAudioCodec ResolveAudioCodec(ConversionContainer container, ConversionAudioCodec codec) {
    return codec == ConversionAudioCodec::Auto ? Spec(container).auto_audio : codec;
}

// ===================== 路径 =====================

bool IsSameLocalFile(const QString& a, const QString& b) {
    const QString left = a.trimmed();
    const QString right = b.trimmed();
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }
    if (!LooksLikeLocalPath(left) || !LooksLikeLocalPath(right)) {
        return false;
    }
    const QFileInfo info_left(left);
    const QFileInfo info_right(right);
    // 两边都真实存在时才用 canonicalFilePath（能穿透符号链接与 8.3 短名）；
    // 输出文件通常还不存在，那时只能比规范化后的绝对路径。
    const QString left_path =
        info_left.exists() ? info_left.canonicalFilePath() : QDir::cleanPath(info_left.absoluteFilePath());
    const QString right_path =
        info_right.exists() ? info_right.canonicalFilePath() : QDir::cleanPath(info_right.absoluteFilePath());
    if (left_path.isEmpty() || right_path.isEmpty()) {
        return false;
    }
#ifdef Q_OS_WIN
    return left_path.compare(right_path, Qt::CaseInsensitive) == 0;
#else
    return left_path == right_path;
#endif
}

QString DefaultConversionOutputPath(const QString& input_path, ConversionContainer container) {
    const QString trimmed = input_path.trimmed();
    if (trimmed.isEmpty() || !LooksLikeLocalPath(trimmed)) {
        return QString();
    }
    const QFileInfo info(trimmed);
    const QString base = info.completeBaseName(); // a.b.mp4 → "a.b"
    if (base.isEmpty()) {
        return QString();
    }
    const QString extension = ContainerExtension(container);
    const QString dir = info.absolutePath();

    QString name = base + QLatin1Char('.') + extension;
    if (IsSameLocalFile(info.absoluteFilePath(), QDir(dir).filePath(name))) {
        // 只改扩展名、其它一概不动的情况：`a.mp4` → `a.mp4`？不可能同名，但
        // 目标容器与源扩展名一致时（a.mp4 → mp4）确实会撞。加后缀区分，
        // 绝不让默认路径等于输入路径。
        name = base + QStringLiteral("_converted.") + extension;
    }
    return QDir(dir).filePath(name);
}

// ===================== 命令拼装 =====================

ConversionPlan BuildConversionPlan(const ConversionProfile& profile, const QString& input_path,
                                   const QString& output_path, const ConversionSourceFacts& source) {
    ConversionPlan plan;
    const ContainerSpec& spec = Spec(profile.container);
    plan.extension = QString::fromLatin1(spec.extension);

    const QString input = input_path.trimmed();
    if (input.isEmpty()) {
        plan.error = QStringLiteral("还没有选择输入文件");
        return plan;
    }

    QString output = output_path.trimmed();
    if (output.isEmpty()) {
        output = DefaultConversionOutputPath(input, profile.container);
    }
    if (output.isEmpty()) {
        plan.error = QStringLiteral("输出路径为空，且无法由输入自动生成（输入看起来不是本地文件）");
        return plan;
    }
    if (IsSameLocalFile(input, output)) {
        // ffmpeg 会先截断输出再读输入，同一路径等于把源文件毁了。
        plan.error = QStringLiteral("输出路径与输入是同一个文件，请换一个输出名");
        return plan;
    }

    // ---- GIF：唯一的参数是调色板，其余选项都不适用 ----
    if (profile.container == ConversionContainer::Gif) {
        const int fps = FrameRateValue(profile.fps) > 0 ? FrameRateValue(profile.fps) : 15;
        const int height = ScaleHeight(profile.scale) > 0 ? ScaleHeight(profile.scale) : 480;
        // 两遍法：先 palettegen 统计全片主色，再 paletteuse 上色。单链 scale 直接转 gif
        // 会出现大面积抖动与色带（256 色调色板是全局共享的，不能每帧各算一份）。
        const QString filter = QStringLiteral("[0:v]fps=%1,scale=-2:%2:flags=lanczos,split[a][b];"
                                              "[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer")
                                   .arg(fps)
                                   .arg(height);
        plan.warnings << QStringLiteral("GIF 用固定调色板转码：视频/音频编码与质量档位不生效（按 %1 fps、高 %2 采样）")
                             .arg(fps)
                             .arg(height);
        plan.arguments << QStringLiteral("-hide_banner") << QStringLiteral("-y") << QStringLiteral("-i") << input
                       << QStringLiteral("-filter_complex") << filter << QStringLiteral("-an")
                       << QStringLiteral("-loop") << QStringLiteral("0") << output;
        plan.ok = true;
        return plan;
    }

    const ConversionVideoCodec video = ResolveVideoCodec(profile.container, profile.video);
    const ConversionAudioCodec audio = ResolveAudioCodec(profile.container, profile.audio);

    if (!Contains(spec.videos, video)) {
        plan.error = QStringLiteral("「%1」不接受 %2 视频，请改选该容器支持的编码（或流复制）")
                         .arg(ContainerLabel(profile.container), VideoCodecLabel(video));
        return plan;
    }
    if (!Contains(spec.audios, audio)) {
        plan.error = QStringLiteral("「%1」不接受 %2 音频，请改选该容器支持的编码（或流复制）")
                         .arg(ContainerLabel(profile.container), AudioCodecLabel(audio));
        return plan;
    }
    if (video == ConversionVideoCodec::None && audio == ConversionAudioCodec::None) {
        plan.error = QStringLiteral("视频与音频都设成了「不要」，没有可写出的内容");
        return plan;
    }

    // ---- 流复制的兼容性 ----
    // 源信息已知时才敢断言"必然失败"；未知时只提示，不然会把本来能跑的组合拦下来。
    if (video == ConversionVideoCodec::Copy) {
        plan.warnings << QStringLiteral("流复制不重新编码：质量档位、CRF、缩放与帧率都不生效");
        if (source.known) {
            if (!source.has_video) {
                plan.warnings << QStringLiteral("源文件没有视频轨，-c:v copy 会被 ffmpeg 忽略");
            } else if (!CopyCodecAccepted(spec.copy_video, source.video_codec)) {
                plan.error = QStringLiteral("源视频编码 %1 无法流复制进「%2」，请改选重编码")
                                 .arg(source.video_codec, ContainerLabel(profile.container));
                return plan;
            }
        } else {
            plan.warnings << QStringLiteral("已选流复制：只有源编码被目标容器接受时才成立，否则 ffmpeg 会报错");
        }
    }
    if (audio == ConversionAudioCodec::Copy) {
        if (source.known) {
            if (!source.has_audio) {
                plan.warnings << QStringLiteral("源文件没有音频轨，-c:a copy 会被 ffmpeg 忽略");
            } else if (!CopyCodecAccepted(spec.copy_audio, source.audio_codec)) {
                plan.error = QStringLiteral("源音频编码 %1 无法流复制进「%2」，请改选重编码")
                                 .arg(source.audio_codec, ContainerLabel(profile.container));
                return plan;
            }
        } else {
            plan.warnings << QStringLiteral("已选流复制：只有源音频编码被目标容器接受时才成立");
        }
    }
    // 滤镜会强制解码再编码，与 -c:v copy 互斥（ffmpeg 会直接拒绝这个组合）。
    if (video == ConversionVideoCodec::Copy &&
        (profile.scale != ConversionScale::Keep || profile.fps != ConversionFrameRate::Keep)) {
        plan.error = QStringLiteral("流复制不能与缩放 / 改帧率同时使用：请改选重编码，或把两项设回原样");
        return plan;
    }

    QStringList& args = plan.arguments;
    args << QStringLiteral("-hide_banner") << QStringLiteral("-y") << QStringLiteral("-i") << input;
    // 只映射音视频流：字幕 / 数据 / 附件在跨容器时是最容易把整条命令搞失败的一类
    // （最典型：mkv 的 ASS 字幕往 mp4 里塞，ffmpeg 直接 "Subtitle codec not supported"）。
    // `?` 让缺失的那一路不报错 —— 纯音频源也要能转。
    args << QStringLiteral("-map") << QStringLiteral("0:v?") << QStringLiteral("-map") << QStringLiteral("0:a?");

    // ---- 视频 ----
    if (video == ConversionVideoCodec::Copy) {
        args << QStringLiteral("-c:v") << QStringLiteral("copy");
    } else if (video == ConversionVideoCodec::None) {
        args << QStringLiteral("-vn");
    } else {
        args << QStringLiteral("-c:v") << VideoEncoderName(video);
        if (video == ConversionVideoCodec::Mpeg4) {
            args << QStringLiteral("-q:v") << QString::number(QScaleFor(profile.quality, profile.crf));
        } else {
            args << QStringLiteral("-crf") << QString::number(CrfFor(video, profile.quality, profile.crf));
            if (video == ConversionVideoCodec::Vp9 || video == ConversionVideoCodec::Av1) {
                // VP9 / AV1 的 -crf 必须配 -b:v 0 才是"恒定质量"；只给 -crf 时
                // 默认的目标码率仍然生效，出来的文件会跟设定的质量对不上。
                args << QStringLiteral("-b:v") << QStringLiteral("0");
            }
            if (video == ConversionVideoCodec::H264 || video == ConversionVideoCodec::H265) {
                args << QStringLiteral("-preset") << QStringLiteral("medium");
            }
        }
        // yuv420p 是"什么播放器都能放"的像素格式；HDR / 10bit 源会因此降到 8bit ——
        // 这一页的定位是交付兼容性，不是保留母版，所以这里固定下来。
        args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    }

    // ---- 音频 ----
    if (audio == ConversionAudioCodec::Copy) {
        args << QStringLiteral("-c:a") << QStringLiteral("copy");
    } else if (audio == ConversionAudioCodec::None) {
        args << QStringLiteral("-an");
    } else {
        args << QStringLiteral("-c:a") << AudioEncoderName(audio);
        const int kbps = AudioBitrateKbps(audio, profile.audio_bitrate_kbps);
        if (kbps > 0) {
            args << QStringLiteral("-b:a") << QString::number(kbps) + QStringLiteral("k");
        }
    }

    // ---- 缩放 / 帧率 ----
    // 只有重编码视频时才会有 -vf：`-vf` 配 `-c:v copy` 是 ffmpeg 明确禁止的组合。
    if (video != ConversionVideoCodec::None && video != ConversionVideoCodec::Copy) {
        QStringList filters;
        if (ScaleHeight(profile.scale) > 0) {
            // 只约束高度、宽度按 -2 自动取偶数：横屏素材不受宽度限制，竖屏也不会被压扁。
            filters << QStringLiteral("scale=-2:%1").arg(ScaleHeight(profile.scale));
        }
        if (FrameRateValue(profile.fps) > 0) {
            filters << QStringLiteral("fps=%1").arg(FrameRateValue(profile.fps));
        }
        if (!filters.isEmpty()) {
            args << QStringLiteral("-vf") << filters.join(QLatin1Char(','));
        }
    }

    if (spec.faststart) {
        // 把 moov 前置，边下边播。只对 mp4 家族有效；加到 .ts / .flv 上会被拒绝。
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    }
    args << output;

    plan.ok = true;
    return plan;
}

} // namespace ffmpeg
} // namespace videoeye
