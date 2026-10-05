#include "core/analysis/quality/ColorHdrAnalyzer.h"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/pixdesc.h>
}

namespace videoeye {
namespace {

// ---- core/domain/model/ColorInfo.h 里镜像的 FFmpeg 枚举值交叉校验 ----
// 这些值一旦被上游改动，色彩/HDR 的判定会整体错位（例如把 PQ 判成 BT.709），
// 所以宁可编译失败也不要静默出错。
static_assert(static_cast<int>(AVCOL_TRC_SMPTE2084) == model::ffmpeg_expect::kTrcSmpte2084,
              "AVCOL_TRC_SMPTE2084 与 ColorInfo.h 的镜像值不一致");
static_assert(static_cast<int>(AVCOL_TRC_ARIB_STD_B67) == model::ffmpeg_expect::kTrcAribStdB67,
              "AVCOL_TRC_ARIB_STD_B67 与 ColorInfo.h 的镜像值不一致");
static_assert(static_cast<int>(AVCOL_PRI_BT2020) == model::ffmpeg_expect::kPriBt2020,
              "AVCOL_PRI_BT2020 与 ColorInfo.h 的镜像值不一致");
static_assert(static_cast<int>(AVCOL_RANGE_MPEG) == model::ffmpeg_expect::kRangeMpeg &&
                  static_cast<int>(AVCOL_RANGE_JPEG) == model::ffmpeg_expect::kRangeJpeg,
              "AVCOL_RANGE_* 与 ColorInfo.h 的镜像值不一致");

double RationalToDouble(AVRational r) {
    if (r.den == 0) return 0.0;
    return static_cast<double>(r.num) / static_cast<double>(r.den);
}

const AVPacketSideData* FindSideData(const AVPacketSideData* sd, int nb,
                                     AVPacketSideDataType type) {
    if (!sd || nb <= 0) return nullptr;
    return av_packet_side_data_get(sd, nb, type);
}

}  // namespace

// ============================ ColorHdrAnalyzer ============================

void ColorHdrAnalyzer::Reset(const ColorHdrOptions& options) {
    options_ = options;
    result_ = ColorHdrAnalysis{};
    finished_ = false;
}

void ColorHdrAnalyzer::UpdateFromStream(const AVStream* stream) {
    if (!stream || stream->codecpar == nullptr) return;
    const AVCodecParameters* par = stream->codecpar;
    if (par->codec_type != AVMEDIA_TYPE_VIDEO) return;

    model::ColorInfo& color = result_.color;
    // 只跟踪第一条视频流
    if (color.stream_index >= 0 && color.stream_index != stream->index) return;

    const bool first = (color.stream_index < 0);
    color.stream_index = stream->index;
    result_.stream_index = stream->index;
    result_.hdr.stream_index = stream->index;

    if (first) {
        if (const char* codec = avcodec_get_name(par->codec_id)) color.codec_name = codec;
        if (const char* profile = avcodec_profile_name(par->codec_id, par->profile)) {
            color.profile_name = profile;
        }
        color.level = par->level;
        color.width = par->width;
        color.height = par->height;
    }

    color.primaries_raw = par->color_primaries;
    color.transfer_raw = par->color_trc;
    color.matrix_raw = par->color_space;
    color.range_raw = par->color_range;
    color.primaries = model::ClassifyPrimariesRaw(par->color_primaries);
    color.transfer = model::ClassifyTransferRaw(par->color_trc);
    color.matrix = model::ClassifyMatrixRaw(par->color_space);
    color.range = model::ClassifyRangeRaw(par->color_range);
    color.primaries_name = model::PrimariesDisplayName(par->color_primaries);
    color.transfer_name = model::TransferDisplayName(par->color_trc);
    color.matrix_name = model::MatrixDisplayName(par->color_space);
    color.range_name = model::RangeDisplayName(par->color_range);

    ApplyPixelFormat(par->format, par->bits_per_raw_sample);

    // 容器 + 码流两层的 coded side data 都扫一遍
    // FFmpeg 8.x 已移除 AVStream::side_data，容器级元数据统一落在 AVCodecParameters 上
    const bool found = ScanSideData(par->coded_side_data, par->nb_coded_side_data,
                                    "coded_side_data");
    if (found) AddSource("容器 / 码流 (coded_side_data)");
}

void ColorHdrAnalyzer::UpdateFromPacket(const AVPacket* packet, int stream_index) {
    if (!packet || packet->side_data_elems <= 0) return;
    if (result_.color.stream_index >= 0 && result_.color.stream_index != stream_index) return;

    if (ScanSideData(packet->side_data, packet->side_data_elems, "AVPacket side data")) {
        AddSource("视频包 (AVPacket side data)");
    }
}

void ColorHdrAnalyzer::UpdateFromFrame(const AVFrame* frame) {
    if (!frame) return;
    result_.color.frame_side_data_checked = true;

    bool found = false;
    if (const AVFrameSideData* md = av_frame_get_side_data(
            frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        ApplyMasteringDisplay(md->data, static_cast<int>(md->size), "AVFrame side data");
        found = true;
    }
    if (const AVFrameSideData* cl =
            av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        ApplyContentLight(cl->data, static_cast<int>(cl->size), "AVFrame side data");
        found = true;
    }
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS)) {
        result_.hdr.has_hdr10_plus = true;
        found = true;
    }
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_VIVID)) {
        result_.hdr.has_hdr_vivid = true;
        found = true;
    }
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT)) {
        result_.hdr.has_ambient_viewing_env = true;
        found = true;
    }
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA)) {
        const bool had_dv_config = result_.hdr.dolby_vision.present;
        result_.hdr.dolby_vision.present = true;
        result_.hdr.dolby_vision.rpu_present = true;
        if (!had_dv_config) {
            AddNote("Dolby Vision RPU 元数据由解码帧确认（容器未给出 DV configuration record）");
        }
        found = true;
    }
    if (found) AddSource("解码帧 AVFrame side data");
}

bool ColorHdrAnalyzer::ScanSideData(const AVPacketSideData* side_data, int count,
                                    const std::string& source) {
    bool found = false;
    const AVPacketSideData* md =
        FindSideData(side_data, count, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    if (md != nullptr) {
        ApplyMasteringDisplay(md->data, md->size, source);
        found = true;
    }
    const AVPacketSideData* cl =
        FindSideData(side_data, count, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    if (cl != nullptr) {
        ApplyContentLight(cl->data, cl->size, source);
        found = true;
    }
    const AVPacketSideData* dv = FindSideData(side_data, count, AV_PKT_DATA_DOVI_CONF);
    if (dv != nullptr) {
        ApplyDolbyVision(dv->data, dv->size, source);
        found = true;
    }
    return found;
}

void ColorHdrAnalyzer::ApplyMasteringDisplay(const unsigned char* data, int size,
                                             const std::string& source) {
    if (!data || size < static_cast<int>(sizeof(AVMasteringDisplayMetadata))) return;
    if (result_.hdr.mastering_display.present) return;

    const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(data);
    model::MasteringDisplayMetadata& target = result_.hdr.mastering_display;
    target.present = true;
    target.has_primaries = md->has_primaries != 0;
    target.has_luminance = md->has_luminance != 0;
    if (target.has_primaries) {
        target.red_x = RationalToDouble(md->display_primaries[0][0]);
        target.red_y = RationalToDouble(md->display_primaries[0][1]);
        target.green_x = RationalToDouble(md->display_primaries[1][0]);
        target.green_y = RationalToDouble(md->display_primaries[1][1]);
        target.blue_x = RationalToDouble(md->display_primaries[2][0]);
        target.blue_y = RationalToDouble(md->display_primaries[2][1]);
        target.white_x = RationalToDouble(md->white_point[0]);
        target.white_y = RationalToDouble(md->white_point[1]);
    }
    if (target.has_luminance) {
        target.max_luminance = RationalToDouble(md->max_luminance);
        target.min_luminance = RationalToDouble(md->min_luminance);
    }
    if (!target.Plausible()) {
        AddNote(source + " 中的母版显示信息数值异常（全 0 或超出合理范围）");
    }
}

void ColorHdrAnalyzer::ApplyContentLight(const unsigned char* data, int size,
                                         const std::string& source) {
    if (!data || size < static_cast<int>(sizeof(AVContentLightMetadata))) return;
    if (result_.hdr.content_light.present) return;

    const auto* cl = reinterpret_cast<const AVContentLightMetadata*>(data);
    model::ContentLightLevelMetadata& target = result_.hdr.content_light;
    target.present = true;
    target.max_cll = cl->MaxCLL;
    target.max_fall = cl->MaxFALL;
    if (!target.HasAny()) AddNote(source + " 中的 MaxCLL/MaxFALL 均为 0");
}

void ColorHdrAnalyzer::ApplyDolbyVision(const unsigned char* data, int size,
                                        const std::string& source) {
    if (!data || size < static_cast<int>(sizeof(AVDOVIDecoderConfigurationRecord))) return;
    if (result_.hdr.dolby_vision.present) return;

    const auto* dv = reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(data);
    model::DolbyVisionMetadata& target = result_.hdr.dolby_vision;
    target.present = true;
    target.profile = dv->dv_profile;
    target.level = dv->dv_level;
    target.rpu_present = dv->rpu_present_flag != 0;
    target.el_present = dv->el_present_flag != 0;
    target.bl_present = dv->bl_present_flag != 0;
    target.compatibility_id = dv->dv_bl_signal_compatibility_id;
    (void)source;
}

void ColorHdrAnalyzer::ApplyPixelFormat(int pix_fmt, int bits_per_raw_sample) {
    auto& pf = result_.color.pixel_format;
    if (pix_fmt == AV_PIX_FMT_NONE) return;

    const AVPixelFormat fmt = static_cast<AVPixelFormat>(pix_fmt);
    if (const char* name = av_get_pix_fmt_name(fmt)) pf.name = name;
    pf.bits_per_raw_sample = bits_per_raw_sample;

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (desc == nullptr || desc->nb_components <= 0) return;
    pf.valid = true;
    pf.rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    pf.bit_depth = desc->comp[0].depth;

    if (!pf.rgb) {
        if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) {
            pf.chroma_subsampling = "4:4:4";
        } else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
            pf.chroma_subsampling = "4:2:2";
        } else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
            pf.chroma_subsampling = "4:2:0";
        } else if (desc->log2_chroma_w == 2 && desc->log2_chroma_h == 2) {
            pf.chroma_subsampling = "4:1:0";
        } else if (desc->log2_chroma_w == 2 && desc->log2_chroma_h == 0) {
            pf.chroma_subsampling = "4:1:1";
        }
    }

    // yuvjXXX 是 FFmpeg 早期表示 full range 的写法（现已 deprecated）。
    // 若它同时被标了 Limited range，说明「像素格式与容器 metadata 打架」，需要告警。
    if (pf.ImpliesFullRange() && result_.color.range == model::ColorRangeKind::Limited) {
        AddNote("像素格式为 " + pf.name + "（隐含 full range），但色彩范围标注为 Limited");
    }
}

void ColorHdrAnalyzer::AddSource(const std::string& source) {
    auto& sources = result_.hdr.sources;
    if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
        sources.push_back(source);
    }
}

void ColorHdrAnalyzer::AddNote(const std::string& note) {
    auto& notes = result_.notes;
    if (std::find(notes.begin(), notes.end(), note) == notes.end()) notes.push_back(note);
}

bool ColorHdrAnalyzer::NeedsFrameProbe() const {
    if (!options_.probe_decoded_frame) return false;
    if (result_.color.frame_side_data_checked) return false;
    // 已经拿到动态元数据就没必要再解一帧
    if (result_.hdr.has_hdr10_plus && result_.hdr.has_hdr_vivid) return false;
    // 疑似 HDR（PQ/HLG/DV）且静态元数据不全时，值得解一帧去 SEI 里再找一遍
    if (result_.color.IsHdrTransfer() || result_.hdr.dolby_vision.present) return true;
    return false;
}

const ColorHdrAnalysis& ColorHdrAnalyzer::Finish() {
    if (finished_) return result_;

    model::HdrMetadataInfo& hdr = result_.hdr;
    hdr.format = model::ClassifyHdrFormat(result_.color, hdr);
    if (result_.color.stream_index >= 0) {
        result_.analyzed = true;
        hdr.analyzed = true;
        result_.color.analyzed = true;
    }

    switch (hdr.format) {
        case model::HdrFormat::DolbyVision:
            hdr.format_name = "Dolby Vision " + hdr.dolby_vision.ProfileText();
            hdr.hdr = true;
            break;
        case model::HdrFormat::Hdr10Plus:
            hdr.format_name = "HDR10+";
            hdr.hdr = true;
            break;
        case model::HdrFormat::HdrVivid:
            hdr.format_name = "HDR Vivid";
            hdr.hdr = true;
            break;
        case model::HdrFormat::Hdr10:
            hdr.format_name = "HDR10";
            hdr.hdr = true;
            break;
        case model::HdrFormat::Hdr10Basic:
            hdr.format_name = "PQ（静态元数据不全，非完整 HDR10）";
            hdr.hdr = true;
            break;
        case model::HdrFormat::Hlg:
            hdr.format_name = "HLG";
            hdr.hdr = true;
            break;
        case model::HdrFormat::Sdr:
            hdr.format_name = "SDR";
            hdr.hdr = false;
            break;
        case model::HdrFormat::Unknown:
        default:
            hdr.format_name = "未知";
            hdr.hdr = false;
            break;
    }
    finished_ = true;
    return result_;
}

}  // namespace videoeye
