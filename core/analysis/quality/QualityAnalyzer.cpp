#include "core/analysis/quality/QualityAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace videoeye {

QualityMetrics QualityAnalyzer::CompareFrames(const AVFrame* reference, const AVFrame* distorted, int frame_index,
                                              double timestamp_seconds) {

    QualityMetrics metrics;
    metrics.frame_index = frame_index;
    metrics.timestamp_seconds = timestamp_seconds;
    if (reference)
        metrics.reference_pts = reference->pts;
    if (distorted)
        metrics.distorted_pts = distorted->pts;

    GrayPlane reference_gray;
    GrayPlane distorted_gray;
    std::string error;
    if (!ExtractGrayPlane(reference, reference_gray, error)) {
        metrics.error_message = error;
        return metrics;
    }
    if (!ExtractGrayPlane(distorted, distorted_gray, error)) {
        metrics.error_message = error;
        return metrics;
    }
    if (reference_gray.width != distorted_gray.width || reference_gray.height != distorted_gray.height) {
        metrics.error_message = "Gray planes have different dimensions";
        return metrics;
    }
    // 不变式: 平面像素数由 w*h 决定，尺寸一致时必然相等。校验一次是为了让
    // 下面两个指标函数不必再处理错配 —— 它们的 NaN 返回永远不会流到这里之外。
    if (reference_gray.pixels.size() != distorted_gray.pixels.size() || reference_gray.pixels.empty()) {
        metrics.error_message = "Gray planes have inconsistent pixel counts";
        return metrics;
    }

    metrics.width = reference_gray.width;
    metrics.height = reference_gray.height;
    metrics.mse = CalculateMse(reference_gray, distorted_gray);
    metrics.psnr_nb = CalculatePsnr(metrics.mse);
    metrics.ssim = CalculateSsim(reference_gray, distorted_gray);
    metrics.valid = true;
    return metrics;
}

QualityMetrics QualityAnalyzer::CompareSamples(const model::FrameSample& reference,
                                               const model::FrameSample& distorted) {
    QualityMetrics metrics;
    metrics.frame_index = distorted.frame_index;
    metrics.timestamp_seconds = distorted.timestamp_seconds;

    GrayPlane reference_gray;
    GrayPlane distorted_gray;
    std::string error;
    if (!MakeGrayPlane(reference, reference_gray, error)) {
        metrics.error_message = error;
        return metrics;
    }
    if (!MakeGrayPlane(distorted, distorted_gray, error)) {
        metrics.error_message = error;
        return metrics;
    }
    if (reference_gray.width != distorted_gray.width || reference_gray.height != distorted_gray.height ||
        reference_gray.pixels.empty() || reference_gray.pixels.size() != distorted_gray.pixels.size()) {
        metrics.error_message = "Frame samples have different dimensions";
        return metrics;
    }

    metrics.width = reference_gray.width;
    metrics.height = reference_gray.height;
    metrics.mse = CalculateMse(reference_gray, distorted_gray);
    metrics.psnr_nb = CalculatePsnr(metrics.mse);
    metrics.ssim = CalculateSsim(reference_gray, distorted_gray);
    // VMAF 未接入时 ComputeVmaf 返回 nullopt，vmaf 保持空 —— 报告与曲线
    // 以 has_value() 判断可用性，不再有 NaN 静默落库
    metrics.vmaf = ComputeVmaf(reference, distorted);
    metrics.valid = true;
    return metrics;
}

double QualityAnalyzer::CalculatePsnr(double mse, double maxPixelValue) {
    if (mse <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10((maxPixelValue * maxPixelValue) / mse);
}

std::optional<double> QualityAnalyzer::ComputeVmaf(const model::FrameSample& reference,
                                                   const model::FrameSample& distorted) {
    (void)reference;
    (void)distorted;
    // 默认构建不带 libvmaf（可选依赖，不进基础安装）。返回 nullopt 表示
    // "未启用"—— 调用方据此在 UI 显示"VMAF 未启用"、报告标记 available=false。
    // 接入 libvmaf 后替换本实现（模型文件缺失 / 版本不匹配同样返回 nullopt）。
    return std::nullopt;
}

bool QualityAnalyzer::VmafSupported() {
    return false;
}

namespace {

// 按原比例算缩放后的高度，并保证至少 2 行且为偶数（隔行/场分析要成对的行）
int ScaledHeight(int src_width, int src_height, int dst_width) {
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0)
        return 0;
    double h = std::round(static_cast<double>(dst_width) * src_height / src_width);
    int height = static_cast<int>(h);
    if (height < 2)
        height = 2;
    if ((height % 2) != 0)
        --height;
    if (height < 2)
        height = 2;
    return height;
}

// 让 sws 保持源帧自己的亮度范围（limited 16-235 / full 0-255）。
// 默认 GRAY8 输出会把 limited 展开成 full，导致合法峰值白 235 -> 255，
// 曝光检测会把白底/标题卡误当成削波。这里把 dstRange 配成 srcRange。
// srcRange 优先取帧上声明的 color_range；没声明时沿用 sws 自己的推断
// （YUV 默认 limited、YUVJ/RGB 默认 full），避免拍脑袋猜错范围。
void ApplyNativeLumaRange(SwsContext* sws, const AVFrame* frame, int* effective_src_range = nullptr) {
    if (!sws || !frame)
        return;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_RGB))
        return; // RGB 输入本身就是 full range，不需要再转换
    int* inv_table = nullptr;
    int* table = nullptr;
    int src_range = 0;
    int dst_range = 1;
    int brightness = 0;
    int contrast = 0;
    int saturation = 0;
    if (sws_getColorspaceDetails(sws, &inv_table, &src_range, &table, &dst_range, &brightness, &contrast, &saturation) <
        0) {
        return;
    }
    if (frame->color_range == AVCOL_RANGE_MPEG)
        src_range = 0;
    else if (frame->color_range == AVCOL_RANGE_JPEG)
        src_range = 1;
    // 未声明范围时 src_range 保留 sws 的推断结果
    sws_setColorspaceDetails(sws, inv_table, src_range, table, src_range, brightness, contrast, saturation);
    if (effective_src_range)
        *effective_src_range = src_range;
}

} // namespace

bool QualityAnalyzer::BuildSample(const AVFrame* frame, int gray_width, int rgb_width, bool capture_rgb,
                                  model::FrameSample& out, std::string& error, bool build_block_probe) {
    out = model::FrameSample();
    if (!frame) {
        error = "Frame is null";
        return false;
    }
    if (frame->width <= 0 || frame->height <= 0 || frame->format < 0 || !frame->data[0]) {
        error = "Frame dimensions are invalid";
        return false;
    }

    const int gw = gray_width > 16 ? gray_width : 16;
    const int gh = ScaledHeight(frame->width, frame->height, gw);
    if (gh <= 0) {
        error = "Invalid scaled height";
        return false;
    }
    std::vector<uint8_t> gray;
    bool limited_range = false;
    if (!ScaleToGray(frame, gw, gh, gray, error, &limited_range))
        return false;
    out.width = gw;
    out.height = gh;
    // 记下源尺寸：下游要把编码块这类"源像素量级"的判定换算到样本像素，
    // 报告/界面也要把采样坐标换算回源分辨率展示。
    out.source_width = frame->width;
    out.source_height = frame->height;
    // 以缩放时实际采用的范围为准（未声明 color_range 时由 sws 推断），
    // 否则会出现"样本值是 235 但阈值按 full range 卡"的口径错位。
    out.limited_range = limited_range;
    out.hdr = frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
    out.gray = std::move(gray);

    if (build_block_probe && !BuildBlockProbe(frame, out, error)) {
        // 块效应探针失败只影响这一项规则，不该把整帧分析废掉
        error.clear();
    }

    if (capture_rgb) {
        const int rw = rgb_width > 16 ? rgb_width : 16;
        const int rh = ScaledHeight(frame->width, frame->height, rw);
        std::vector<uint8_t> rgb;
        // RGB 失败只影响色偏与证据图，不该把整帧分析废掉
        if (rh > 0 && ScaleToRgb24(frame, rw, rh, rgb, error)) {
            out.rgb_width = rw;
            out.rgb_height = rh;
            out.rgb = std::move(rgb);
            error.clear();
        }
    }

    // PQ / HLG 帧在采样后做一次近似色调映射，把码值换算到 SDR 显示口径；
    // 曝光/色偏判定据此恢复（VisualDefectAnalyzer 按 tone_mapped 判断）。
    ApplyHdrToneMap(frame, out);
    return true;
}

bool QualityAnalyzer::ScaleToGray(const AVFrame* frame, int width, int height, std::vector<uint8_t>& out,
                                  std::string& error, bool* limited_range) {
    out.clear();
    if (limited_range)
        *limited_range = false;
    if (!frame || width <= 0 || height <= 0 || frame->format < 0 || !frame->data[0]) {
        error = "Frame dimensions are invalid";
        return false;
    }
    const auto src_fmt = static_cast<AVPixelFormat>(frame->format);
    if (!av_pix_fmt_desc_get(src_fmt)) {
        error = "Invalid pixel format";
        return false;
    }

    out.resize(static_cast<size_t>(width) * height);
    SwsContext* sws = sws_getContext(frame->width, frame->height, src_fmt, width, height, AV_PIX_FMT_GRAY8,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        error = "Failed to create sws context (gray)";
        out.clear();
        return false;
    }
    int src_range = -1;
    ApplyNativeLumaRange(sws, frame, &src_range);
    if (limited_range)
        *limited_range = (src_range == 0);
    uint8_t* dst_data[4] = {out.data(), nullptr, nullptr, nullptr};
    const int dst_linesize[4] = {width, 0, 0, 0};
    const int ret = sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);
    sws_freeContext(sws);
    if (ret < 0) {
        error = "Failed to scale gray plane";
        out.clear();
        return false;
    }
    return true;
}

bool QualityAnalyzer::BuildBlockProbe(const AVFrame* frame, model::FrameSample& out, std::string& error) {
    out.block_probes.clear();
    if (!frame || frame->width < 16 || frame->height < 16 || frame->format < 0 || !frame->data[0])
        return false;

    const auto src_fmt = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src_fmt);
    if (!desc)
        return false;

    // 单块上限 256x256: 够覆盖 8px 编码块的几十个周期，5 块合计约 320KB。
    // 裁剪边长与起点都对齐到 16: 8/16 两种编码块的边界相位都保持与源帧一致
    // （x % bs == 0 才是真实块边界）。
    constexpr int kMaxSize = 256;
    constexpr int kAlign = 16;
    const int crop_w = std::min(frame->width, kMaxSize) / kAlign * kAlign;
    const int crop_h = std::min(frame->height, kMaxSize) / kAlign * kAlign;
    if (crop_w < 16 || crop_h < 16)
        return false;

    const auto align_down = [](int v) { return v / kAlign * kAlign; };
    const int center_x = align_down((frame->width - crop_w) / 2);
    const int center_y = align_down((frame->height - crop_h) / 2);
    const int left = 0;
    const int top = 0;
    const int right = align_down(frame->width - crop_w);
    const int bottom = align_down(frame->height - crop_h);

    // 中心 + 四角。画面不够大（两倍裁剪边长都放不下）时只取中心 ——
    // 否则各块大面积重叠，5 次统计几乎是同一片像素，白费 CPU。
    std::vector<std::pair<int, int>> origins;
    origins.emplace_back(center_x, center_y);
    if (frame->width >= crop_w * 2 && frame->height >= crop_h * 2) {
        origins.emplace_back(left, top);
        origins.emplace_back(right, top);
        origins.emplace_back(left, bottom);
        origins.emplace_back(right, bottom);
    }

    for (const auto& origin : origins) {
        const int x0 = origin.first;
        const int y0 = origin.second;
        const uint8_t* src_data[4] = {nullptr, nullptr, nullptr, nullptr};
        int src_linesize[4] = {0, 0, 0, 0};
        for (int plane = 0; plane < 4; ++plane) {
            if (!frame->data[plane])
                continue;
            int comp = -1;
            for (int c = 0; c < desc->nb_components; ++c) {
                if (desc->comp[c].plane == plane) {
                    comp = c;
                    break;
                }
            }
            if (comp < 0)
                continue;
            const int shift_w = (plane == 1 || plane == 2) ? desc->log2_chroma_w : 0;
            const int shift_h = (plane == 1 || plane == 2) ? desc->log2_chroma_h : 0;
            const int step = desc->comp[comp].step > 0 ? desc->comp[comp].step : 1;
            src_data[plane] = frame->data[plane] + static_cast<ptrdiff_t>(y0 >> shift_h) * frame->linesize[plane] +
                              static_cast<ptrdiff_t>(x0 >> shift_w) * step;
            src_linesize[plane] = frame->linesize[plane];
        }

        std::vector<uint8_t> gray(static_cast<size_t>(crop_w) * crop_h);
        SwsContext* sws = sws_getContext(crop_w, crop_h, src_fmt, crop_w, crop_h, AV_PIX_FMT_GRAY8, SWS_POINT, nullptr,
                                         nullptr, nullptr);
        if (!sws) {
            error = "Failed to create sws context (block probe)";
            continue;
        }
        ApplyNativeLumaRange(sws, frame);
        uint8_t* dst_data[4] = {gray.data(), nullptr, nullptr, nullptr};
        const int dst_linesize[4] = {crop_w, 0, 0, 0};
        const int ret = sws_scale(sws, src_data, src_linesize, 0, crop_h, dst_data, dst_linesize);
        sws_freeContext(sws);
        if (ret < 0) {
            error = "Failed to scale block probe";
            continue;
        }
        model::FrameSample::BlockProbeRegion region;
        region.x = x0;
        region.y = y0;
        region.width = crop_w;
        region.height = crop_h;
        region.gray = std::move(gray);
        out.block_probes.push_back(std::move(region));
    }
    if (out.block_probes.empty()) {
        if (error.empty())
            error = "Failed to build block probe";
        return false;
    }
    return true;
}

namespace {

// ---- HDR 近似色调映射 ----
// PQ(SMPTE2084) / HLG(ARIB STD-B67) 的码值不是显示亮度，直接套 SDR 的曝光/色偏
// 阈值会把整片高动态范围素材判成过曝/欠曝。这里在采样后套一条静态曲线:
//   码值 -> 归一化 -> 逆传递函数得线性亮度 -> 以 203nit 为参考白做 Reinhard
//   -> BT.1886 gamma 编码回 8bit
// 这是"近似"而不是标准 HDR->SDR 转换（没有逐帧峰值/元数据，也没有色域映射），
// 目的是让阈值判定可用，不是产出可用于交付的 SDR 画面。
constexpr double kSdrReferenceWhiteNits = 203.0;
constexpr double kHlgNominalPeakNits = 1000.0;

// ST 2084 逆向 EOTF: 码值 -> 绝对亮度（cd/m^2，0..10000）
double PqToNits(double encoded) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;
    const double e = std::clamp(encoded, 0.0, 1.0);
    const double p = std::pow(e, 1.0 / m2);
    const double y = std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
    return 10000.0 * std::clamp(y, 0.0, 1.0);
}

// ARIB STD-B67 逆向 OETF: 码值 -> 场景线性（0..1）
double HlgToSceneLinear(double encoded) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    const double e = std::clamp(encoded, 0.0, 1.0);
    if (e <= 0.5)
        return e * e / 3.0;
    return (std::exp((e - c) / a) + b) / 12.0;
}

// 线性亮度 -> SDR 码值: 以参考白做 Reinhard 压缩，再按 BT.1886 (gamma 2.4) 编码
double ToneMapNitsToCode(double nits) {
    const double x = std::max(0.0, nits) / kSdrReferenceWhiteNits;
    const double mapped = x / (1.0 + x);
    return std::clamp(std::pow(mapped, 1.0 / 2.4) * 255.0, 0.0, 255.0);
}

// 256 项查找表，按源帧实际采用的亮度范围归一化（limited 16-235 / full 0-255）
std::array<uint8_t, 256> BuildToneMapLut(AVColorTransferCharacteristic trc, bool limited_range) {
    std::array<uint8_t, 256> lut{};
    const double lo = limited_range ? 16.0 : 0.0;
    const double span = limited_range ? 219.0 : 255.0;
    for (int v = 0; v < 256; ++v) {
        const double e = std::clamp((static_cast<double>(v) - lo) / span, 0.0, 1.0);
        double nits = 0.0;
        if (trc == AVCOL_TRC_SMPTE2084) {
            nits = PqToNits(e);
        } else {
            // HLG 的 OOTF 系统 gamma 取 1.2（BT.2100 对 1000nit 参考显示器的取值）
            nits = kHlgNominalPeakNits * std::pow(HlgToSceneLinear(e), 1.2);
        }
        lut[static_cast<size_t>(v)] = static_cast<uint8_t>(std::lround(ToneMapNitsToCode(nits)));
    }
    return lut;
}

} // namespace

void QualityAnalyzer::ApplyHdrToneMap(const AVFrame* frame, model::FrameSample& sample) {
    if (!frame)
        return;
    const AVColorTransferCharacteristic trc = frame->color_trc;
    if (trc != AVCOL_TRC_SMPTE2084 && trc != AVCOL_TRC_ARIB_STD_B67)
        return;
    if (!sample.valid())
        return;

    // 灰度图与块效应探针走同一亮度范围口径（ApplyNativeLumaRange 配置的 srcRange），
    // 必须用同一张 LUT，否则两者的块边界判据会不一致。
    const auto gray_lut = BuildToneMapLut(trc, sample.limited_range);
    for (auto& v : sample.gray)
        v = gray_lut[v];
    for (auto& probe : sample.block_probes) {
        if (!probe.valid())
            continue;
        for (auto& v : probe.gray)
            v = gray_lut[v];
    }
    if (sample.has_rgb()) {
        // ScaleToRgb24 没配 dstRange，sws 输出 RGB24 时按 full range 展开
        const auto rgb_lut = BuildToneMapLut(trc, false);
        for (auto& v : sample.rgb)
            v = rgb_lut[v];
    }
    sample.tone_mapped = true;
}

bool QualityAnalyzer::ScaleToRgb24(const AVFrame* frame, int width, int height, std::vector<uint8_t>& out,
                                   std::string& error) {
    out.clear();
    if (!frame || width <= 0 || height <= 0 || frame->format < 0 || !frame->data[0]) {
        error = "Frame dimensions are invalid";
        return false;
    }
    const auto src_fmt = static_cast<AVPixelFormat>(frame->format);
    if (!av_pix_fmt_desc_get(src_fmt)) {
        error = "Invalid pixel format";
        return false;
    }

    out.resize(static_cast<size_t>(width) * height * 3);
    SwsContext* sws = sws_getContext(frame->width, frame->height, src_fmt, width, height, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        error = "Failed to create sws context (rgb24)";
        out.clear();
        return false;
    }
    uint8_t* dst_data[4] = {out.data(), nullptr, nullptr, nullptr};
    const int dst_linesize[4] = {width * 3, 0, 0, 0};
    const int ret = sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);
    sws_freeContext(sws);
    if (ret < 0) {
        error = "Failed to scale rgb plane";
        out.clear();
        return false;
    }
    return true;
}

bool QualityAnalyzer::MakeGrayPlane(const model::FrameSample& sample, GrayPlane& gray_plane, std::string& error) {
    gray_plane = GrayPlane();
    if (!sample.valid()) {
        error = "Frame sample is invalid";
        return false;
    }
    gray_plane.width = sample.width;
    gray_plane.height = sample.height;
    gray_plane.pixels = sample.gray;
    return true;
}

bool QualityAnalyzer::ExtractGrayPlane(const AVFrame* frame, GrayPlane& out, std::string& error) {
    out = GrayPlane();

    if (!frame) {
        error = "Frame is null";
        return false;
    }
    if (frame->width <= 0 || frame->height <= 0 || frame->format < 0 || !frame->data[0]) {
        error = "Frame dimensions are invalid";
        return false;
    }

    const auto src_fmt = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src_fmt);
    if (!desc) {
        error = "Invalid pixel format";
        return false;
    }

    out.width = frame->width;
    out.height = frame->height;
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);

    if (src_fmt == AV_PIX_FMT_GRAY8) {
        if (frame->linesize[0] < out.width) {
            error = "Invalid gray plane line size";
            return false;
        }
        for (int y = 0; y < out.height; y++) {
            const uint8_t* src = frame->data[0] + static_cast<int>(y) * frame->linesize[0];
            uint8_t* dst = out.pixels.data() + static_cast<size_t>(y) * out.width;
            std::copy(src, src + out.width, dst);
        }
        return true;
    }

    SwsContext* sws = sws_getContext(frame->width, frame->height, src_fmt, out.width, out.height, AV_PIX_FMT_GRAY8,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        error = "Failed to create sws context";
        return false;
    }

    uint8_t* dts_data[4] = {out.pixels.data(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {out.width, 0, 0, 0};
    const int ret = sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dts_data, dst_linesize);

    sws_freeContext(sws);
    if (ret < 0) {
        error = "Failed to scale gray plane";
        return false;
    }
    return true;
}

double QualityAnalyzer::CalculateMse(const GrayPlane& reference, const GrayPlane& distorted) {
    if (reference.pixels.empty() || reference.pixels.size() != distorted.pixels.size()) {
        // NaN 而不是 0.0: MSE=0 的含义是"两帧完全一致"，拿它当错误返回值会让
        // 调用方把失败读成完美匹配（PSNR 还会跟着变成 +inf）。
        return std::numeric_limits<double>::quiet_NaN();
    }

    long double sum = 0.0;
    for (size_t i = 0; i < reference.pixels.size(); ++i) {
        const int diff = static_cast<int>(reference.pixels[i]) - static_cast<int>(distorted.pixels[i]);
        sum += static_cast<long double>(diff) * diff;
    }
    return static_cast<double>(sum) / reference.pixels.size();
}

double QualityAnalyzer::CalculateSsim(const GrayPlane& reference, const GrayPlane& distorted, double maxPixelValue) {
    const size_t count = reference.pixels.size();
    if (count == 0 || count != distorted.pixels.size()) {
        // 同上: 这里的 0.0 会被读成"结构无关"，与真实失效无法区分
        return std::numeric_limits<double>::quiet_NaN();
    }

    long double sum_x = 0.0;
    long double sum_y = 0.0;
    long double sum_x2 = 0.0;
    long double sum_y2 = 0.0;
    long double sum_xy = 0.0;

    for (size_t i = 0; i < count; ++i) {
        const long double x = reference.pixels[i];
        const long double y = distorted.pixels[i];
        sum_x += x;
        sum_y += y;
        sum_x2 += x * x;
        sum_y2 += y * y;
        sum_xy += x * y;
    }

    const long double inv_count = 1.0 / count;
    const long double mu_x = inv_count * sum_x;
    const long double mu_y = inv_count * sum_y;
    const long double var_x = inv_count * sum_x2 - mu_x * mu_x;
    const long double var_y = inv_count * sum_y2 - mu_y * mu_y;
    const long double cov_xy = inv_count * sum_xy - mu_x * mu_y;

    // 稳定常数按 Wang 2004 的建议系数由动态范围推导，与 CalculatePsnr 的
    // maxPixelValue 口径一致 —— 换位深时两个指标会同步变化，不会一个对一个不对。
    constexpr long double k1 = 0.01L;
    constexpr long double k2 = 0.03L;
    const long double dynamic_range = static_cast<long double>(maxPixelValue);
    const long double c1 = (k1 * dynamic_range) * (k1 * dynamic_range);
    const long double c2 = (k2 * dynamic_range) * (k2 * dynamic_range);

    // 单尺度 SSIM（Wang 2004，alpha=beta=gamma=1 的简化式）:
    //   SSIM = (2*mu_x*mu_y + c1) * (2*cov_xy + c2)
    //          / ((mu_x^2 + mu_y^2 + c1) * (var_x + var_y + c2))
    //
    // 亮度项的分母是 mu_x^2 + mu_y^2（两项相加），不是 (mu_x^2+c1)*(mu_y^2+c1)
    // 相乘。写成相乘时两帧完全相同（mu_x=mu_y=100）会算出 3.4e-06 而不是 1.0，
    // 差了近三个数量级 —— 这类错误用肉眼比对视频质量是发现不了的。
    const long double numerator = (2.0L * mu_x * mu_y + c1) * (2.0L * cov_xy + c2);
    const long double denominator = (mu_x * mu_x + mu_y * mu_y + c1) * (var_x + var_y + c2);

    if (denominator <= 0.0L) {
        // 只在退化输入下发生（maxPixelValue<=0 之类），此时指标本身无意义
        return std::numeric_limits<double>::quiet_NaN();
    }

    // 返回真实值: 值域 [-1, 1]，整帧反相时是负数。不要在这里 clamp ——
    // 一旦截断，反相比 I 帧压缩失真这种"最坏情况"在图表上就和"结构无关"
    // 长得一模一样，排查时会被误导。
    return static_cast<double>(numerator / denominator);
}

} // namespace videoeye
