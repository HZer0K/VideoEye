#include "StreamInfoExtractor.h"
#include <QFileInfo>
#include <cmath>
#include <limits>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

namespace videoeye {
namespace player {

namespace {

std::string FormatDurationMs(int64_t ms) {
    if (ms <= 0) return {};
    int64_t total_seconds = ms / 1000;
    int64_t hours = total_seconds / 3600;
    int64_t minutes = (total_seconds % 3600) / 60;
    int64_t seconds = total_seconds % 60;
    std::ostringstream oss;
    if (hours > 0) oss << hours << " h ";
    if (minutes > 0 || hours > 0) oss << minutes << " min ";
    oss << seconds << " s";
    return oss.str();
}

std::string FormatBitrate(int64_t bps) {
    if (bps <= 0) return {};
    std::ostringstream oss;
    oss << (bps / 1000) << " kb/s";
    return oss.str();
}

std::string FormatFrameRate(double fps) {
    if (fps <= 0.0) return {};
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);
    oss << fps << " FPS";
    return oss.str();
}

std::string FormatPixels(int v) {
    if (v <= 0) return {};
    return std::to_string(v) + " pixels";
}

std::string FourCC(uint32_t tag) {
    if (tag == 0) return {};
    char s[5];
    s[0] = static_cast<char>(tag & 0xFF);
    s[1] = static_cast<char>((tag >> 8) & 0xFF);
    s[2] = static_cast<char>((tag >> 16) & 0xFF);
    s[3] = static_cast<char>((tag >> 24) & 0xFF);
    s[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        if (s[i] == '\0') s[i] = ' ';
    }
    return std::string(s);
}

std::string GetMetadata(AVDictionary* dict, const char* key) {
    if (!dict || !key) return {};
    if (AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0)) {
        if (e->value) return std::string(e->value);
    }
    return {};
}

int BitsPerSampleForCodec(AVCodecID codec_id) {
    switch (codec_id) {
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_S8:
        return 8;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_U16LE:
    case AV_CODEC_ID_PCM_U16BE:
        return 16;
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_PCM_U24LE:
    case AV_CODEC_ID_PCM_U24BE:
        return 24;
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_U32LE:
    case AV_CODEC_ID_PCM_U32BE:
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_F32BE:
        return 32;
    case AV_CODEC_ID_PCM_F64LE:
    case AV_CODEC_ID_PCM_F64BE:
        return 64;
    default:
        return 0;
    }
}

std::string ColorRangeName(AVColorRange range) {
    switch (range) {
    case AVCOL_RANGE_MPEG: return "Limited";
    case AVCOL_RANGE_JPEG: return "Full";
    default: return {};
    }
}

std::string ScanTypeName(AVFieldOrder order) {
    switch (order) {
    case AV_FIELD_PROGRESSIVE: return "Progressive";
    case AV_FIELD_TT:
    case AV_FIELD_BB:
    case AV_FIELD_TB:
    case AV_FIELD_BT:
        return "Interlaced";
    default: return {};
    }
}

std::string ChromaSubsamplingFromPixFmt(int pix_fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pix_fmt));
    if (!desc || desc->nb_components <= 0) return {};
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0)
        return "4:4:4";
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0)
        return "4:2:2";
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
        return "4:2:0";
    return {};
}

std::string BitDepthFromPixFmt(int pix_fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pix_fmt));
    if (!desc || desc->nb_components <= 0) return {};
    int depth = desc->comp[0].depth;
    if (depth <= 0) return {};
    return std::to_string(depth) + " bits";
}

const uint8_t* GetCodedSideData(const AVCodecParameters* par, AVPacketSideDataType type, int* size) {
    if (size) *size = 0;
    if (!par || !par->coded_side_data || par->nb_coded_side_data <= 0) return nullptr;
    for (int i = 0; i < par->nb_coded_side_data; ++i) {
        const AVPacketSideData& sd = par->coded_side_data[i];
        if (sd.type == type && sd.data && sd.size > 0) {
            if (size) *size = sd.size;
            return sd.data;
        }
    }
    return nullptr;
}

std::string FriendlyColorPrimaries(AVColorPrimaries prim) {
    switch (prim) {
    case AVCOL_PRI_BT709: return "BT.709";
    case AVCOL_PRI_BT2020: return "BT.2020";
    case AVCOL_PRI_SMPTE432: return "Display P3";
    default: break;
    }
    if (const char* n = av_color_primaries_name(prim)) return n;
    return {};
}

std::string FriendlyTransfer(AVColorTransferCharacteristic trc) {
    switch (trc) {
    case AVCOL_TRC_BT709: return "BT.709";
    case AVCOL_TRC_SMPTE2084: return "PQ";
    case AVCOL_TRC_ARIB_STD_B67: return "HLG";
    default: break;
    }
    if (const char* n = av_color_transfer_name(trc)) return n;
    return {};
}

std::string FriendlyColorSpace(AVColorSpace spc) {
    switch (spc) {
    case AVCOL_SPC_BT709: return "BT.709";
    case AVCOL_SPC_BT2020_NCL: return "BT.2020 (non-constant)";
    case AVCOL_SPC_BT2020_CL: return "BT.2020 (constant)";
    default: break;
    }
    if (const char* n = av_color_space_name(spc)) return n;
    return {};
}

std::string FormatMasteringDisplayPrimaries(const AVMasteringDisplayMetadata& md) {
    auto RationalToDouble = [](AVRational r) -> double {
        if (r.num == 0 || r.den == 0) return 0.0;
        return static_cast<double>(r.num) / static_cast<double>(r.den);
    };
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(4);
    if (!md.has_primaries) return {};
    const double rx = RationalToDouble(md.display_primaries[0][0]);
    const double ry = RationalToDouble(md.display_primaries[0][1]);
    const double gx = RationalToDouble(md.display_primaries[1][0]);
    const double gy = RationalToDouble(md.display_primaries[1][1]);
    const double bx = RationalToDouble(md.display_primaries[2][0]);
    const double by = RationalToDouble(md.display_primaries[2][1]);
    const double wx = RationalToDouble(md.white_point[0]);
    const double wy = RationalToDouble(md.white_point[1]);
    oss << "R(" << rx << "," << ry << ") "
        << "G(" << gx << "," << gy << ") "
        << "B(" << bx << "," << by << ") "
        << "WP(" << wx << "," << wy << ")";
    return oss.str();
}

std::string FormatMasteringDisplayLuminance(const AVMasteringDisplayMetadata& md) {
    if (!md.has_luminance) return {};
    auto LuminanceToDouble = [](AVRational r) -> double {
        if (r.num == 0 || r.den == 0) return 0.0;
        return static_cast<double>(r.num) / static_cast<double>(r.den);
    };
    const double max_l = LuminanceToDouble(md.max_luminance);
    const double min_l = LuminanceToDouble(md.min_luminance);
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << min_l << " - " << max_l << " cd/m2";
    return oss.str();
}

} // namespace

int64_t StreamInfoExtractor::DurationMsFromFormat(const AVFormatContext* format_ctx) {
    if (!format_ctx) return 0;
    int64_t best_ms = 0;
    if (format_ctx->duration != AV_NOPTS_VALUE && format_ctx->duration > 0) {
        best_ms = av_rescale(format_ctx->duration, 1000, AV_TIME_BASE);
    }
    if (format_ctx->streams && format_ctx->nb_streams > 0) {
        const AVRational kMsBase{1, 1000};
        for (unsigned i = 0; i < format_ctx->nb_streams; ++i) {
            const AVStream* s = format_ctx->streams[i];
            if (!s || s->time_base.den == 0) continue;
            if (s->duration == AV_NOPTS_VALUE || s->duration <= 0) continue;
            const int64_t ms = av_rescale_q(s->duration, s->time_base, kMsBase);
            if (ms > best_ms) best_ms = ms;
        }
    }
    return best_ms;
}

int StreamInfoExtractor::ClampMsToInt(int64_t ms) {
    if (ms <= 0) return 0;
    if (ms > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(ms);
}

int64_t StreamInfoExtractor::EstimateRawPcmDurationMs(const QString& url,
                                                       const AVCodecParameters* codecpar) {
    if (!codecpar || codecpar->sample_rate <= 0) return 0;
    int channels = codecpar->ch_layout.nb_channels;
    if (channels <= 0) channels = 1;
    int bits_per_sample = codecpar->bits_per_coded_sample;
    if (bits_per_sample <= 0) bits_per_sample = BitsPerSampleForCodec(codecpar->codec_id);
    if (bits_per_sample <= 0) return 0;
    const QFileInfo fi(url);
    if (!fi.exists() || !fi.isFile()) return 0;
    const int64_t bytes_per_second =
        static_cast<int64_t>(codecpar->sample_rate) * channels * bits_per_sample / 8;
    if (bytes_per_second <= 0) return 0;
    return (fi.size() * 1000LL) / bytes_per_second;
}

StreamInfoExtractor::ExtractResult StreamInfoExtractor::Extract(AVFormatContext* format_ctx,
                                                int video_stream_index,
                                                int audio_stream_index,
                                                const QString& url) const {
    ExtractResult result;
    model::StreamInfo& info = result.info;
    if (!format_ctx) return result;

    info.filename = url.toStdString();
    int64_t duration_ms = DurationMsFromFormat(format_ctx);
    if (duration_ms <= 0 && audio_stream_index >= 0 &&
        format_ctx->streams && format_ctx->streams[audio_stream_index]) {
        duration_ms = EstimateRawPcmDurationMs(url, format_ctx->streams[audio_stream_index]->codecpar);
    }
    result.duration_ms = ClampMsToInt(duration_ms);

    info.extractor.complete_name = info.filename;
    if (format_ctx->iformat) {
        if (format_ctx->iformat->long_name)
            info.extractor.format = format_ctx->iformat->long_name;
        else if (format_ctx->iformat->name)
            info.extractor.format = format_ctx->iformat->name;
    }
    info.extractor.format_profile = GetMetadata(format_ctx->metadata, "major_brand");
    {
        std::string major_brand = GetMetadata(format_ctx->metadata, "major_brand");
        std::string compatible = GetMetadata(format_ctx->metadata, "compatible_brands");
        if (!major_brand.empty()) {
            if (!compatible.empty())
                info.extractor.codec_id = major_brand + " (" + compatible + ")";
            else
                info.extractor.codec_id = major_brand;
        }
    }
    {
        QFileInfo fi(url);
        if (fi.exists() && fi.isFile()) {
            const qint64 size = fi.size();
            if (size > 0) {
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(1);
                oss << (static_cast<double>(size) / (1024.0 * 1024.0)) << " MiB";
                info.extractor.file_size = oss.str();
            }
        }
    }
    info.extractor.duration = FormatDurationMs(duration_ms);
    info.extractor.overall_bit_rate = FormatBitrate(format_ctx->bit_rate);
    info.extractor.writing_application = GetMetadata(format_ctx->metadata, "encoder");
    {
        std::string desc = GetMetadata(format_ctx->metadata, "description");
        if (desc.empty()) desc = GetMetadata(format_ctx->metadata, "comment");
        info.extractor.description = desc;
    }

    // 视频流信息
    if (video_stream_index >= 0 && format_ctx->streams && format_ctx->streams[video_stream_index]) {
        AVStream* vs = format_ctx->streams[video_stream_index];
        AVCodecParameters* vp = vs->codecpar;
        const double fps = av_q2d(vs->avg_frame_rate);
        info.extractor.frame_rate = FormatFrameRate(fps);

        info.video.id = std::to_string(video_stream_index + 1);
        const AVCodecDescriptor* vdesc = avcodec_descriptor_get(vp->codec_id);
        if (vdesc) {
            info.video.format = vdesc->name ? std::string(vdesc->name) : "";
            info.video.format_info = vdesc->long_name ? std::string(vdesc->long_name) : "";
            info.video.codec_id_info = info.video.format_info;
        } else {
            const char* name = avcodec_get_name(vp->codec_id);
            info.video.format = name ? std::string(name) : "";
        }
        {
            const char* profile = avcodec_profile_name(vp->codec_id, vp->profile);
            if (profile) info.video.format_profile = profile;
        }
        {
            std::string tag = FourCC(vp->codec_tag);
            if (!tag.empty()) info.video.codec_id = tag;
            else if (vdesc && vdesc->name) info.video.codec_id = vdesc->name;
        }
        info.video.duration = info.extractor.duration;
        info.video.bit_rate = FormatBitrate(vp->bit_rate > 0 ? vp->bit_rate : vs->codecpar->bit_rate);
        info.video.width = FormatPixels(vp->width);
        info.video.height = FormatPixels(vp->height);
        {
            AVRational sar = vs->sample_aspect_ratio.num ? vs->sample_aspect_ratio : vp->sample_aspect_ratio;
            if (sar.num > 0 && sar.den > 0 && vp->width > 0 && vp->height > 0) {
                const double dar = (static_cast<double>(vp->width) * sar.num) /
                                   (static_cast<double>(vp->height) * sar.den);
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(3);
                oss << dar;
                info.video.display_aspect_ratio = oss.str();
            }
        }
        info.video.frame_rate = FormatFrameRate(fps);
        if (vp->color_space != AVCOL_SPC_UNSPECIFIED) {
            info.video.color_space = FriendlyColorSpace(vp->color_space);
            info.video.matrix_coefficients = FriendlyColorSpace(vp->color_space);
        }
        info.video.chroma_subsampling = ChromaSubsamplingFromPixFmt(vp->format);
        info.video.bit_depth = BitDepthFromPixFmt(vp->format);
        info.video.scan_type = ScanTypeName(vp->field_order);
        info.video.color_range = ColorRangeName(vp->color_range);
        if (vp->color_primaries != AVCOL_PRI_UNSPECIFIED)
            info.video.color_primaries = FriendlyColorPrimaries(vp->color_primaries);
        if (vp->color_trc != AVCOL_TRC_UNSPECIFIED)
            info.video.transfer_characteristics = FriendlyTransfer(vp->color_trc);

        // HDR 信息
        {
            bool is_pq = (vp->color_trc == AVCOL_TRC_SMPTE2084);
            bool is_hlg = (vp->color_trc == AVCOL_TRC_ARIB_STD_B67);
            if (is_pq) info.video.hdr_format = "PQ";
            else if (is_hlg) {
                info.video.hdr_format = "HLG";
                info.video.hdr_format_compatibility = "HLG";
            }
        }
        {
            int side_size = 0;
            const uint8_t* side = GetCodedSideData(vp, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, &side_size);
            if (side && side_size >= static_cast<int>(sizeof(AVMasteringDisplayMetadata))) {
                const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(side);
                std::string prim = FormatMasteringDisplayPrimaries(*md);
                std::string lum = FormatMasteringDisplayLuminance(*md);
                if (!prim.empty()) info.video.mastering_display_color_primaries = prim;
                if (!lum.empty()) info.video.mastering_display_luminance = lum;
                if ((!prim.empty() || !lum.empty()) && vp->color_trc == AVCOL_TRC_SMPTE2084) {
                    info.video.hdr_format = "HDR10";
                    info.video.hdr_format_compatibility = "HDR10";
                }
            }
        }
        {
            int side_size = 0;
            const uint8_t* side = GetCodedSideData(vp, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, &side_size);
            if (side && side_size >= static_cast<int>(sizeof(AVContentLightMetadata))) {
                const auto* cl = reinterpret_cast<const AVContentLightMetadata*>(side);
                if (cl->MaxCLL > 0)
                    info.video.maximum_content_light_level = std::to_string(cl->MaxCLL) + " cd/m2";
                if (cl->MaxFALL > 0)
                    info.video.maximum_frame_average_light_level = std::to_string(cl->MaxFALL) + " cd/m2";
                if ((cl->MaxCLL > 0 || cl->MaxFALL > 0) && vp->color_trc == AVCOL_TRC_SMPTE2084) {
                    info.video.hdr_format = "HDR10";
                    info.video.hdr_format_compatibility = "HDR10";
                }
            }
        }
    }

    // 音频流信息
    if (audio_stream_index >= 0 && format_ctx->streams && format_ctx->streams[audio_stream_index]) {
        AVStream* as = format_ctx->streams[audio_stream_index];
        AVCodecParameters* ap = as->codecpar;

        info.audio.id = std::to_string(audio_stream_index + 1);
        const AVCodecDescriptor* adesc = avcodec_descriptor_get(ap->codec_id);
        if (adesc) {
            info.audio.format = adesc->name ? std::string(adesc->name) : "";
            info.audio.format_info = adesc->long_name ? std::string(adesc->long_name) : "";
        } else {
            const char* name = avcodec_get_name(ap->codec_id);
            info.audio.format = name ? std::string(name) : "";
        }
        info.audio.codec_id = FourCC(ap->codec_tag);
        info.audio.duration = info.extractor.duration;
        int64_t audio_bit_rate = ap->bit_rate;
        if (audio_bit_rate <= 0 && ap->sample_rate > 0 && ap->ch_layout.nb_channels > 0) {
            const int bits_per_sample = ap->bits_per_coded_sample > 0
                ? ap->bits_per_coded_sample
                : BitsPerSampleForCodec(ap->codec_id);
            if (bits_per_sample > 0)
                audio_bit_rate = static_cast<int64_t>(ap->sample_rate) *
                                 ap->ch_layout.nb_channels * bits_per_sample;
        }
        info.audio.bit_rate = FormatBitrate(audio_bit_rate);
        if (ap->sample_rate > 0)
            info.audio.sampling_rate = std::to_string(ap->sample_rate / 1000.0) + " kHz";
        if (ap->ch_layout.nb_channels > 0) {
            info.audio.channels = std::to_string(ap->ch_layout.nb_channels) + " channels";
            char buf[128];
            if (av_channel_layout_describe(&ap->ch_layout, buf, sizeof(buf)) > 0)
                info.audio.channel_layout = buf;
        }
        info.audio.is_default = (as->disposition & AV_DISPOSITION_DEFAULT) ? "Yes" : "No";
    }

    return result;
}

} // namespace player
} // namespace videoeye
