#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "core/model/QualityMetric.h"

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
    // VMAF 需要外部 libvmaf（库 + 模型文件），默认构建不带，见 ComputeVmaf()。
    // 未接入时恒为 NaN。
    double vmaf = model::kQualityNoValue;
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

    // 同一套比较，只是输入换成已降采样好的画面快照（离线批量比对时省掉重复缩放）
    static QualityMetrics CompareSamples(const model::FrameSample& reference,
                                         const model::FrameSample& distorted);
    
    static double CalculatePsnr(double mse, double maxPixelValue = 255.0);

    // 把任意像素格式的 AVFrame 降采样成视觉分析用的快照（GRAY8 + 可选 RGB24 小图）。
    // gray_width / rgb_width 为目标宽度，高度按原比例推算（最小 2 行）。
    // capture_rgb=false 时 out.rgb 为空，省一次 sws_scale（只要不看色偏/证据图就够用）。
    static bool BuildSample(const AVFrame* frame, int gray_width, int rgb_width,
                            bool capture_rgb, model::FrameSample& out, std::string& error);

    // VMAF: 可选依赖。需要 libvmaf（体积大、模型文件另带），第一阶段不接入，
    // 这里保留接口与字段占位 —— 未接入时恒返回 NaN，调用方无需改代码。
    // 接入路径: 用 ffmpeg CLI 的 libvmaf filter 或链接 libvmaf，替换本函数实现即可。
    static double ComputeVmaf(const model::FrameSample& reference,
                              const model::FrameSample& distorted);
    static bool VmafSupported();

private:
    struct GrayPlane {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };
    
    static bool ExtractGrayPlane(const AVFrame* frame, GrayPlane& grayPlane, std::string& errorMessage);
    // 按目标尺寸缩放出 GRAY8 / RGB24 平面（BuildSample 用）
    static bool ScaleToGray(const AVFrame* frame, int width, int height,
                            std::vector<uint8_t>& out, std::string& error);
    static bool ScaleToRgb24(const AVFrame* frame, int width, int height,
                             std::vector<uint8_t>& out, std::string& error);
    static bool MakeGrayPlane(const model::FrameSample& sample, GrayPlane& grayPlane,
                              std::string& error);
    static double CalculateMse(const GrayPlane& reference, const GrayPlane& distorted);
    // maxPixelValue 与 CalculatePsnr 保持同一口径: 灰度平面是 8bit, 默认 255。
    // c1/c2 由它推导 (Wang 2004 建议 k1=0.01, k2=0.03), 不写死常量。
    static double CalculateSsim(const GrayPlane& reference,
                                const GrayPlane& distorted,
                                double maxPixelValue = 255.0);

};

} // namespace analyzer
} // namespace videoeye