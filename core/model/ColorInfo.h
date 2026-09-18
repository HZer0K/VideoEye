#pragma once

#include <array>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// ==========================================================================
// 色彩描述模型（刻意不依赖 FFmpeg / Qt）
//
// 约定：
//   - 所有 "Raw" 取值是 FFmpeg 枚举的数值（AVColorPrimaries / AVColorTransferCharacteristic /
//     AVColorSpace / AVColorRange），由 core/analyzer/ColorHdrAnalyzer.cpp 负责转换。
//     ColorHdrAnalyzer.cpp 里的 static_assert 会校验两者的数值关系，
//     FFmpeg 版本一旦改动这些枚举值会直接编译失败（而不是静默错位）。
//   - 规则判定与 UI 只使用语义枚举（ColorPrimariesKind / TransferKind / ...），
//     这样 QC 规则与单元测试无需链接 FFmpeg（与 AudioQcResult 同一思路）。
//   - Raw 数值保留下来是为了排障：遇到尚未收录的枚举值时仍能显示原始编号。
// ==========================================================================

// 色域（原色）的语义分类
enum class ColorPrimariesKind {
    Unspecified = 0,  // 未标注（AVCOL_PRI_UNSPECIFIED）
    Bt601,            // BT.601 / BT.470 M/BG，SD 时代
    Bt709,            // BT.709，HD 标准 SDR 色域
    Bt2020,           // BT.2020，UHDTV / HDR 广色域
    DisplayP3,        // SMPTE ST 432（Apple Display P3）
    DciP3,            // SMPTE ST 431（影院 DCI-P3）
    Smpte240M,        // SMPTE 240M
    Film,             // Illuminant C（胶片）
    Smpte428,         // SMPTE ST 428（DCI XYZ）
    Other,
};

// 光电传递函数的语义分类
enum class TransferKind {
    Unspecified = 0,
    Sdr,     // 传统 gamma：BT.709 / BT.1886 / BT.601 / Gamma 2.2 / SMPTE 240M
    Pq,      // SMPTE ST 2084（HDR10 / Dolby Vision 使用的 PQ 曲线）
    Hlg,     // ARIB STD-B67（HLG，广播电视 HDR）
    Linear,  // 线性（无 gamma）
    Log,     // 摄影机 log 曲线（S-Log / Cineon 等）
    Srgb,    // IEC 61966-2-1（sRGB / BT.1361 ECG）
    Other,
};

// YUV<->RGB 转换矩阵的语义分类
enum class MatrixKind {
    Unspecified = 0,
    Rgb,         // GBR / 无矩阵（identity）
    Bt601,       // BT.601 / BT.470BG / FCC
    Bt709,
    Bt2020Ncl,   // BT.2020 non-constant luminance（PQ / HLG 的常规搭配）
    Bt2020Cl,    // BT.2020 constant luminance
    YCgCo,       // YCgCo / YCgCo-Re
    ICtCp,       // ICtCp（Dolby Vision 常用）
    Other,
};

// 量化范围
enum class ColorRangeKind {
    Unspecified = 0,
    Limited,  // TV / MPEG：8bit 下 16..235（10bit 64..940）
    Full,     // PC / JPEG：8bit 下 0..255（10bit 0..1023）
};

const char* ToString(ColorPrimariesKind kind);
const char* ToString(TransferKind kind);
const char* ToString(MatrixKind kind);
const char* ToString(ColorRangeKind kind);

// ==========================================================================
// FFmpeg 枚举数值镜像
//
// 本层不 include FFmpeg 头文件，改为把 AVColorPrimaries / AVColorTransferCharacteristic /
// AVColorSpace / AVColorRange 的取值在这里镜像一份。ColorHdrAnalyzer.cpp 里有一组
// static_assert 把 FFmpeg 真实取值与这里的常量对齐，一旦上游改动枚举值会在编译期报错，
// 而不是把 PQ 静默判成 BT.709 这类致命误判。
// ==========================================================================
namespace ffmpeg_expect {

// AVColorPrimaries
constexpr int kPriReserved0  = 0;
constexpr int kPriBt709      = 1;
constexpr int kPriUnspecified = 2;
constexpr int kPriReserved   = 3;
constexpr int kPriBt470M     = 4;
constexpr int kPriBt470Bg    = 5;
constexpr int kPriSmpte170M  = 6;
constexpr int kPriSmpte240M  = 7;
constexpr int kPriFilm       = 8;
constexpr int kPriBt2020     = 9;
constexpr int kPriSmpte428   = 10;
constexpr int kPriSmpte431   = 11;
constexpr int kPriSmpte432   = 12;

// AVColorTransferCharacteristic
constexpr int kTrcReserved0    = 0;
constexpr int kTrcBt709        = 1;
constexpr int kTrcUnspecified  = 2;
constexpr int kTrcReserved     = 3;
constexpr int kTrcGamma22      = 4;
constexpr int kTrcGamma28      = 5;
constexpr int kTrcSmpte170M    = 6;
constexpr int kTrcSmpte240M    = 7;
constexpr int kTrcLinear       = 8;
constexpr int kTrcLog          = 9;
constexpr int kTrcLogSqrt      = 10;
constexpr int kTrcIec61966_2_4 = 11;
constexpr int kTrcBt1361Ecg    = 12;
constexpr int kTrcIec61966_2_1 = 13;
constexpr int kTrcBt2020_10    = 14;
constexpr int kTrcBt2020_12    = 15;
constexpr int kTrcSmpte2084    = 16;
constexpr int kTrcSmpte428     = 17;
constexpr int kTrcAribStdB67   = 18;

// AVColorSpace
constexpr int kSpcRgb               = 0;
constexpr int kSpcBt709             = 1;
constexpr int kSpcUnspecified       = 2;
constexpr int kSpcReserved          = 3;
constexpr int kSpcFcc               = 4;
constexpr int kSpcBt470Bg           = 5;
constexpr int kSpcSmpte170M         = 6;
constexpr int kSpcSmpte240M         = 7;
constexpr int kSpcYCgCo             = 8;
constexpr int kSpcBt2020Ncl         = 9;
constexpr int kSpcBt2020Cl          = 10;
constexpr int kSpcSmpte2085         = 11;
constexpr int kSpcChromaDerivedNcl  = 12;
constexpr int kSpcChromaDerivedCl   = 13;
constexpr int kSpcICtCp             = 14;

// AVColorRange
constexpr int kRangeUnspecified = 0;
constexpr int kRangeMpeg        = 1;   // limited (16..235 / 64..940)
constexpr int kRangeJpeg        = 2;   // full (0..255 / 0..1023)

}  // namespace ffmpeg_expect

// ---- FFmpeg 原始枚举值 -> 语义枚举（入参为 AVColorXXX 的数值）----
ColorPrimariesKind ClassifyPrimariesRaw(int av_primaries);
TransferKind ClassifyTransferRaw(int av_transfer);
MatrixKind ClassifyMatrixRaw(int av_matrix);
ColorRangeKind ClassifyRangeRaw(int av_range);

// ---- FFmpeg 原始枚举值 -> 展示名（"BT.709" / "PQ" / "HLG" / "Full"）----
std::string PrimariesDisplayName(int av_primaries);
std::string TransferDisplayName(int av_transfer);
std::string MatrixDisplayName(int av_matrix);
std::string RangeDisplayName(int av_range);

// 像素格式的描述（全部来自 AVPixFmtDescriptor，由 ColorHdrAnalyzer 填充）
struct PixelFormatInfo {
    std::string name;                 // 如 "yuv420p10le"
    bool valid = false;               // 是否成功拿到 descriptor
    bool rgb = false;                 // RGB / GBR 族（不存在色度下采样）
    int bit_depth = 0;                // 每分量位深（10 = 10bit），0 = 未知
    int bits_per_raw_sample = 0;      // 编码器标注的原始位深（AVCodecParameters::bits_per_raw_sample）
    std::string chroma_subsampling;   // "4:2:0" / "4:2:2" / "4:4:4"，RGB 为空

    bool Known() const { return valid; }
    // yuvjXXX（deprecated 的 full range 变体）：像素格式本身就隐含 full range
    bool ImpliesFullRange() const;
};

// 单条视频流的完整色彩描述
struct ColorInfo {
    bool analyzed = false;
    int stream_index = -1;

    std::string codec_name;    // h264 / hevc / av1 ...
    std::string profile_name;  // High / Main 10 / Main10 ...
    int level = 0;             // AVCodecParameters::level，-1/0 表示未标注
    int width = 0;
    int height = 0;

    // ---- 原始取值（FFmpeg 枚举数值）----
    int primaries_raw = 0;
    int transfer_raw = 0;
    int matrix_raw = 0;
    int range_raw = 0;

    // ---- 语义取值（规则判定与 UI 展示都基于这一组）----
    ColorPrimariesKind primaries = ColorPrimariesKind::Unspecified;
    TransferKind transfer = TransferKind::Unspecified;
    MatrixKind matrix = MatrixKind::Unspecified;
    ColorRangeKind range = ColorRangeKind::Unspecified;

    // ---- 展示名 ----
    std::string primaries_name;
    std::string transfer_name;
    std::string matrix_name;
    std::string range_name;

    PixelFormatInfo pixel_format;

    // 容器/容器编码extradata 之外是否还从 AVFrame side data 补过信息
    bool frame_side_data_checked = false;

    bool PrimariesSpecified() const { return primaries != ColorPrimariesKind::Unspecified; }
    bool TransferSpecified() const { return transfer != TransferKind::Unspecified; }
    bool MatrixSpecified() const { return matrix != MatrixKind::Unspecified; }
    bool RangeSpecified() const { return range != ColorRangeKind::Unspecified; }

    bool IsPqTransfer() const { return transfer == TransferKind::Pq; }
    bool IsHdrTransfer() const {
        return transfer == TransferKind::Pq || transfer == TransferKind::Hlg;
    }
    bool IsWideGamutPrimaries() const {
        return primaries == ColorPrimariesKind::Bt2020 ||
               primaries == ColorPrimariesKind::DisplayP3 ||
               primaries == ColorPrimariesKind::DciP3 ||
               primaries == ColorPrimariesKind::Smpte428;
    }
    bool IsFullRange() const { return range == ColorRangeKind::Full; }
    // 位深：优先取像素格式的计算结果，其次取编码器标注
    int EffectiveBitDepth() const;

    // 一行摘要："BT.2020 / PQ / BT.2020 NCL / Limited / 10bit / 4:2:0"
    std::string ToString() const;
};

}  // namespace model
}  // namespace videoeye
