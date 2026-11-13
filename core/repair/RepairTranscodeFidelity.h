#pragma once

// 阶段 6 / 切片 J：S08 重编码前的「源侧保真属性」探测。
//
// S08 把解码帧统一重编码成 8-bit yuv420p。位深、色彩标记、旋转矩阵、HDR
// 元数据、动态分辨率这些属性不会跟着画面一起过去，旧实现既不继承也不报告。
//
// 这里只做**实测**：拿首帧 + 输入流 + 编码器实际选定的像素格式逐项对照，
// 能零风险继承的交给编码器，其余逐条写进 lost_properties。绝不做「源大概率
// 是 SDR」这类推断 —— 没探测到证据就保持 inspected=false。
//
// 边界（必须随结论一起报出去）：
//   * 只看首帧与容器声明，不扫描全程；动态分辨率由调用方在逐帧循环里补记；
//   * 旋转矩阵 / HDR 元数据本版只如实记账，不尝试继承（见 §4.10）；
//   * 色彩标记只在位深不变时继承：给 8-bit SDR 画面贴 bt2020/smpte2084
//     会让播放器按 HDR 渲染，比不写标记更糟。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include "core/domain/model/RepairTypes.h"

namespace videoeye {
namespace repair {

inline int TranscodeBitDepth(AVPixelFormat format) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    if (desc == nullptr || desc->nb_components == 0)
        return 0;
    return desc->comp[0].depth;
}

inline std::string TranscodePixFmtName(AVPixelFormat format) {
    const char* name = av_get_pix_fmt_name(format);
    return name != nullptr ? std::string(name) : std::string("unknown");
}

inline std::string TranscodeColorName(const char* name) {
    return name != nullptr ? std::string(name) : std::string();
}

// 哪些属性真的被写进了编码器（决定 preserved/lost 的归类，不许凭意图记账）。
struct RepairTranscodeInheritance {
    bool color_tags_copied = false;
    bool sample_aspect_ratio_copied = false;
    // AVCodecContext 没有 chroma_location（FFmpeg 8 起只在 AVFrame /
    // AVCodecParameters 上），必须单独写到转换后帧里才算真的继承。
    bool chroma_location_copied = false;
};

inline RepairTranscodeInheritance ApplyTranscodeInheritance(AVCodecContext* encoder, const AVStream* stream,
                                                            const AVFrame* first_frame, int source_bit_depth,
                                                            int output_bit_depth) {
    RepairTranscodeInheritance applied;
    if (encoder == nullptr || stream == nullptr || stream->codecpar == nullptr)
        return applied;
    const AVCodecParameters* par = stream->codecpar;

    if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
        encoder->sample_aspect_ratio = par->sample_aspect_ratio;
        applied.sample_aspect_ratio_copied = true;
    } else if (first_frame != nullptr && first_frame->sample_aspect_ratio.num > 0 &&
               first_frame->sample_aspect_ratio.den > 0) {
        encoder->sample_aspect_ratio = first_frame->sample_aspect_ratio;
        applied.sample_aspect_ratio_copied = true;
    }

    // 位深变了就不能搬色彩标记：标记描述的是源色彩体系，不是重编码后的画面。
    if (source_bit_depth <= 0 || source_bit_depth != output_bit_depth)
        return applied;

    const AVColorPrimaries primaries = first_frame != nullptr && first_frame->color_primaries != AVCOL_PRI_UNSPECIFIED
                                           ? first_frame->color_primaries
                                           : par->color_primaries;
    const AVColorTransferCharacteristic transfer =
        first_frame != nullptr && first_frame->color_trc != AVCOL_TRC_UNSPECIFIED ? first_frame->color_trc
                                                                                  : par->color_trc;
    const AVColorSpace space = first_frame != nullptr && first_frame->colorspace != AVCOL_SPC_UNSPECIFIED
                                   ? first_frame->colorspace
                                   : par->color_space;
    const AVColorRange range = first_frame != nullptr && first_frame->color_range != AVCOL_RANGE_UNSPECIFIED
                                   ? first_frame->color_range
                                   : par->color_range;
    const bool has_any = primaries != AVCOL_PRI_UNSPECIFIED || transfer != AVCOL_TRC_UNSPECIFIED ||
                         space != AVCOL_SPC_UNSPECIFIED || range != AVCOL_RANGE_UNSPECIFIED;
    if (!has_any)
        return applied;

    encoder->color_primaries = primaries;
    encoder->color_trc = transfer;
    encoder->colorspace = space;
    encoder->color_range = range;
    applied.color_tags_copied = true;
    return applied;
}

// 色度采样位置只能落在 AVFrame 上；converted 在整段循环里只 make_writable、
// 从不 unref，所以设一次就够。返回「确实写进去了」，供 preserved/lost 归类。
inline bool ApplyTranscodeFrameProperties(AVFrame* output_frame, const AVStream* stream) {
    if (output_frame == nullptr || stream == nullptr || stream->codecpar == nullptr)
        return false;
    if (stream->codecpar->chroma_location == AVCHROMA_LOC_UNSPECIFIED)
        return false;
    output_frame->chroma_location = stream->codecpar->chroma_location;
    return true;
}

// 逐帧缩放状态：把「sws 是针对哪种源尺寸建的」和「有多少帧尺寸与产物不同」
// 放在一起，避免调用方一边用固定 context 一边另设一个计数器、两处慢慢漂移。
//
// Observe() 记实测计数并返回是否需要重建 sws；只有重建**成功**才 Commit()，
// 否则下一帧同尺寸仍会再次尝试（重建失败时该帧直接跳过，绝不喂给旧 context）。
struct TranscodeScaleState {
    int output_width = 0;
    int output_height = 0;
    int source_width = 0;
    int source_height = 0;
    int source_format = AV_PIX_FMT_NONE;
    std::uint64_t resolution_changed_frames = 0;

    bool Observe(const AVFrame* frame) {
        if (frame == nullptr)
            return false;
        if (frame->width != output_width || frame->height != output_height)
            ++resolution_changed_frames;
        return frame->width != source_width || frame->height != source_height || frame->format != source_format;
    }

    void Commit(const AVFrame* frame) {
        if (frame == nullptr)
            return;
        source_width = frame->width;
        source_height = frame->height;
        source_format = frame->format;
    }
};

inline std::string TranscodeColorTagList(const model::RepairTranscodeFidelity& fidelity) {
    std::string out;
    const auto append = [&out](const char* label, const std::string& value) {
        if (value.empty())
            return;
        if (!out.empty())
            out += "/";
        out += label;
        out += "=";
        out += value;
    };
    append("primaries", fidelity.color_primaries);
    append("transfer", fidelity.color_transfer);
    append("space", fidelity.color_space);
    append("range", fidelity.color_range);
    return out;
}

// 编码器事实的唯一渲染入口（切片 O）。报告、限制项、验收断言都从这里取，
// 避免能力契约 / 排除项 / 报告三处各写一份「libx264」然后互相矛盾。
//
// 空 encoder_used 表示这次根本没走到编码器（未转码 / 打开失败），返回空串，
// 调用方不得自己补一个默认名。
inline std::string TranscodeEncoderLabel(const model::RepairTranscodeFidelity& fidelity) {
    if (fidelity.encoder_used.empty())
        return std::string();
    if (!fidelity.encoder_fallback)
        return fidelity.encoder_used;
    const std::string preferred = fidelity.encoder_preferred.empty() ? std::string("未知") : fidelity.encoder_preferred;
    return fidelity.encoder_used + "（首选 " + preferred + " 在本构建不可用，已回退）";
}

// profile 比对只在两端都有实测值时成立：源侧没声明（AV_PROFILE_UNKNOWN）
// 就不写；产物侧拿不到编码器回报就明说「未实测」，不推断「大概仍一致」。
inline std::string TranscodeProfileNote(const model::RepairTranscodeFidelity& fidelity) {
    if (fidelity.source_profile.empty())
        return std::string();
    if (fidelity.output_profile.empty())
        return "源侧 profile=" + fidelity.source_profile + "，产物 profile 编码器未回报，未能比对";
    if (fidelity.output_profile == fidelity.source_profile)
        return "源侧与产物 profile 一致（" + fidelity.source_profile + "）";
    return "源侧 profile=" + fidelity.source_profile + "，产物 profile=" + fidelity.output_profile + "（已改变）";
}

inline model::RepairTranscodeFidelity InspectTranscodeFidelity(const AVStream* stream, const AVFrame* first_frame,
                                                               AVPixelFormat output_format,
                                                               const RepairTranscodeInheritance& applied) {
    model::RepairTranscodeFidelity fidelity;
    // 没有首帧 / 没有流参数 = 没有证据。inspected 保持 false，空表不许冒充「无损失」。
    if (stream == nullptr || stream->codecpar == nullptr || first_frame == nullptr)
        return fidelity;
    const AVCodecParameters* par = stream->codecpar;
    const AVPixelFormat source_format = static_cast<AVPixelFormat>(first_frame->format);

    fidelity.inspected = true;
    fidelity.source_pixel_format = TranscodePixFmtName(source_format);
    fidelity.output_pixel_format = TranscodePixFmtName(output_format);
    fidelity.source_bit_depth = TranscodeBitDepth(source_format);
    fidelity.output_bit_depth = TranscodeBitDepth(output_format);
    fidelity.source_width = first_frame->width;
    fidelity.source_height = first_frame->height;

    const AVColorPrimaries primaries =
        first_frame->color_primaries != AVCOL_PRI_UNSPECIFIED ? first_frame->color_primaries : par->color_primaries;
    const AVColorTransferCharacteristic transfer =
        first_frame->color_trc != AVCOL_TRC_UNSPECIFIED ? first_frame->color_trc : par->color_trc;
    const AVColorSpace space =
        first_frame->colorspace != AVCOL_SPC_UNSPECIFIED ? first_frame->colorspace : par->color_space;
    const AVColorRange range =
        first_frame->color_range != AVCOL_RANGE_UNSPECIFIED ? first_frame->color_range : par->color_range;
    if (primaries != AVCOL_PRI_UNSPECIFIED)
        fidelity.color_primaries = TranscodeColorName(av_color_primaries_name(primaries));
    if (transfer != AVCOL_TRC_UNSPECIFIED)
        fidelity.color_transfer = TranscodeColorName(av_color_transfer_name(transfer));
    if (space != AVCOL_SPC_UNSPECIFIED)
        fidelity.color_space = TranscodeColorName(av_color_space_name(space));
    if (range != AVCOL_RANGE_UNSPECIFIED)
        fidelity.color_range = TranscodeColorName(av_color_range_name(range));
    if (par->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
        fidelity.chroma_location = TranscodeColorName(av_chroma_location_name(par->chroma_location));
    if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
        fidelity.sample_aspect_ratio =
            std::to_string(par->sample_aspect_ratio.num) + ":" + std::to_string(par->sample_aspect_ratio.den);
    }

    // 旋转 / 镜像矩阵：FFmpeg 8 起流级 side data 挂在 codecpar->coded_side_data，
    // 解码后的帧也可能带一份；两处任一存在都算「源片有方向信息」。
    const AVPacketSideData* display =
        av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    const AVFrameSideData* frame_display = av_frame_get_side_data(first_frame, AV_FRAME_DATA_DISPLAYMATRIX);
    const std::uint8_t* display_data = display != nullptr ? display->data : nullptr;
    std::size_t display_size = display != nullptr ? display->size : 0;
    if (display_data == nullptr && frame_display != nullptr) {
        display_data = frame_display->data;
        display_size = frame_display->size;
    }
    if (display_data != nullptr && display_size >= 9 * sizeof(std::int32_t)) {
        std::int32_t matrix[9] = {};
        std::memcpy(matrix, display_data, sizeof(matrix));
        fidelity.rotation_degrees = av_display_rotation_get(matrix);
        fidelity.has_display_matrix = std::abs(fidelity.rotation_degrees) > 0.5 || matrix[0] < 0 || matrix[4] < 0;
    }

    const AVPacketSideData* mastering =
        av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    const AVPacketSideData* light =
        av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    fidelity.has_hdr_metadata =
        mastering != nullptr || light != nullptr ||
        av_frame_get_side_data(first_frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) != nullptr ||
        av_frame_get_side_data(first_frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) != nullptr;

    if (fidelity.source_bit_depth > 0 && fidelity.output_bit_depth > 0 &&
        fidelity.source_bit_depth > fidelity.output_bit_depth) {
        fidelity.lost_properties.push_back("位深 " + std::to_string(fidelity.source_bit_depth) + " → " +
                                           std::to_string(fidelity.output_bit_depth) +
                                           " bit：高比特深度色阶被压到低位深，无法恢复");
    }
    const AVPixFmtDescriptor* src_desc = av_pix_fmt_desc_get(source_format);
    const AVPixFmtDescriptor* out_desc = av_pix_fmt_desc_get(output_format);
    if (src_desc != nullptr && out_desc != nullptr &&
        (src_desc->log2_chroma_w != out_desc->log2_chroma_w || src_desc->log2_chroma_h != out_desc->log2_chroma_h)) {
        fidelity.lost_properties.push_back("色度抽样 " + fidelity.source_pixel_format + " → " +
                                           fidelity.output_pixel_format + "：色度分辨率下降，无法恢复");
    }
    const std::string color_tags = TranscodeColorTagList(fidelity);
    if (!color_tags.empty()) {
        if (applied.color_tags_copied) {
            fidelity.preserved_properties.push_back("色彩标记（" + color_tags + "）已原样继承到编码器");
        } else {
            fidelity.lost_properties.push_back("色彩标记（" + color_tags +
                                               "）未继承：位深已改变，照搬会把重编码画面错标成源色彩空间");
        }
    }
    if (applied.sample_aspect_ratio_copied && !fidelity.sample_aspect_ratio.empty()) {
        fidelity.preserved_properties.push_back("像素宽高比 " + fidelity.sample_aspect_ratio + " 已原样继承到编码器");
    }
    if (!fidelity.chroma_location.empty()) {
        if (applied.chroma_location_copied) {
            fidelity.preserved_properties.push_back("色度采样位置 " + fidelity.chroma_location + " 已继承到转换后帧");
        } else {
            fidelity.lost_properties.push_back("色度采样位置 " + fidelity.chroma_location + " 未继承");
        }
    }
    if (fidelity.has_display_matrix) {
        char degrees[32] = {};
        std::snprintf(degrees, sizeof(degrees), "%.1f", fidelity.rotation_degrees);
        fidelity.lost_properties.push_back(std::string("画面旋转 / 镜像矩阵（") + degrees +
                                           "°）未继承：产物画面方向可能与源片不同");
    }
    if (fidelity.has_hdr_metadata) {
        fidelity.lost_properties.push_back(
            "HDR 静态元数据（mastering display / content light level）未继承：重编码产物不能沿用源 HDR 标记");
    }

    fidelity.all_properties_preserved = fidelity.lost_properties.empty();
    return fidelity;
}

// 逐帧循环实测出的动态分辨率帧数要并进同一个结论；直接改列表比事后在报告层
// 拼接可靠（报告层拿不到逐帧计数）。
inline void FinalizeTranscodeFidelity(model::RepairTranscodeFidelity& fidelity,
                                      std::uint64_t resolution_changed_frames) {
    // 没探测过就没有可归属的实测值：计数归零、结论保持 false。
    // 否则「转码中途失败」的空表会被 all_properties_preserved=true 自证成完好。
    if (!fidelity.inspected) {
        fidelity.resolution_changed_frames = 0;
        fidelity.all_properties_preserved = false;
        return;
    }
    fidelity.resolution_changed_frames = resolution_changed_frames;
    if (resolution_changed_frames > 0) {
        fidelity.lost_properties.push_back("动态分辨率：" + std::to_string(resolution_changed_frames) +
                                           " 帧的尺寸与首帧不同，产物按首帧尺寸缩放，不是原样保留");
    }
    fidelity.all_properties_preserved = fidelity.lost_properties.empty();
}

} // namespace repair
} // namespace videoeye
