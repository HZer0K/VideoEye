#include "core/model/ColorInfo.h"

#include <algorithm>

namespace videoeye {
namespace model {

// 常量镜像见 core/model/ColorInfo.h 的 ffmpeg_expect 命名空间
using namespace ffmpeg_expect;

namespace {

std::string Lower(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;  // text.compare(0, strlen(prefix), prefix) == 0
}
}  // namespace

const char* ToString(ColorPrimariesKind kind) {
    switch (kind) {
        case ColorPrimariesKind::Unspecified: return "未标注";
        case ColorPrimariesKind::Bt601:       return "BT.601";
        case ColorPrimariesKind::Bt709:       return "BT.709";
        case ColorPrimariesKind::Bt2020:      return "BT.2020";
        case ColorPrimariesKind::DisplayP3:   return "Display P3";
        case ColorPrimariesKind::DciP3:       return "DCI-P3";
        case ColorPrimariesKind::Smpte240M:   return "SMPTE 240M";
        case ColorPrimariesKind::Film:        return "Film";
        case ColorPrimariesKind::Smpte428:    return "SMPTE 428";
        case ColorPrimariesKind::Other:       return "其他";
    }
    return "未知";
}

const char* ToString(TransferKind kind) {
    switch (kind) {
        case TransferKind::Unspecified: return "未标注";
        case TransferKind::Sdr:         return "SDR";
        case TransferKind::Pq:          return "PQ";
        case TransferKind::Hlg:         return "HLG";
        case TransferKind::Linear:      return "Linear";
        case TransferKind::Log:         return "Log";
        case TransferKind::Srgb:        return "sRGB";
        case TransferKind::Other:       return "其他";
    }
    return "未知";
}

const char* ToString(MatrixKind kind) {
    switch (kind) {
        case MatrixKind::Unspecified: return "未标注";
        case MatrixKind::Rgb:         return "RGB";
        case MatrixKind::Bt601:       return "BT.601";
        case MatrixKind::Bt709:       return "BT.709";
        case MatrixKind::Bt2020Ncl:   return "BT.2020 NCL";
        case MatrixKind::Bt2020Cl:    return "BT.2020 CL";
        case MatrixKind::YCgCo:       return "YCgCo";
        case MatrixKind::ICtCp:       return "ICtCp";
        case MatrixKind::Other:       return "其他";
    }
    return "未知";
}

const char* ToString(ColorRangeKind kind) {
    switch (kind) {
        case ColorRangeKind::Unspecified: return "未标注";
        case ColorRangeKind::Limited:     return "Limited";
        case ColorRangeKind::Full:        return "Full";
    }
    return "未知";
}

ColorPrimariesKind ClassifyPrimariesRaw(int av_primaries) {
    switch (av_primaries) {
        case kPriUnspecified:
        case kPriReserved0:
        case kPriReserved:
            return ColorPrimariesKind::Unspecified;
        case kPriBt709:
            return ColorPrimariesKind::Bt709;
        case kPriBt470M:
        case kPriBt470Bg:
        case kPriSmpte170M:
            return ColorPrimariesKind::Bt601;
        case kPriSmpte240M:
            return ColorPrimariesKind::Smpte240M;
        case kPriFilm:
            return ColorPrimariesKind::Film;
        case kPriBt2020:
            return ColorPrimariesKind::Bt2020;
        case kPriSmpte428:
            return ColorPrimariesKind::Smpte428;
        case kPriSmpte431:
            return ColorPrimariesKind::DciP3;
        case kPriSmpte432:
            return ColorPrimariesKind::DisplayP3;
        default:
            return ColorPrimariesKind::Other;
    }
}

TransferKind ClassifyTransferRaw(int av_transfer) {
    switch (av_transfer) {
        case kTrcUnspecified:
        case kTrcReserved0:
        case kTrcReserved:
            return TransferKind::Unspecified;
        case kTrcSmpte2084:
            return TransferKind::Pq;
        case kTrcAribStdB67:
            return TransferKind::Hlg;
        case kTrcLinear:
            return TransferKind::Linear;
        case kTrcLog:
        case kTrcLogSqrt:
            return TransferKind::Log;
        case kTrcIec61966_2_1:
            return TransferKind::Srgb;
        case kTrcBt709:
        case kTrcGamma22:
        case kTrcGamma28:
        case kTrcSmpte170M:
        case kTrcSmpte240M:
        case kTrcBt2020_10:
        case kTrcBt2020_12:
        case kTrcIec61966_2_4:
        case kTrcBt1361Ecg:
        case kTrcSmpte428:
            return TransferKind::Sdr;
        default:
            return TransferKind::Other;
    }
}

MatrixKind ClassifyMatrixRaw(int av_matrix) {
    switch (av_matrix) {
        case kSpcUnspecified:
        case kSpcReserved:
            return MatrixKind::Unspecified;
        case kSpcRgb:
            return MatrixKind::Rgb;
        case kSpcBt709:
            return MatrixKind::Bt709;
        case kSpcFcc:
        case kSpcBt470Bg:
        case kSpcSmpte170M:
        case kSpcSmpte240M:
            return MatrixKind::Bt601;
        case kSpcBt2020Ncl:
        case kSpcChromaDerivedNcl:
            return MatrixKind::Bt2020Ncl;
        case kSpcBt2020Cl:
        case kSpcChromaDerivedCl:
            return MatrixKind::Bt2020Cl;
        case kSpcYCgCo:
            return MatrixKind::YCgCo;
        case kSpcSmpte2085:
        case kSpcICtCp:
            return MatrixKind::ICtCp;
        default:
            return MatrixKind::Other;
    }
}

ColorRangeKind ClassifyRangeRaw(int av_range) {
    switch (av_range) {
        case kRangeMpeg: return ColorRangeKind::Limited;
        case kRangeJpeg: return ColorRangeKind::Full;
        default:         return ColorRangeKind::Unspecified;
    }
}

std::string PrimariesDisplayName(int av_primaries) {
    switch (av_primaries) {
        case kPriBt709:      return "BT.709";
        case kPriBt2020:     return "BT.2020";
        case kPriSmpte432:   return "Display P3";
        case kPriSmpte431:   return "DCI-P3";
        case kPriSmpte428:   return "SMPTE 428";
        case kPriSmpte240M:  return "SMPTE 240M";
        case kPriFilm:       return "Film";
        case kPriBt470M:     return "BT.470 M";
        case kPriBt470Bg:    return "BT.470 BG";
        case kPriSmpte170M:  return "BT.601 NTSC";
        default: break;
    }
    const ColorPrimariesKind kind = ClassifyPrimariesRaw(av_primaries);
    if (kind == ColorPrimariesKind::Unspecified) return {};
    if (kind == ColorPrimariesKind::Bt601 && av_primaries == kPriBt470Bg) return "BT.601 PAL";
    if (kind == ColorPrimariesKind::Other) return "其他(" + std::to_string(av_primaries) + ")";
    return ToString(kind);
}

std::string TransferDisplayName(int av_transfer) {
    switch (av_transfer) {
        case kTrcBt709:         return "BT.709";
        case kTrcSmpte2084:     return "PQ";
        case kTrcAribStdB67:    return "HLG";
        case kTrcLinear:        return "Linear";
        case kTrcGamma22:       return "Gamma 2.2";
        case kTrcGamma28:       return "Gamma 2.8";
        case kTrcSmpte170M:     return "BT.601";
        case kTrcSmpte240M:     return "SMPTE 240M";
        case kTrcBt2020_10:     return "BT.2020 (10bit)";
        case kTrcBt2020_12:     return "BT.2020 (12bit)";
        case kTrcIec61966_2_1:  return "sRGB";
        case kTrcIec61966_2_4:  return "IEC 61966-2-4";
        case kTrcBt1361Ecg:     return "BT.1361 ECG";
        case kTrcSmpte428:      return "SMPTE 428";
        case kTrcLog:           return "Log";
        case kTrcLogSqrt:       return "Log Sqrt";
        default: break;
    }
    const TransferKind kind = ClassifyTransferRaw(av_transfer);
    if (kind == TransferKind::Unspecified) return {};
    if (kind == TransferKind::Other) return "其他(" + std::to_string(av_transfer) + ")";
    return ToString(kind);
}

std::string MatrixDisplayName(int av_matrix) {
    switch (av_matrix) {
        case kSpcRgb:                return "GBR";
        case kSpcBt709:              return "BT.709";
        case kSpcFcc:                return "FCC";
        case kSpcBt470Bg:            return "BT.470 BG";
        case kSpcSmpte170M:          return "BT.601";
        case kSpcSmpte240M:          return "SMPTE 240M";
        case kSpcBt2020Ncl:          return "BT.2020 NCL";
        case kSpcBt2020Cl:           return "BT.2020 CL";
        case kSpcYCgCo:              return "YCgCo";
        case kSpcSmpte2085:          return "SMPTE 2085";
        case kSpcChromaDerivedNcl:   return "Chroma NCL";
        case kSpcChromaDerivedCl:    return "Chroma CL";
        case kSpcICtCp:              return "ICtCp";
        default: break;
    }
    const MatrixKind kind = ClassifyMatrixRaw(av_matrix);
    if (kind == MatrixKind::Unspecified) return {};
    if (kind == MatrixKind::Other) return "其他(" + std::to_string(av_matrix) + ")";
    return ToString(kind);
}

std::string RangeDisplayName(int av_range) {
    const ColorRangeKind kind = ClassifyRangeRaw(av_range);
    if (kind == ColorRangeKind::Unspecified) return {};
    return ToString(kind);
}

bool PixelFormatInfo::ImpliesFullRange() const {
    return StartsWith(Lower(name), "yuvj");
}

int ColorInfo::EffectiveBitDepth() const {
    if (pixel_format.bit_depth > 0) return pixel_format.bit_depth;
    return pixel_format.bits_per_raw_sample;
}

std::string ColorInfo::ToString() const {
    auto piece = [](const std::string& text) -> std::string {
        return text.empty() ? std::string("未标注") : text;
    };
    std::string out = piece(primaries_name) + " / " + piece(transfer_name) + " / " +
                      piece(matrix_name) + " / " + piece(range_name);
    const int depth = EffectiveBitDepth();
    if (depth > 0) out += " / " + std::to_string(depth) + "bit";
    if (!pixel_format.chroma_subsampling.empty()) {
        out += " / " + pixel_format.chroma_subsampling;
    }
    return out;
}

}  // namespace model
}  // namespace videoeye
