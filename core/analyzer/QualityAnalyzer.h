#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace videoeye {
namespace analyzer {

// 单帧参考质量评估结果，基于灰度/亮度平面计算全帧 MSE、PSNR、SSIM。
// 取值约定（全类统一，不要再引入哨兵值）:
//   valid == false  -> mse/psnr_nb/ssim 一律无意义，读之前先看 valid 和 error_message
//   mse             -> [0, 255^2]
//   psnr_nb         -> (0, +inf]，无损帧为 +inf（10*log10(255^2/0) 的真值）
//   ssim            -> [-1, 1]，见下方字段说明
struct QualityMetrics {
    int frame_index = 0;
    int64_t reference_pts = AV_NOPTS_VALUE;
    int64_t distorted_pts = AV_NOPTS_VALUE;
    double timestamp_seconds = 0.0;
    int width = 0;
    int height = 0;
    double mse = 0.0;
    double psnr_nb = 0.0;
    // SSIM 取真实值，值域 [-1, 1]:
    //    1  = 两帧完全一致
    //    0  = 结构无关
    //   <0  = 负相关（例如整帧反相），这是合法结果，不要当成异常裁到 0
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
    // maxPixelValue 与 CalculatePsnr 保持同一口径: 灰度平面是 8bit, 默认 255。
    // c1/c2 由它推导 (Wang 2004 建议 k1=0.01, k2=0.03), 不写死常量。
    static double CalculateSsim(const GrayPlane& reference,
                                const GrayPlane& distorted,
                                double maxPixelValue = 255.0);

};

} // namespace analyzer
} // namespace videoeye