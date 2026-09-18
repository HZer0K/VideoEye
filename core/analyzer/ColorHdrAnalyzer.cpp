#include "core/analyzer/ColorHdrAnalyzer.h"

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
namespace analyzer {
namespace {

// ---- core/model/ColorInfo.h 里镜像的 FFmpeg 枚举值交叉校验 ----
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

// ============================ ColorHdrAnalysis ============================

bool ColorHdrAnalysis::StaticMetadataComplete() const {
    return hdr.mastering_display.Complete() && hdr.content_light.Complete();
}

std::string ColorHdrAnalysis::ToString() const {
    if (!analyzed) return "未执行色彩/HDR 分析";
    std::string out = hdr.format_name;
    out += " ｜ " + color.ToString();
    if (hdr.mastering_display.Complete()) {
        out += " ｜ MaxCLL " + std::to_string(hdr.content_light.max_cll) + " / MaxFALL " +
               std::to_string(hdr.content_light.max_fall);
    }
    return out;
}

// ============================ 行构造（UI / 报告共用） ============================

std::vector<ColorKeyValueRow> BuildColorRows(const ColorHdrAnalysis& analysis) {
    std::vector<ColorKeyValueRow> rows;
    if (!analysis.analyzed) return rows;

    const model::ColorInfo& c = analysis.color;
    auto add = [&rows](const std::string& key, const std::string& value, const std::string& note) {
        rows.push_back(ColorKeyValueRow{key, value, note});
    };

    add("像素格式", c.pixel_format.name.empty() ? "未标注" : c.pixel_format.name,
        c.pixel_format.bit_depth > 0
            ? ("每分量 " + std::to_string(c.pixel_format.bit_depth) + " 位")
            : "");

    const int depth = c.EffectiveBitDepth();
    add("位深", depth > 0 ? (std::to_string(depth) + " bit") : "未标注",
        depth >= 10 ? "高位深：HDR / 10bit 分发的最低要求"
                    : (depth == 8 ? "8 bit：SDR 常规位深，承载 PQ/HLG 会出现明显色带" : ""));

    add("色度采样", c.pixel_format.chroma_subsampling.empty() ? "RGB / 无" : c.pixel_format.chroma_subsampling,
        c.pixel_format.rgb ? "RGB 像素格式不存在色度下采样" : "");

    add("色彩原色 (Primaries)", c.primaries_name.empty() ? "未标注" : c.primaries_name,
        c.IsWideGamutPrimaries() ? "广色域：需搭配 BT.2020 矩阵与 PQ/HLG 传递函数"
                                 : "标准色域");

    add("传递函数 (Transfer)", c.transfer_name.empty() ? "未标注" : c.transfer_name,
        c.transfer == model::TransferKind::Pq  ? "SMPTE ST 2084，HDR10 的 EOTF"
        : c.transfer == model::TransferKind::Hlg ? "ARIB STD-B67，广播电视 HDR"
        : c.transfer == model::TransferKind::Sdr ? "传统 gamma 曲线（SDR）"
                                                 : "");

    add("矩阵系数 (Matrix)", c.matrix_name.empty() ? "未标注" : c.matrix_name,
        "决定 YUV ↔ RGB 转换，与 primaries 不一致会导致整体偏色");

    add("量化范围 (Range)", c.range_name.empty() ? "未标注" : c.range_name,
        (c.range == model::ColorRangeKind::Limited)
            ? "Limited：8bit 下有效值 16-235"
            : (c.range == model::ColorRangeKind::Full ? "Full：8bit 下有效值 0-255"
                                                      : "播放器通常按 Limited 猜测"));

    add("编码 Profile", c.profile_name.empty() ? "未标注" : c.profile_name,
        c.level > 0 ? ("Level " + std::to_string(c.level)) : "");

    if (c.width > 0 && c.height > 0) {
        add("分辨率", std::to_string(c.width) + " × " + std::to_string(c.height), "");
    }
    return rows;
}

std::vector<ColorKeyValueRow> BuildHdrRows(const ColorHdrAnalysis& analysis) {
    std::vector<ColorKeyValueRow> rows;
    if (!analysis.analyzed) return rows;

    const model::HdrMetadataInfo& h = analysis.hdr;
    const model::ColorInfo& c = analysis.color;
    auto add = [&rows](const std::string& key, const std::string& value, const std::string& note) {
        rows.push_back(ColorKeyValueRow{key, value, note});
    };

    add("HDR 格式", h.format_name.empty() ? model::ToString(h.format) : h.format_name,
        h.hdr ? "高动态范围内容" : "未识别到 HDR 线索");

    add("母版显示色域 (Primaries)",
        h.mastering_display.has_primaries ? h.mastering_display.PrimariesText() : "缺失",
        h.mastering_display.present ? "SMPTE ST 2086" : "未找到 SMPTE ST 2086 mastering display metadata");

    add("母版显示亮度 (Luminance)",
        h.mastering_display.has_luminance ? h.mastering_display.LuminanceText() : "缺失",
        h.mastering_display.has_luminance
            ? ("母版最低/最高亮度，播放端据此做色调映射")
            : "缺少该值时播放器通常按默认 1000 nit 映射");

    add("MaxCLL", h.content_light.present && h.content_light.max_cll > 0
                      ? (std::to_string(h.content_light.max_cll) + " cd/m²")
                      : "缺失",
        "CTA-861.3 最大内容光level");

    add("MaxFALL", h.content_light.present && h.content_light.max_fall > 0
                       ? (std::to_string(h.content_light.max_fall) + " cd/m²")
                       : "缺失",
        "CTA-861.3 最大帧平均光level");

    if (h.dolby_vision.present) {
        add("Dolby Vision Profile", h.dolby_vision.ProfileText(),
            h.dolby_vision.el_present ? "含增强层 (EL)" : "单层 (仅基础层)");
        add("Dolby Vision Level", std::to_string(h.dolby_vision.level), "");
        add("DV 兼容层", h.dolby_vision.CompatibilityText(),
            h.dolby_vision.compatibility_id == 0
                ? "非 DV 设备可能出现错误的颜色与亮度"
                : "存在向下兼容层，非 DV 设备可按 SDR/HDR10 显示");
        add("DV RPU (动态元数据)", h.dolby_vision.rpu_present ? "存在" : "未标注",
            h.dolby_vision.rpu_present ? "每帧动态 tones mapping 数据" : "");
    } else {
        add("Dolby Vision", "未检测到", "未找到 DOVI configuration record");
    }

    add("HDR10+ 动态元数据", h.has_hdr10_plus ? "存在" : "无",
        h.has_hdr10_plus ? "SMPTE ST 2094-40 逐场景/逐帧动态元数据" : "");

    if (h.has_hdr_vivid) {
        add("HDR Vivid", "存在", "CUVA 005 动态元数据");
    }

    std::string sources;
    for (size_t i = 0; i < h.sources.size(); ++i) {
        if (i > 0) sources += "、";
        sources += h.sources[i];
    }
    add("数据来源", sources.empty() ? "仅 AVCodecParameters" : sources,
        c.frame_side_data_checked ? "已尝试解码帧读取 AVFrame side data"
                                  : "未解码视频帧（动态元数据可能漏检）");

    return rows;
}

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

}  // namespace analyzer
}  // namespace videoeye
