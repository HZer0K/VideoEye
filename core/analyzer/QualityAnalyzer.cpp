#include "QualityAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace videoeye {
namespace analyzer {

QualityMetrics QualityAnalyzer::CompareFrames(const AVFrame* reference, 
                                            const AVFrame* distorted, 
                                            int frame_index, 
                                            double timestamp_seconds) {
    
    QualityMetrics metrics;
    metrics.frame_index = frame_index;
    metrics.timestamp_seconds = timestamp_seconds;
    if(reference) metrics.reference_pts = reference->pts;
    if(distorted) metrics.distorted_pts = distorted->pts;

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
    if (reference_gray.width != distorted_gray.width ||
        reference_gray.height != distorted_gray.height) {
        metrics.error_message = "Gray planes have different dimensions";
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

double QualityAnalyzer::CalculatePsnr(double mse, double maxPixelValue) {
    if (mse <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10((maxPixelValue * maxPixelValue) / mse);
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

    SwsContext* sws = sws_getContext(frame->width, frame->height, src_fmt,
                                        out.width, out.height, AV_PIX_FMT_GRAY8,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        error = "Failed to create sws context";
        return false;
    }

    uint8_t* dts_data[4] = {out.pixels.data(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {out.width, 0, 0, 0};
    const int ret = sws_scale(sws, frame->data, frame->linesize, 0, 
                                frame->height, dts_data, dst_linesize);
    
    sws_freeContext(sws);
    if (ret < 0) {
        error = "Failed to scale gray plane";
        return false;
    }
    return true;
}

double QualityAnalyzer::CalculateMse(const GrayPlane& reference, const GrayPlane& distorted) {
    if (reference.pixels.empty() || reference.pixels.size() != distorted.pixels.size()) {
        return 0.0;
    }

    long double sum = 0.0;
    for (size_t i = 0; i < reference.pixels.size(); ++i) {
        const int diff = static_cast<int>(reference.pixels[i]) -
                         static_cast<int>(distorted.pixels[i]);
        sum += static_cast<long double>(diff) * diff;
    }
    return static_cast<double>(sum) / reference.pixels.size();
}

double QualityAnalyzer::CalculateSsim(const GrayPlane& reference, const GrayPlane& distorted) {
    const size_t count = reference.pixels.size();
    if (count == 0 || count != distorted.pixels.size()) {
        return 0.0;
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

    constexpr long double c1 = 6.5025L; // (0.01 * 255)^2
    constexpr long double c2 = 58.5225L; // (0.03 * 255)^2

    const long double numerator = (2.0L * mu_x * mu_y + c1) *
                                  (2.0L * cov_xy + c2);
    const long double denominator = (mu_x * mu_x + c1) * (mu_y * mu_y + c1) *
                                  (var_x + c2) * (var_y + c2);

    if (denominator <= 0.0L) {
        return 0.0;
    }

    const long double value = numerator / denominator;
    return static_cast<double>(std::clamp(value, 0.0L, 1.0L));
}

} // namespace analyzer
} // namespace videoeye
