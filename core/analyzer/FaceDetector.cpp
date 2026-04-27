#include "FaceDetector.h"
#include "utils/Logger.h"
#include <chrono>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace videoeye {
namespace analyzer {

FaceDetector::FaceDetector()
    : min_size_(30, 30)
    , max_size_(300, 300) {
}

FaceDetector::~FaceDetector() {
}

bool FaceDetector::InitializeHaarCascade(const std::string& cascade_path) {
    if (haar_cascade_.load(cascade_path)) {
        haar_initialized_ = true;
        LOG_INFO("Haar Cascade 检测器已初始化: " + cascade_path);
        return true;
    } else {
        LOG_ERROR("无法加载 Haar Cascade: " + cascade_path);
        return false;
    }
}

bool FaceDetector::InitializeDnnModel(const std::string& model_path, const std::string& config_path) {
    try {
        dnn_model_ = cv::dnn::readNetFromCaffe(config_path, model_path);
        if (!dnn_model_.empty()) {
            dnn_initialized_ = true;
            LOG_INFO("DNN 检测器已初始化");
            return true;
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("DNN 初始化失败: ") + e.what());
    }
    return false;
}

std::vector<FaceInfo> FaceDetector::DetectFaces(const model::FrameData& frame) {
    cv::Mat bgr = FrameDataToBgr(frame);
    return DetectFaces(bgr);
}

std::vector<FaceInfo> FaceDetector::DetectFaces(const cv::Mat& image) {
    auto start_time = std::chrono::steady_clock::now();
    
    std::vector<FaceInfo> faces;
    
    if (image.empty()) {
        LOG_WARN("输入图像为空");
        return faces;
    }
    
    switch (current_method_) {
        case DetectionMethod::HaarCascade:
            if (haar_initialized_) {
                cv::Mat gray;
                if (image.channels() == 1) {
                    gray = image;
                } else {
                    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
                }
                faces = DetectHaar(gray);
            } else {
                LOG_WARN("Haar Cascade 未初始化");
            }
            break;
            
        case DetectionMethod::Dnn:
            if (dnn_initialized_) {
                faces = DetectDnn(image);
            } else {
                LOG_WARN("DNN 模型未初始化");
            }
            break;
    }
    
    // 记录检测时间
    auto end_time = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    detection_times_.push_back(elapsed_ms);
    if (detection_times_.size() > 100) {
        detection_times_.erase(detection_times_.begin());
    }
    
    total_detections_ += faces.size();
    
    if (!faces.empty()) {
        LOG_DEBUG("检测到 " + std::to_string(faces.size()) + " 个人脸 (" + 
                  std::to_string((int)elapsed_ms) + " ms)");
    }
    
    return faces;
}

void FaceDetector::SetMinSize(int width, int height) {
    min_size_ = cv::Size(width, height);
}

void FaceDetector::SetMaxSize(int width, int height) {
    max_size_ = cv::Size(width, height);
}

void FaceDetector::SetScaleFactor(double factor) {
    scale_factor_ = factor;
}

void FaceDetector::SetMinNeighbors(int neighbors) {
    min_neighbors_ = neighbors;
}

void FaceDetector::SetMethod(DetectionMethod method) {
    current_method_ = method;
}

double FaceDetector::GetAvgDetectionTimeMs() const {
    if (detection_times_.empty()) {
        return 0.0;
    }
    double sum = 0;
    for (double t : detection_times_) {
        sum += t;
    }
    return sum / detection_times_.size();
}

std::vector<FaceInfo> FaceDetector::DetectHaar(const cv::Mat& gray) {
    std::vector<cv::Rect> faces_rect;
    
    haar_cascade_.detectMultiScale(
        gray,
        faces_rect,
        scale_factor_,
        min_neighbors_,
        0 | cv::CASCADE_SCALE_IMAGE,
        min_size_,
        max_size_
    );
    
    return PostProcess(faces_rect);
}

std::vector<FaceInfo> FaceDetector::DetectDnn(const cv::Mat& image) {
    std::vector<FaceInfo> faces;
    
    // 准备输入
    cv::Mat blob;
    cv::dnn::blobFromImage(image, blob, 1.0, cv::Size(300, 300),
                          cv::Scalar(104, 177, 123), false, false);
    
    dnn_model_.setInput(blob);
    cv::Mat detections = dnn_model_.forward();
    
    // detections shape: [1, 1, N, 7]
    int num_detections = detections.size[2];
    
    for (int i = 0; i < num_detections; i++) {
        // 使用 ptr 访问 4D 张量
        float* detection_data = detections.ptr<float>(0, 0, i);
        float confidence = detection_data[2];
        
        if (confidence > 0.5) { // 置信度阈值
            FaceInfo face;
            face.confidence = confidence;
            
            int x1 = static_cast<int>(detection_data[3] * image.cols);
            int y1 = static_cast<int>(detection_data[4] * image.rows);
            int x2 = static_cast<int>(detection_data[5] * image.cols);
            int y2 = static_cast<int>(detection_data[6] * image.rows);
            
            face.bounding_box = cv::Rect(x1, y1, x2 - x1, y2 - y1);
            
            faces.push_back(face);
        }
    }
    
    return faces;
}

std::vector<FaceInfo> FaceDetector::PostProcess(const std::vector<cv::Rect>& faces) {
    std::vector<FaceInfo> result;
    
    for (const auto& rect : faces) {
        FaceInfo face;
        face.bounding_box = rect;
        face.confidence = 1.0; // Haar Cascade 不提供置信度
        
        // 估算关键点位置 (简化版)
        face.left_eye = cv::Point(rect.x + rect.width * 0.3, rect.y + rect.height * 0.4);
        face.right_eye = cv::Point(rect.x + rect.width * 0.7, rect.y + rect.height * 0.4);
        face.nose = cv::Point(rect.x + rect.width * 0.5, rect.y + rect.height * 0.6);
        face.left_mouth = cv::Point(rect.x + rect.width * 0.35, rect.y + rect.height * 0.8);
        face.right_mouth = cv::Point(rect.x + rect.width * 0.65, rect.y + rect.height * 0.8);
        
        result.push_back(face);
    }
    
    return result;
}

cv::Mat FaceDetector::FrameDataToBgr(const model::FrameData& frame) {
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
    cv::Mat bgr_frame;
    cv::cvtColor(yuv_frame, bgr_frame, cv::COLOR_YUV2BGR_I420);
    return bgr_frame;
}

} // namespace analyzer
} // namespace videoeye
