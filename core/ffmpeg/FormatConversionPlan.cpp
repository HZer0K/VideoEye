#include "core/ffmpeg/FormatConversionPlan.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

#include "core/ffmpeg/FfmpegMediaProbe.h"

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

/// 被接受为某个容器的输出扩展名（第一个是能力表里的规范名）。
/// 只放**确实等价**的别名：`.m2ts` 是 TS、`.adts` 是 AAC 裸流、`.qt` 是 MOV；
/// 不把 `.mkv` 当成 MP4 —— 扩展名决定 muxer，写错就是另一种文件。
QStringList AcceptedExtensions(ConversionContainer container) {
    const QString canonical = ContainerExtension(container);
    switch (container) {
    case ConversionContainer::Ts:
        return {canonical, QStringLiteral("m2ts"), QStringLiteral("mts")};
    case ConversionContainer::Aac:
        return {canonical, QStringLiteral("adts")};
    case ConversionContainer::Mov:
        return {canonical, QStringLiteral("qt")};
    default:
        return {canonical};
    }
}

/// 编码预设 → libvpx / libaom 的 -cpu-used（0 最慢最好，8 最快最糙）。
/// 与 x264/x265 的 -preset 是同一套"速度 / 压缩率取舍"语义，只是名字不同。
int CpuUsedFor(ConversionPreset preset) {
    switch (preset) {
    case ConversionPreset::UltraFast:
        return 8;
    case ConversionPreset::SuperFast:
        return 7;
    case ConversionPreset::VeryFast:
        return 6;
    case ConversionPreset::Faster:
        return 5;
    case ConversionPreset::Fast:
        return 4;
    case ConversionPreset::Medium:
        return 3;
    case ConversionPreset::Slow:
        return 2;
    case ConversionPreset::Slower:
        return 1;
    case ConversionPreset::VerySlow:
        return 0;
    }
    return 3;
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

/// 附加参数的分词结果。`ok=false` 时 error 是可以直接显示给人看的原因。
struct ParsedExtraArguments {
    bool ok = true;
    QString error;
    QStringList tokens;
    /// 是否出现了页面已经生成的同名选项（ffmpeg 以最后一个为准，页面设置会被覆盖）。
    QStringList overridden_options;
};

/// 按 shell 习惯分词：空白分隔，单/双引号可包住带空格的整段。**不把反斜杠当转义**
/// —— 用户可能写 Windows 路径，把 `\` 吃掉会得到一条完全不同的命令。
bool TokenizeExtraArguments(const QString& text, QStringList* tokens, QString* error) {
    QString current;
    bool in_token = false;
    QChar quote;
    for (const QChar ch : text) {
        if (!quote.isNull()) {
            if (ch == quote) {
                quote = QChar();
            } else {
                current += ch;
            }
            continue;
        }
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            quote = ch;
            in_token = true;
            continue;
        }
        if (ch.isSpace()) {
            if (in_token) {
                tokens->append(current);
                current.clear();
                in_token = false;
            }
            continue;
        }
        current += ch;
        in_token = true;
    }
    if (!quote.isNull()) {
        *error = QStringLiteral("附加参数里的引号没有闭合");
        return false;
    }
    if (in_token) {
        tokens->append(current);
    }
    return true;
}

/// 页面自己也会生成的选项。附加参数里重复出现时给一条警告（不是错误）：
/// 高级用户可能就是想覆盖页面设置，但得知道"最后一个才生效"。
const QStringList& ManagedOptions() {
    static const QStringList options = {
        QStringLiteral("-c:v"),
        QStringLiteral("-c:a"),
        QStringLiteral("-crf"),
        QStringLiteral("-b:v"),
        QStringLiteral("-b:a"),
        QStringLiteral("-vf"),
        QStringLiteral("-pix_fmt"),
        QStringLiteral("-preset"),
        QStringLiteral("-movflags"),
        QStringLiteral("-r"),
        QStringLiteral("-s"),
        QStringLiteral("-q:v"),
        QStringLiteral("-filter_complex"),
        QStringLiteral("-loop"),
    };
    return options;
}

ParsedExtraArguments ParseExtraArguments(const QString& text, const QString& input, const QString& output) {
    ParsedExtraArguments result;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return result;
    }
    if (!TokenizeExtraArguments(trimmed, &result.tokens, &result.error)) {
        result.ok = false;
        return result;
    }
    if (result.tokens.isEmpty()) {
        return result;
    }
    // 第一个 token 必须是选项：否则会变成一个裸输出路径，把转换写成第二个文件。
    if (!result.tokens.front().startsWith(QLatin1Char('-'))) {
        result.ok = false;
        result.error = QStringLiteral("附加参数必须从以 - 开头的选项开始，例如 -metadata title=片名");
        return result;
    }
    for (const QString& token : result.tokens) {
        const QString lower = token.toLower();
        if (lower == QLatin1String("-i")) {
            result.ok = false;
            result.error = QStringLiteral("附加参数里不能再次出现 -i：一次转换只处理一个输入文件");
            return result;
        }
        if (lower == QLatin1String("-y") || lower == QLatin1String("-n")) {
            result.ok = false;
            result.error = QStringLiteral("附加参数里不要写 -y / -n：覆盖策略由页面统一控制");
            return result;
        }
        if (lower == QLatin1String("-nostdin")) {
            result.ok = false;
            result.error = QStringLiteral("附加参数里不能写 -nostdin：停止功能依赖 stdin 让 ffmpeg 收尾");
            return result;
        }
        if (LooksLikeLocalPath(token) && (IsSameLocalFile(token, input) || IsSameLocalFile(token, output))) {
            result.ok = false;
            result.error = QStringLiteral("附加参数里不能包含输入或输出文件路径");
            return result;
        }
        if (ManagedOptions().contains(lower) && !result.overridden_options.contains(lower)) {
            result.overridden_options << lower;
        }
    }
    return result;
}

} // namespace

// ===================== 质量数值范围 =====================

int MinQualityValue(ConversionVideoCodec codec) {
    // MPEG-4 的 -q:v 没有 0/1（2 已经是最佳画质），其它编码的 CRF 从 0 开始。
    return codec == ConversionVideoCodec::Mpeg4 ? 2 : 0;
}

int MaxQualityValue(ConversionVideoCodec codec) {
    return codec == ConversionVideoCodec::Mpeg4 ? 31 : CrfCeiling(codec);
}

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

std::vector<ConversionGifLoop> AllConversionGifLoops() {
    return {ConversionGifLoop::Infinite, ConversionGifLoop::Once};
}

QString GifLoopLabel(ConversionGifLoop loop) {
    return loop == ConversionGifLoop::Once ? QStringLiteral("播放一次") : QStringLiteral("无限循环");
}

int GifLoopValue(ConversionGifLoop loop) {
    return loop == ConversionGifLoop::Once ? -1 : 0;
}

std::vector<ConversionPreset> AllConversionPresets() {
    using P = ConversionPreset;
    return {P::UltraFast, P::SuperFast, P::VeryFast, P::Faster, P::Fast, P::Medium, P::Slow, P::Slower, P::VerySlow};
}

QString PresetLabel(ConversionPreset preset) {
    switch (preset) {
    case ConversionPreset::UltraFast:
        return QStringLiteral("ultrafast（最快）");
    case ConversionPreset::SuperFast:
        return QStringLiteral("superfast");
    case ConversionPreset::VeryFast:
        return QStringLiteral("veryfast（快）");
    case ConversionPreset::Faster:
        return QStringLiteral("faster");
    case ConversionPreset::Fast:
        return QStringLiteral("fast");
    case ConversionPreset::Medium:
        return QStringLiteral("medium（默认）");
    case ConversionPreset::Slow:
        return QStringLiteral("slow（较慢 / 更小）");
    case ConversionPreset::Slower:
        return QStringLiteral("slower");
    case ConversionPreset::VerySlow:
        return QStringLiteral("veryslow（最慢 / 最小）");
    }
    return QStringLiteral("medium");
}

QString PresetName(ConversionPreset preset) {
    switch (preset) {
    case ConversionPreset::UltraFast:
        return QStringLiteral("ultrafast");
    case ConversionPreset::SuperFast:
        return QStringLiteral("superfast");
    case ConversionPreset::VeryFast:
        return QStringLiteral("veryfast");
    case ConversionPreset::Faster:
        return QStringLiteral("faster");
    case ConversionPreset::Fast:
        return QStringLiteral("fast");
    case ConversionPreset::Medium:
        return QStringLiteral("medium");
    case ConversionPreset::Slow:
        return QStringLiteral("slow");
    case ConversionPreset::Slower:
        return QStringLiteral("slower");
    case ConversionPreset::VerySlow:
        return QStringLiteral("veryslow");
    }
    return QStringLiteral("medium");
}

bool CodecUsesPreset(ConversionVideoCodec codec) {
    // "用不用速度预设"与"写不写 -preset"是两件事：含 VP9 / AV1，但它们收到的是
    // -cpu-used（见 BuildConversionPlan）。
    return codec == ConversionVideoCodec::H264 || codec == ConversionVideoCodec::H265 ||
           codec == ConversionVideoCodec::Vp9 || codec == ConversionVideoCodec::Av1;
}

std::vector<ConversionPixelFormat> AllConversionPixelFormats() {
    return {ConversionPixelFormat::Yuv420p, ConversionPixelFormat::Yuv420p10};
}

QString PixelFormatLabel(ConversionPixelFormat format) {
    switch (format) {
    case ConversionPixelFormat::Yuv420p:
        return QStringLiteral("8-bit（兼容性最好）");
    case ConversionPixelFormat::Yuv420p10:
        return QStringLiteral("10-bit（减少色带）");
    }
    return QStringLiteral("8-bit（兼容性最好）");
}

QString PixelFormatName(ConversionPixelFormat format) {
    switch (format) {
    case ConversionPixelFormat::Yuv420p:
        return QStringLiteral("yuv420p");
    case ConversionPixelFormat::Yuv420p10:
        return QStringLiteral("yuv420p10le");
    }
    return QStringLiteral("yuv420p");
}

bool CodecSupportsTenBit(ConversionVideoCodec codec) {
    switch (codec) {
    case ConversionVideoCodec::H264:
    case ConversionVideoCodec::H265:
    case ConversionVideoCodec::Vp9:
    case ConversionVideoCodec::Av1:
        return true;
    default:
        return false;
    }
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

    // ffmpeg 按**扩展名**选 muxer，不是按这里的容器枚举：`.mkv` 扩展名配 MP4 设置
    // 会写出 Matroska，没扩展名则直接 "Unable to find a suitable output format"。
    // 这种"界面说的和盘上不是一回事"必须在拼命令前拦掉。
    if (LooksLikeLocalPath(output)) {
        const QString suffix = QFileInfo(output).suffix().toLower();
        const QStringList accepted = AcceptedExtensions(profile.container);
        if (suffix.isEmpty()) {
            plan.error = QStringLiteral("输出文件名缺少扩展名：请以 .%1 结尾（ffmpeg 靠扩展名判断封装格式）")
                             .arg(plan.extension);
            return plan;
        }
        if (!accepted.contains(suffix)) {
            plan.error = QStringLiteral("输出扩展名 .%1 与所选容器「%2」不一致：ffmpeg 会按扩展名写成 .%1 格式"
                                        "（不是 .%3）。请改输出名，或改选容器")
                             .arg(suffix, ContainerLabel(profile.container), plan.extension);
            return plan;
        }
    }

    // 附加参数先分词并校验：错的话在拼任何命令之前返回，绝不留下半条 argv。
    const ParsedExtraArguments extra = ParseExtraArguments(profile.extra_arguments, input, output);
    if (!extra.ok) {
        plan.error = extra.error;
        return plan;
    }
    if (!extra.overridden_options.isEmpty()) {
        plan.warnings << QStringLiteral(
                             "附加参数与页面生成的选项重名（%1）：ffmpeg 以最后一个为准，页面上的对应设置可能被覆盖")
                             .arg(extra.overridden_options.join(QStringLiteral("、")));
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
        plan.warnings << QStringLiteral(
                             "GIF 用固定调色板转码：视频/音频编码与质量档位不生效（按 %1 fps、高 %2 采样，%3）")
                             .arg(QString::number(fps), QString::number(height), GifLoopLabel(profile.gif_loop));
        plan.arguments << QStringLiteral("-hide_banner") << QStringLiteral("-y") << QStringLiteral("-i") << input
                       << QStringLiteral("-filter_complex") << filter << QStringLiteral("-an")
                       << QStringLiteral("-loop") << QString::number(GifLoopValue(profile.gif_loop));
        plan.arguments << extra.tokens;
        plan.arguments << output;
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

    // ---- 重编码视频的额外参数 ----
    const bool reencode_video = video != ConversionVideoCodec::Copy && video != ConversionVideoCodec::None;
    if (reencode_video && profile.pixel_format == ConversionPixelFormat::Yuv420p10 && !CodecSupportsTenBit(video)) {
        plan.error = QStringLiteral("「%1」不支持 10-bit 输出，请把像素格式改回 8-bit").arg(VideoCodecLabel(video));
        return plan;
    }
    if (reencode_video && profile.video_rate_mode == ConversionVideoRateMode::Bitrate &&
        profile.video_bitrate_kbps <= 0) {
        plan.error = QStringLiteral("按码率模式需要填写大于 0 的视频码率（kbps）");
        return plan;
    }

    // ---- 流复制的兼容性 ----
    // 源信息已知时才敢断言"必然失败"；未知时只提示，不然会把本来能跑的组合拦下来。
    if (video == ConversionVideoCodec::Copy) {
        plan.warnings << QStringLiteral("流复制不重新编码：质量档位、码率模式、编码预设、像素格式、缩放与帧率都不生效");
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

    // 字幕默认不映射：跨容器字幕编码（ASS 进 MP4、PGS 进 WebM）经常让整条命令失败，
    // 所以默认只带音视频。但"丢了"不能是静默的 —— 源里有时明确说一声。
    // 用户已经在附加参数里自己写了 `-map 0:s…` 时就不再重复提醒。
    const bool maps_subtitles = std::any_of(extra.tokens.cbegin(), extra.tokens.cend(),
                                            [](const QString& token) { return token.contains(QLatin1String("0:s")); });
    // 先把轨数取成局部变量再 arg：QtDomainBorder 审计按"参数里出现 source.xxx"
    // 判命中（source 恰好与某些 domain 字段同名），直接 .arg(source.xxx) 会被误报。
    const int dropped_subtitles = source.subtitle_track_count;
    const int incoming_audio_tracks = source.audio_track_count;
    if (source.known && dropped_subtitles > 0 && !maps_subtitles) {
        plan.warnings << QStringLiteral("源含 %1 条字幕轨，字幕不会写入输出；需要时可在附加参数里加 -map 0:s?")
                             .arg(dropped_subtitles);
    }
    // 纯音频容器的 muxer 通常只接受一条音轨：显式只映射第一条，并说清楚为什么。
    if (!spec.has_video && source.known && incoming_audio_tracks > 1) {
        plan.warnings
            << QStringLiteral("源含 %1 条音轨，目标容器是纯音频格式，只保留第一条").arg(incoming_audio_tracks);
    }

    QStringList& args = plan.arguments;
    args << QStringLiteral("-hide_banner") << QStringLiteral("-y") << QStringLiteral("-i") << input;
    // 只映射音视频流：字幕 / 数据 / 附件在跨容器时是最容易把整条命令搞失败的一类
    // （最典型：mkv 的 ASS 字幕往 mp4 里塞，ffmpeg 直接 "Subtitle codec not supported"）。
    // `?` 让缺失的那一路不报错 —— 纯音频源也要能转。
    //
    // 大写 `V`：mp4 / mp3 里的封面图（attached pic）也是 video 流，用 `v` 会把它
    // 当成第二路视频重新编码。`V` 只选真正的视频轨。
    // 纯音频容器只映射第一条音轨：mp3 / wav / flac 的 muxer 收不下多条。
    args << QStringLiteral("-map") << QStringLiteral("0:V?") << QStringLiteral("-map")
         << (spec.has_video ? QStringLiteral("0:a?") : QStringLiteral("0:a:0?"));

    // ---- 视频 ----
    if (video == ConversionVideoCodec::Copy) {
        args << QStringLiteral("-c:v") << QStringLiteral("copy");
    } else if (video == ConversionVideoCodec::None) {
        args << QStringLiteral("-vn");
    } else {
        args << QStringLiteral("-c:v") << VideoEncoderName(video);
        if (profile.video_rate_mode == ConversionVideoRateMode::Bitrate) {
            // 码率优先：体积可预估。上面已确保 kbps > 0；夹一个上界防止手滑。
            args << QStringLiteral("-b:v")
                 << QString::number(qBound(1, profile.video_bitrate_kbps, 200000)) + QStringLiteral("k");
        } else if (video == ConversionVideoCodec::Mpeg4) {
            args << QStringLiteral("-q:v") << QString::number(QScaleFor(profile.quality, profile.crf));
        } else {
            args << QStringLiteral("-crf") << QString::number(CrfFor(video, profile.quality, profile.crf));
            if (video == ConversionVideoCodec::Vp9 || video == ConversionVideoCodec::Av1) {
                // VP9 / AV1 的 -crf 必须配 -b:v 0 才是"恒定质量"；只给 -crf 时
                // 默认的目标码率仍然生效，出来的文件会跟设定的质量对不上。
                args << QStringLiteral("-b:v") << QStringLiteral("0");
            }
        }
        // 速度 / 压缩率取舍：x264/x265 用 -preset；libvpx / libaom 不认 -preset，
        // 换成语义等价的 -cpu-used（0 最慢最好、8 最快）。不加这两个参数时
        // VP9 / AV1 默认几乎是"最慢档 + 单线程"，转一部长片能慢到不可用。
        if (video == ConversionVideoCodec::H264 || video == ConversionVideoCodec::H265) {
            args << QStringLiteral("-preset") << PresetName(profile.preset);
        } else if (video == ConversionVideoCodec::Vp9 || video == ConversionVideoCodec::Av1) {
            args << QStringLiteral("-row-mt") << QStringLiteral("1") << QStringLiteral("-cpu-used")
                 << QString::number(CpuUsedFor(profile.preset));
        }
        // 8-bit 兼容性最好；10-bit 只允许在支持的编码上（上面已校验）。
        args << QStringLiteral("-pix_fmt") << PixelFormatName(profile.pixel_format);
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
    // 附加参数放在所有自动生成的选项之后、输出路径之前：同名选项以用户写的为准，
    // 同时保证输出路径永远是最后一个 token。
    args << extra.tokens;
    args << output;

    plan.ok = true;
    return plan;
}

// ===================== 编码器与产物核对 =====================

QStringList RequiredEncoders(const ConversionProfile& profile) {
    QStringList encoders;
    if (profile.container == ConversionContainer::Gif) {
        // GIF 走调色板分支，唯一的编码器就是 gif 本身（palettegen / paletteuse 是滤镜，
        // 不在 -encoders 里，别拿它去对照，否则会误报"缺失"）。
        encoders << QStringLiteral("gif");
        return encoders;
    }
    const QString video = VideoEncoderName(ResolveVideoCodec(profile.container, profile.video));
    if (!video.isEmpty()) {
        encoders << video;
    }
    const QString audio = AudioEncoderName(ResolveAudioCodec(profile.container, profile.audio));
    if (!audio.isEmpty()) {
        encoders << audio;
    }
    return encoders;
}

namespace {

/// 第一条真视频轨。mp4 / mp3 里的封面图也是 video 流，核对"有没有视频"时必须排除。
const MediaStreamSnapshot* FirstPlayableVideo(const MediaProbeSnapshot& snapshot) {
    for (const MediaStreamSnapshot& stream : snapshot.streams) {
        if (stream.kind == MediaStreamKind::Video && !stream.attached_pic) {
            return &stream;
        }
    }
    return nullptr;
}

/// 编码器（枚举）→ 探测输出里应看到的编码短名。空串 = 不做编码判定。
QString ExpectedVideoCodec(ConversionVideoCodec codec) {
    switch (codec) {
    case ConversionVideoCodec::H264:
        return QStringLiteral("h264");
    case ConversionVideoCodec::H265:
        return QStringLiteral("hevc");
    case ConversionVideoCodec::Vp9:
        return QStringLiteral("vp9");
    case ConversionVideoCodec::Av1:
        return QStringLiteral("av1");
    case ConversionVideoCodec::Mpeg4:
        return QStringLiteral("mpeg4");
    default:
        return QString();
    }
}

QString ExpectedAudioCodec(ConversionAudioCodec codec) {
    switch (codec) {
    case ConversionAudioCodec::Aac:
        return QStringLiteral("aac");
    case ConversionAudioCodec::Mp3:
        return QStringLiteral("mp3");
    case ConversionAudioCodec::Opus:
        return QStringLiteral("opus");
    case ConversionAudioCodec::Flac:
        return QStringLiteral("flac");
    case ConversionAudioCodec::Pcm:
        return QStringLiteral("pcm_s16le");
    default:
        return QString();
    }
}

QString DurationText(double seconds) {
    if (seconds < 0) {
        return QStringLiteral("—");
    }
    const int minutes = static_cast<int>(seconds) / 60;
    const double rest = seconds - minutes * 60;
    return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(rest, 4, 'f', 1, QLatin1Char('0'));
}

} // namespace

ConversionVerification VerifyConversionOutput(const ConversionProfile& profile, const ConversionSourceFacts& source,
                                              const MediaProbeSnapshot& output) {
    ConversionVerification result;
    if (!output.parsed) {
        result.mismatch = true;
        result.notes << QStringLiteral("转换已完成，但读不出输出文件的流信息（文件可能不完整或格式不被支持）");
        return result;
    }
    result.parsed = true;
    auto note = [&result](const QString& text) {
        result.mismatch = true;
        result.notes << text;
    };

    const ConversionVideoCodec video = ResolveVideoCodec(profile.container, profile.video);
    const ConversionAudioCodec audio = ResolveAudioCodec(profile.container, profile.audio);
    const MediaStreamSnapshot* actual_video = FirstPlayableVideo(output);
    const MediaStreamSnapshot* actual_audio = output.FirstStream(MediaStreamKind::Audio);

    if (profile.container == ConversionContainer::Gif) {
        // GIF 分支不看编码枚举：视频是 gif，音频恒为无，尺寸按 480 高采样。
        if (actual_video == nullptr) {
            note(QStringLiteral("输出没有视频帧（GIF 为空）"));
        } else {
            if (!actual_video->codec.isEmpty() &&
                actual_video->codec.compare(QLatin1String("gif"), Qt::CaseInsensitive) != 0) {
                note(QStringLiteral("视频编码：目标 gif，实测 %1").arg(actual_video->codec));
            }
            const int target_height = ScaleHeight(profile.scale) > 0 ? ScaleHeight(profile.scale) : 480;
            if (actual_video->has_size() && actual_video->height != target_height) {
                note(QStringLiteral("分辨率：目标高 %1，实测 %2×%3")
                         .arg(target_height)
                         .arg(actual_video->width)
                         .arg(actual_video->height));
            }
        }
        if (actual_audio != nullptr) {
            note(QStringLiteral("GIF 不含音频，输出却带了音频轨"));
        }
    } else {
        if (video == ConversionVideoCodec::None) {
            if (actual_video != nullptr) {
                note(QStringLiteral("目标是不要视频，但输出里仍有视频轨（%1）").arg(actual_video->codec));
            }
        } else {
            QString expected;
            if (video == ConversionVideoCodec::Copy) {
                if (source.known) {
                    expected = source.video_codec;
                }
            } else {
                expected = ExpectedVideoCodec(video);
            }
            if (actual_video == nullptr) {
                // 流复制时源本来没视频，ffmpeg 忽略 -c:v copy 是预期行为，不算差异。
                if (video != ConversionVideoCodec::Copy || (source.known && source.has_video)) {
                    note(QStringLiteral("输出没有视频轨"));
                }
            } else {
                if (!expected.isEmpty() && !actual_video->codec.isEmpty() &&
                    actual_video->codec.compare(expected, Qt::CaseInsensitive) != 0) {
                    note(QStringLiteral("视频编码：目标 %1，实测 %2").arg(expected, actual_video->codec));
                }
                if (profile.scale != ConversionScale::Keep && actual_video->has_size()) {
                    const int target_height = ScaleHeight(profile.scale);
                    if (target_height > 0 && actual_video->height != target_height) {
                        note(QStringLiteral("分辨率：目标高 %1，实测 %2×%3")
                                 .arg(target_height)
                                 .arg(actual_video->width)
                                 .arg(actual_video->height));
                    }
                }
            }
        }

        if (audio == ConversionAudioCodec::None) {
            if (actual_audio != nullptr) {
                note(QStringLiteral("目标是不要音频，但输出里仍有音频轨"));
            }
        } else {
            QString expected;
            if (audio == ConversionAudioCodec::Copy) {
                if (source.known) {
                    expected = source.audio_codec;
                }
            } else {
                expected = ExpectedAudioCodec(audio);
            }
            if (actual_audio == nullptr) {
                if (audio != ConversionAudioCodec::Copy || (source.known && source.has_audio)) {
                    note(QStringLiteral("输出没有音频轨"));
                }
            } else if (!expected.isEmpty() && !actual_audio->codec.isEmpty() &&
                       actual_audio->codec.compare(expected, Qt::CaseInsensitive) != 0) {
                note(QStringLiteral("音频编码：目标 %1，实测 %2").arg(expected, actual_audio->codec));
            }
        }
    }

    // 时长被截短是"转完了但内容少了"里最隐蔽的一种：只在两边都读得到时判。
    if (source.known && source.duration_seconds > 0 && output.duration_seconds > 0) {
        const double tolerance = qMax(1.5, source.duration_seconds * 0.01);
        if (std::fabs(output.duration_seconds - source.duration_seconds) > tolerance) {
            note(QStringLiteral("时长：源 %1，输出 %2（可能被截断）")
                     .arg(DurationText(source.duration_seconds), DurationText(output.duration_seconds)));
        }
    }

    return result;
}

} // namespace ffmpeg
} // namespace videoeye
