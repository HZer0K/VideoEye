#include "ui/dialogs/FfmpegSupportProbe.h"

// 本文件是 ui/ 里唯一直接问 libav* 的地方（帮助页「支持的格式」用）。
//
// 必须包在 extern "C" 里: 随包分发的这份 FFmpeg 头文件里没有 extern "C" 声明，
// 不包的话 MSVC 按 C++ 名字修饰去找 avutil_version 之类的符号，链接期直接
// LNK2019（项目里所有 include libav* 的 TU 都一样，见 core/analysis 下任一文件）。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <map>
#include <string>

namespace videoeye {
namespace ui {
namespace {

// 归并用的下标: 同名（解码器/编码器、解复用器/复用器）合成一行。
// 用 std::string 而不用 QString 当 key，是为了让"归并"这件事跟 Qt 容器无关。
using IndexByName = std::map<std::string, std::size_t>;

QString SafeUtf8(const char* s) {
    return s ? QString::fromUtf8(s) : QString();
}

QString VersionText(int version) {
    return QString::number(AV_VERSION_MAJOR(version)) + "." +
           QString::number(AV_VERSION_MINOR(version)) + "." +
           QString::number(AV_VERSION_MICRO(version));
}

SupportMediaType FromAvMediaType(AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return SupportMediaType::Video;
        case AVMEDIA_TYPE_AUDIO: return SupportMediaType::Audio;
        case AVMEDIA_TYPE_SUBTITLE: return SupportMediaType::Subtitle;
        case AVMEDIA_TYPE_DATA: return SupportMediaType::Data;
        case AVMEDIA_TYPE_ATTACHMENT: return SupportMediaType::Attachment;
        default: return SupportMediaType::Unknown;
    }
}

// FFmpeg 8.0 把 av_hwdevice_iterate() 改名成 av_hwdevice_iterate_types()。
// 老名字在 8.x 头文件里已经不存在了，所以这里按 avutil 的大版本分流。
AVHWDeviceType NextHwDeviceType(AVHWDeviceType current) {
#if LIBAVUTIL_VERSION_MAJOR >= 60
    return av_hwdevice_iterate_types(current);
#else
    return av_hwdevice_iterate(current);
#endif
}

// 硬件加速类型的中文说明。只给认识的那几个，其余回落到 FFmpeg 原名 ——
// 宁可少说，也不要给一个看起来权威其实是猜的名字。
QString HwAccelDescription(const QString& name) {
    static const std::map<QString, QString> kKnown = {
        {QStringLiteral("cuda"), QStringLiteral("NVIDIA CUDA (NVDEC/NVENC)")},
        {QStringLiteral("cuvid"), QStringLiteral("NVIDIA CUVID (旧接口)")},
        {QStringLiteral("d3d11va"), QStringLiteral("Direct3D 11 视频加速 (Windows)")},
        {QStringLiteral("dxva2"), QStringLiteral("DirectX VA 2.0 (Windows)")},
        {QStringLiteral("qsv"), QStringLiteral("Intel Quick Sync Video")},
        {QStringLiteral("vaapi"), QStringLiteral("VAAPI (Linux)")},
        {QStringLiteral("vdpau"), QStringLiteral("NVIDIA VDPAU (Linux)")},
        {QStringLiteral("videotoolbox"), QStringLiteral("VideoToolbox (macOS/iOS)")},
        {QStringLiteral("opencl"), QStringLiteral("OpenCL")},
        {QStringLiteral("vulkan"), QStringLiteral("Vulkan")},
        {QStringLiteral("drm"), QStringLiteral("DRM 直通 (Linux)")},
        {QStringLiteral("mediacodec"), QStringLiteral("Android MediaCodec")},
    };
    const auto it = kKnown.find(name);
    return it == kKnown.end() ? name : it->second;
}

void CollectBuildInfo(FfmpegSupportSnapshot& out) {
    out.build.version = SafeUtf8(av_version_info());
    // 配置行为空只可能是极小体积的裁剪构建（--disable-...），此时退回 avformat 的
    // configure 行，两个都空就留空字符串 —— 页面上会显示「未提供」。
    out.build.configuration = SafeUtf8(avutil_configuration());
    if (out.build.configuration.isEmpty()) {
        out.build.configuration = SafeUtf8(avformat_configuration());
    }
    out.build.license = SafeUtf8(avutil_license());
    if (out.build.license.isEmpty()) out.build.license = SafeUtf8(avformat_license());

    out.build.libraries = {
        {QStringLiteral("libavutil"), VersionText(avutil_version())},
        {QStringLiteral("libavcodec"), VersionText(avcodec_version())},
        {QStringLiteral("libavformat"), VersionText(avformat_version())},
        {QStringLiteral("libswscale"), VersionText(swscale_version())},
        {QStringLiteral("libswresample"), VersionText(swresample_version())},
    };
}

void CollectCodecs(FfmpegSupportSnapshot& out) {
    IndexByName index;
    void* iter = nullptr;
    const AVCodec* codec = nullptr;
    while ((codec = av_codec_iterate(&iter)) != nullptr) {
        if (!codec->name) continue;
        const std::string key(codec->name);
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, out.codecs.size()).first;
            CodecSupportEntry entry;
            entry.name = QString::fromUtf8(codec->name);
            entry.long_name = SafeUtf8(codec->long_name);
            entry.type = FromAvMediaType(codec->type);
#if LIBAVCODEC_VERSION_MAJOR >= 61
            // 7.0 起才有 wrapper_name: NULL = FFmpeg 原生实现，否则是外部库
            entry.wrapper = SafeUtf8(codec->wrapper_name);
#endif
            if (const AVCodecDescriptor* desc = avcodec_descriptor_get(codec->id)) {
                entry.intra_only = (desc->props & AV_CODEC_PROP_INTRA_ONLY) != 0;
                entry.lossless = (desc->props & AV_CODEC_PROP_LOSSLESS) != 0;
            }
            out.codecs.push_back(entry);
        }
        CodecSupportEntry& entry = out.codecs[it->second];
        if (av_codec_is_decoder(codec)) entry.can_decode = true;
        if (av_codec_is_encoder(codec)) entry.can_encode = true;
        if (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) entry.experimental = true;
    }
}

void CollectContainers(FfmpegSupportSnapshot& out) {
    IndexByName index;
    auto find_or_add = [&](const char* raw_name) -> std::size_t {
        const std::string key(raw_name ? raw_name : "");
        auto it = index.find(key);
        if (it != index.end()) return it->second;
        const std::size_t pos = out.containers.size();
        index.emplace(key, pos);
        ContainerSupportEntry entry;
        entry.name = SafeUtf8(raw_name);
        out.containers.push_back(entry);
        return pos;
    };

    void* iter = nullptr;
    const AVInputFormat* demuxer = nullptr;
    while ((demuxer = av_demuxer_iterate(&iter)) != nullptr) {
        const std::size_t pos = find_or_add(demuxer->name);
        ContainerSupportEntry& entry = out.containers[pos];
        entry.can_demux = true;
        if (entry.long_name.isEmpty()) entry.long_name = SafeUtf8(demuxer->long_name);
        if (entry.extensions.isEmpty()) entry.extensions = SafeUtf8(demuxer->extensions);
        if (entry.mime_type.isEmpty()) entry.mime_type = SafeUtf8(demuxer->mime_type);
    }

    iter = nullptr;
    const AVOutputFormat* muxer = nullptr;
    while ((muxer = av_muxer_iterate(&iter)) != nullptr) {
        const std::size_t pos = find_or_add(muxer->name);
        ContainerSupportEntry& entry = out.containers[pos];
        entry.can_mux = true;
        if (entry.long_name.isEmpty()) entry.long_name = SafeUtf8(muxer->long_name);
        if (entry.extensions.isEmpty()) entry.extensions = SafeUtf8(muxer->extensions);
        if (entry.mime_type.isEmpty()) entry.mime_type = SafeUtf8(muxer->mime_type);
    }
}

void CollectProtocols(FfmpegSupportSnapshot& out) {
    IndexByName index;
    auto find_or_add = [&](const char* raw_name) -> std::size_t {
        const std::string key(raw_name ? raw_name : "");
        auto it = index.find(key);
        if (it != index.end()) return it->second;
        const std::size_t pos = out.protocols.size();
        index.emplace(key, pos);
        ProtocolSupportEntry entry;
        entry.name = SafeUtf8(raw_name);
        out.protocols.push_back(entry);
        return pos;
    };

    void* iter = nullptr;
    const char* name = nullptr;
    while ((name = avio_enum_protocols(&iter, 0)) != nullptr) {
        out.protocols[find_or_add(name)].can_read = true;
    }
    iter = nullptr;
    while ((name = avio_enum_protocols(&iter, 1)) != nullptr) {
        out.protocols[find_or_add(name)].can_write = true;
    }
}

void CollectHwAccels(FfmpegSupportSnapshot& out) {
    // 先按类型建行。注意: 这里是**编译进来的**类型，能不能用还取决于驱动与设备，
    // 页面上必须写明这一点 —— 否则用户看到 "cuda" 就以为本机一定能硬解。
    std::map<AVHWDeviceType, std::size_t> by_type;
    for (AVHWDeviceType type = NextHwDeviceType(AV_HWDEVICE_TYPE_NONE);
         type != AV_HWDEVICE_TYPE_NONE;
         type = NextHwDeviceType(type)) {
        by_type[type] = out.hwaccels.size();
        HwAccelEntry entry;
        entry.name = SafeUtf8(av_hwdevice_get_type_name(type));
        entry.description = HwAccelDescription(entry.name);
        out.hwaccels.push_back(entry);
    }
    if (out.hwaccels.empty()) return;

    // 再数每个类型有多少解码器声明支持: 只数有实际设备类型的那些（不数
    // AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX 以外的实现细节）。
    void* iter = nullptr;
    const AVCodec* codec = nullptr;
    while ((codec = av_codec_iterate(&iter)) != nullptr) {
        if (!av_codec_is_decoder(codec)) continue;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (!config) break;
            const auto it = by_type.find(config->device_type);
            if (it != by_type.end()) ++out.hwaccels[it->second].decoder_count;
        }
    }
}

template <typename T>
void SortByName(std::vector<T>& rows) {
    std::sort(rows.begin(), rows.end(), [](const T& a, const T& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
}

} // namespace

QString SupportMediaTypeName(SupportMediaType type) {
    switch (type) {
        case SupportMediaType::Video: return QStringLiteral("视频");
        case SupportMediaType::Audio: return QStringLiteral("音频");
        case SupportMediaType::Subtitle: return QStringLiteral("字幕");
        case SupportMediaType::Data: return QStringLiteral("数据");
        case SupportMediaType::Attachment: return QStringLiteral("附件");
        case SupportMediaType::Unknown: break;
    }
    return QStringLiteral("未知");
}

int FfmpegSupportSnapshot::DemuxerCount() const {
    int n = 0;
    for (const auto& c : containers) if (c.can_demux) ++n;
    return n;
}

int FfmpegSupportSnapshot::MuxerCount() const {
    int n = 0;
    for (const auto& c : containers) if (c.can_mux) ++n;
    return n;
}

int FfmpegSupportSnapshot::DecoderCount() const {
    int n = 0;
    for (const auto& c : codecs) if (c.can_decode) ++n;
    return n;
}

int FfmpegSupportSnapshot::EncoderCount() const {
    int n = 0;
    for (const auto& c : codecs) if (c.can_encode) ++n;
    return n;
}

FfmpegSupportSnapshot CollectFfmpegSupport() {
    FfmpegSupportSnapshot snapshot;
    CollectBuildInfo(snapshot);
    CollectCodecs(snapshot);
    CollectContainers(snapshot);
    CollectProtocols(snapshot);
    CollectHwAccels(snapshot);

    SortByName(snapshot.codecs);
    SortByName(snapshot.containers);
    SortByName(snapshot.protocols);
    SortByName(snapshot.hwaccels);
    return snapshot;
}

} // namespace ui
} // namespace videoeye
