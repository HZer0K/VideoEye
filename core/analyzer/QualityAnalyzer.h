#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace videoeye {
namespace analyzer {

// 单帧参考质量评估结果，当前实现基于灰度/亮度平面计算全帧 MSE、PSNR、SSIM、VSSIM
struct QualityMetrics {
    int frame_index = 0;
    int64_t reference_pts = AV_NOPTS_VALUE;
    int64_t distorted_pts = AV_NOPTS_VALUE;
    double timestamp_seconds = 0.0;
    int width = 0;
    int height = 0;
    double mse = 0.0;
    double psnr_nb = 0.0;
    double ssim = 0.0;
    bool valid = false;
    std::string error_message;
};

class QualityAnalyzer {
public:
    // 比较同一时间位置的参考帧与待测帧，支持ffmpeg可转换GRAY8的常见像素格式
    static QualityMetrics CompareFrames(const AVFrame* reference, 
                                        const AVFrame* distorted, 
                                        int frameIndex = 0, 
                                        double timestampSeconds = 0.0);
    
    static double CalculatePsnr(double mse, double maxPixelValue = 255.0);

private:
    struct GrayPlane {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };
    
    static bool ExtractGrayPlane(const AVFrame* frame, GrayPlane& grayPlane, std::string& errorMessage);
    static double CalculateMse(const GrayPlane& reference, const GrayPlane& distorted);
    static double CalculateSsim(const GrayPlane& reference, const GrayPlane& distorted);

};

} // namespace analyzer
} // namespace videoeye