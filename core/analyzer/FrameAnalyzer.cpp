#include "FrameAnalyzer.h"
#include "utils/Logger.h"
#include <numeric>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace videoeye {
namespace analyzer {

FrameAnalyzer::FrameAnalyzer() {
    LOG_INFO("帧分析器已初始化");
}

FrameAnalyzer::~FrameAnalyzer() {
}

HistogramData FrameAnalyzer::ComputeHistogram(const model::FrameData& frame) {
    cv::Mat rgb = YuvToRgb(frame);
    return ComputeHistogram(rgb);
}

HistogramData FrameAnalyzer::ComputeHistogram(const cv::Mat& image) {
    HistogramData hist_data;
    
    if (image.empty()) {
        LOG_WARN("输入图像为空");
        return hist_data;
    }
    
    cv::Mat gray, bgr;
    if (image.channels() == 1) {
        gray = image;
        hist_data.gray_channel = CalcChannelHistogram(gray, 256);
    } else {
        bgr = image;
        std::vector<cv::Mat> channels;
        cv::split(bgr, channels);
        
        if (channels.size() >= 3) {
            hist_data.blue_channel = CalcChannelHistogram(channels[0], 256);
            hist_data.green_channel = CalcChannelHistogram(channels[1], 256);
            hist_data.red_channel = CalcChannelHistogram(channels[2], 256);
            
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            hist_data.gray_channel = CalcChannelHistogram(gray, 256);
        }
    }
    
    return hist_data;
}

EdgeResult FrameAnalyzer::DetectEdges(const model::FrameData& frame, double threshold1, double threshold2) {
    cv::Mat rgb = YuvToRgb(frame);
    return DetectEdges(rgb, threshold1, threshold2);
}

EdgeResult FrameAnalyzer::DetectEdges(const cv::Mat& image, double threshold1, double threshold2) {
    EdgeResult result;
    
    if (image.empty()) {
        LOG_WARN("输入图像为空");
        return result;
    }
    
    cv::Mat gray;
    if (image.channels() == 1) {
        gray = image;
    } else {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    }
    
    // Canny 边缘检测
    cv::Canny(gray, result.edge_map, threshold1, threshold2);
    
    // 统计边缘像素
    result.edge_pixel_count = cv::countNonZero(result.edge_map);
    result.edge_ratio = (double)result.edge_pixel_count / (gray.rows * gray.cols);
    
    return result;
}

ContourResult FrameAnalyzer::FindContours(const model::FrameData& frame) {
    cv::Mat rgb = YuvToRgb(frame);
    return FindContours(rgb);
}

ContourResult FrameAnalyzer::FindContours(const cv::Mat& image) {
    ContourResult result;
    
    if (image.empty()) {
        LOG_WARN("输入图像为空");
        return result;
    }
    
    cv::Mat gray, edge;
    if (image.channels() == 1) {
        gray = image;
    } else {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    }
    
    // 边缘检测
    cv::Canny(gray, edge, 100, 200);
    
    // 查找轮廓
    cv::findContours(edge, result.contours, result.hierarchy, 
                    cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    
    result.contour_count = result.contours.size();
    
    // 计算边界框
    if (!result.contours.empty()) {
        result.bounding_box = cv::boundingRect(result.contours[0]);
        for (const auto& contour : result.contours) {
            cv::Rect rect = cv::boundingRect(contour);
            result.bounding_box = result.bounding_box | rect;
        }
    }
    
    return result;
}

DftResult FrameAnalyzer::Compute2DDft(const model::FrameData& frame) {
    cv::Mat rgb = YuvToRgb(frame);
    return Compute2DDft(rgb);
}

DftResult FrameAnalyzer::Compute2DDft(const cv::Mat& image) {
    DftResult result;
    
    if (image.empty()) {
        LOG_WARN("输入图像为空");
        return result;
    }
    
    cv::Mat gray;
    if (image.channels() == 1) {
        gray = image;
    } else {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    }
    
    // 转换为 float 类型
    cv::Mat padded;
    int m = cv::getOptimalDFTSize(gray.rows);
    int n = cv::getOptimalDFTSize(gray.cols);
    cv::copyMakeBorder(gray, padded, 0, m - gray.rows, 0, n - gray.cols, 
                      cv::BORDER_CONSTANT, cv::Scalar::all(0));
    
    cv::Mat planes[] = {cv::Mat_<float>(padded), cv::Mat::zeros(padded.size(), CV_32F)};
    cv::Mat complex_img;
    cv::merge(planes, 2, complex_img);
    
    // 执行 DFT
    cv::dft(complex_img, complex_img);
    
    // 计算幅度谱和相位谱
    cv::split(complex_img, planes);
    result.magnitude_spectrum = GetMagnitudeSpectrum(complex_img);
    cv::phase(planes[0], planes[1], result.phase_spectrum);
    
    // 计算能量
    result.energy = cv::sum(result.magnitude_spectrum)[0];
    
    return result;
}

cv::Mat FrameAnalyzer::FrameDataToMat(const model::FrameData& frame) {
    if (frame.width == 0 || frame.height == 0) {
        return cv::Mat();
    }
    
    return YuvToRgb(frame);
}

cv::Mat FrameAnalyzer::YuvToRgb(const model::FrameData& frame) {
    if (frame.width <= 0 || frame.height <= 0 || !frame.data[0] || !frame.data[1] || !frame.data[2]) {
        return cv::Mat();
    }
    if (frame.linesize[0] <= 0 || frame.linesize[1] <= 0 || frame.linesize[2] <= 0) {
        return cv::Mat();
    }
    if ((frame.width % 2) != 0 || (frame.height % 2) != 0) {
        return cv::Mat();
    }

    const auto pix_fmt = static_cast<AVPixelFormat>(frame.format);
    if (pix_fmt != AV_PIX_FMT_YUV420P && pix_fmt != AV_PIX_FMT_YUVJ420P) {
        return cv::Mat();
    }

    const int w = frame.width;
    const int h = frame.height;
    const int y_size = w * h;
    const int uv_w = w / 2;
    const int uv_h = h / 2;
    const int uv_size = uv_w * uv_h;

    std::vector<uchar> yuv_data(y_size + 2 * uv_size);
    uchar* dst_y = yuv_data.data();
    uchar* dst_u = dst_y + y_size;
    uchar* dst_v = dst_u + uv_size;

    for (int y = 0; y < h; ++y) {
        std::memcpy(dst_y + y * w, frame.data[0] + y * frame.linesize[0], w);
    }
    for (int y = 0; y < uv_h; ++y) {
        std::memcpy(dst_u + y * uv_w, frame.data[1] + y * frame.linesize[1], uv_w);
        std::memcpy(dst_v + y * uv_w, frame.data[2] + y * frame.linesize[2], uv_w);
    }

    cv::Mat yuv_frame(h * 3 / 2, w, CV_8UC1, yuv_data.data());
    cv::Mat rgb_frame;
    cv::cvtColor(yuv_frame, rgb_frame, cv::COLOR_YUV2BGR_I420);
    return rgb_frame;
}

cv::Mat FrameAnalyzer::NormalizeImage(const cv::Mat& image) {
    cv::Mat normalized;
    cv::normalize(image, normalized, 0, 255, cv::NORM_MINMAX);
    return normalized;
}

cv::Mat FrameAnalyzer::ResizeImage(const cv::Mat& image, int width, int height) {
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(width, height));
    return resized;
}

std::vector<float> FrameAnalyzer::CalcChannelHistogram(const cv::Mat& channel, int bins) {
    std::vector<float> histogram;
    
    if (channel.empty()) {
        return histogram;
    }
    
    cv::Mat hist;
    int hist_size = bins;
    float range[] = {0, 256};
    const float* hist_range = {range};
    
    cv::calcHist(&channel, 1, 0, cv::Mat(), hist, 1, &hist_size, &hist_range);
    
    histogram.resize(hist_size);
    for (int i = 0; i < hist_size; i++) {
        histogram[i] = hist.at<float>(i);
    }
    
    // 归一化
    float max_val = *std::max_element(histogram.begin(), histogram.end());
    if (max_val > 0) {
        for (auto& val : histogram) {
            val /= max_val;
        }
    }
    
    return histogram;
}

cv::Mat FrameAnalyzer::GetMagnitudeSpectrum(const cv::Mat& complex_img) {
    cv::Mat planes[2];
    cv::split(complex_img, planes);
    
    cv::Mat magnitude;
    cv::magnitude(planes[0], planes[1], magnitude);
    
    // 转换到对数尺度
    magnitude += cv::Scalar::all(1);
    cv::log(magnitude, magnitude);
    
    // 裁剪并重新排列象限
    magnitude = magnitude(cv::Rect(0, 0, magnitude.cols & -2, magnitude.rows & -2));
    
    int cx = magnitude.cols / 2;
    int cy = magnitude.rows / 2;
    
    cv::Mat q0(magnitude, cv::Rect(0, 0, cx, cy));
    cv::Mat q1(magnitude, cv::Rect(cx, 0, cx, cy));
    cv::Mat q2(magnitude, cv::Rect(0, cy, cx, cy));
    cv::Mat q3(magnitude, cv::Rect(cx, cy, cx, cy));
    
    cv::Mat tmp;
    q0.copyTo(tmp);
    q3.copyTo(q0);
    tmp.copyTo(q3);
    
    q1.copyTo(tmp);
    q2.copyTo(q1);
    tmp.copyTo(q2);
    
    // 归一化到 0-255
    cv::normalize(magnitude, magnitude, 0, 255, cv::NORM_MINMAX, CV_8UC1);
    
    return magnitude;
}

} // namespace analyzer
} // namespace videoeye
