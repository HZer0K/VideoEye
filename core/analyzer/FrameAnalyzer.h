#pragma once

#include <vector>
#include <string>
#include <memory>

#include <opencv2/opencv.hpp>

#include "core/model/FrameData.h"

namespace videoeye {
namespace analyzer {

// 直方图数据
struct HistogramData {
    std::vector<float> red_channel;
    std::vector<float> green_channel;
    std::vector<float> blue_channel;
    std::vector<float> gray_channel;
    int bins = 256;
};

// 边缘检测结果
struct EdgeResult {
    cv::Mat edge_map;
    int edge_pixel_count;
    double edge_ratio;
};

// 轮廓检测结果
struct ContourResult {
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    int contour_count;
    cv::Rect bounding_box;
};

// 2D DFT 结果
struct DftResult {
    cv::Mat magnitude_spectrum;
    cv::Mat phase_spectrum;
    double energy;
};

// 帧分析器类
class FrameAnalyzer {
public:
    FrameAnalyzer();
    ~FrameAnalyzer();
    
    // 直方图分析
    HistogramData ComputeHistogram(const model::FrameData& frame);
    HistogramData ComputeHistogram(const cv::Mat& image);
    
    // 边缘检测 (Canny)
    EdgeResult DetectEdges(const model::FrameData& frame, 
                          double threshold1 = 100, 
                          double threshold2 = 200);
    EdgeResult DetectEdges(const cv::Mat& image,
                          double threshold1 = 100,
                          double threshold2 = 200);
    
    // 轮廓提取
    ContourResult FindContours(const model::FrameData& frame);
    ContourResult FindContours(const cv::Mat& image);
    
    // 2D DFT 变换
    DftResult Compute2DDft(const model::FrameData& frame);
    DftResult Compute2DDft(const cv::Mat& image);
    
    // 格式转换工具
    static cv::Mat FrameDataToMat(const model::FrameData& frame);
    static cv::Mat YuvToRgb(const model::FrameData& frame);
    
    // 图像预处理
    static cv::Mat NormalizeImage(const cv::Mat& image);
    static cv::Mat ResizeImage(const cv::Mat& image, int width, int height);
    
private:
    // 计算直方图通道
    std::vector<float> CalcChannelHistogram(const cv::Mat& channel, int bins = 256);
    
    // 计算DFT幅度谱
    cv::Mat GetMagnitudeSpectrum(const cv::Mat& complex_img);
};

} // namespace analyzer
} // namespace videoeye
