#pragma once

#include <vector>
#include <string>
#include <opencv2/opencv.hpp>

#include "core/model/FrameData.h"

namespace videoeye {
namespace analyzer {

// 人脸检测结果
struct FaceInfo {
    cv::Rect bounding_box;
    float confidence;
    cv::Point left_eye;
    cv::Point right_eye;
    cv::Point nose;
    cv::Point left_mouth;
    cv::Point right_mouth;
};

// 人脸检测器类
class FaceDetector {
public:
    FaceDetector();
    ~FaceDetector();
    
    // 初始化 Haar Cascade 检测器
    bool InitializeHaarCascade(const std::string& cascade_path);
    
    // 初始化 DNN 检测器
    bool InitializeDnnModel(const std::string& model_path, const std::string& config_path);
    
    // 检测人脸 (FrameData)
    std::vector<FaceInfo> DetectFaces(const model::FrameData& frame);
    
    // 检测人脸 (cv::Mat)
    std::vector<FaceInfo> DetectFaces(const cv::Mat& image);
    
    // 设置检测参数
    void SetMinSize(int width, int height);
    void SetMaxSize(int width, int height);
    void SetScaleFactor(double factor);
    void SetMinNeighbors(int neighbors);
    
    // 选择检测方法
    enum class DetectionMethod {
        HaarCascade,
        Dnn
    };
    void SetMethod(DetectionMethod method);
    
    // 获取统计信息
    int GetTotalDetections() const { return total_detections_; }
    double GetAvgDetectionTimeMs() const;
    
    // 格式转换
    static cv::Mat FrameDataToBgr(const model::FrameData& frame);
    
private:
    // Haar Cascade 检测
    std::vector<FaceInfo> DetectHaar(const cv::Mat& gray);
    
    // DNN 检测
    std::vector<FaceInfo> DetectDnn(const cv::Mat& image);
    
    // 后处理
    std::vector<FaceInfo> PostProcess(const std::vector<cv::Rect>& faces);
    
    // 成员变量
    cv::CascadeClassifier haar_cascade_;
    cv::dnn::Net dnn_model_;
    
    DetectionMethod current_method_ = DetectionMethod::HaarCascade;
    
    cv::Size min_size_;
    cv::Size max_size_;
    double scale_factor_ = 1.1;
    int min_neighbors_ = 3;
    
    int total_detections_ = 0;
    std::vector<double> detection_times_;
    
    bool haar_initialized_ = false;
    bool dnn_initialized_ = false;
};

} // namespace analyzer
} // namespace videoeye
