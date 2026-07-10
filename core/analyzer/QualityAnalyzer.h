#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>

#include <opencv2/opencv.hpp>

namespace videoeye {
namespace analyzer {

// 单帧质量评估结果
struct QualityFrameResult {
    int frame_index = 0;    // 帧序号 (从 0 开始, 以主视频为准)
    double timestamp = 0.0; // 主视频时间戳 (秒)
    double psnr = 0.0;      // PSNR (dB), 越高越好
    double ssim = 0.0;      // SSIM (0..1), 越高越好
};

// 质量评估汇总
struct QualitySummary {
    int compared_frames = 0;
    double mean_psnr = 0.0;
    double min_psnr = 0.0;
    int min_psnr_frame = -1;
    double mean_ssim = 0.0;
    double min_ssim = 0.0;
    int min_ssim_frame = -1;
    bool ok = false;
    std::string error;
};

// 离线批量质量评估器: 解码"主视频"与"参考视频", 逐帧对齐 (按帧序号) 后
// 计算逐帧 PSNR 与 SSIM。基于亮度 (luma) 通道计算, 符合视频质量评估惯例。
//
// 设计为自包含: 内部使用独立的 FFmpeg 软件解码, 不依赖实时播放链路,
// 可在后台线程中运行, 通过 cancel_ 标志随时中止。
class QualityAnalyzer {
public:
    using ProgressCallback = std::function<void(int current, int total)>;
    using ResultCallback = std::function<void(const QualityFrameResult&)>;
    using SummaryCallback = std::function<void(const QualitySummary&)>;

    // 是否比较前将参考视频缩放到主视频分辨率 (默认 true)。
    // 当主/参考分辨率不一致时必须为 true; 一致时开启也无副作用。
    bool align_resolution = true;

    // 终止标志: 可在另一线程置 true 以中断评估。
    std::atomic<bool> cancel_{false};

    // 运行评估。on_progress 的 total 在无法预估时为 -1。
    // 任一阶段失败会在 summary.ok=false 且 summary.error 中说明, 不会抛异常。
    QualitySummary Run(const std::string& main_path,
                       const std::string& ref_path,
                       const ProgressCallback& on_progress,
                       const ResultCallback& on_frame,
                       const SummaryCallback& on_summary);

private:
    // 单路视频流: 软件解码, 可逐帧拉取 BGRA cv::Mat。
    struct FrameSource {
        // 返回 false 并填充 err 表示打开失败; 成功后需调用 Close()。
        bool Open(const std::string& path, std::string& err);
        // 拉取下一帧 (BGRA cv::Mat) 与时间戳 (秒)。返回 false 表示已结束或出错。
        bool ReadNext(cv::Mat& out, double& ts);
        void Close();

        void* fmt_ctx = nullptr;   // AVFormatContext*
        void* codec_ctx = nullptr; // AVCodecContext*
        void* sws_ctx = nullptr;   // SwsContext*
        void* pkt = nullptr;       // AVPacket*
        void* frame = nullptr;     // AVFrame*
        int video_stream_idx = -1;
        int width = 0;
        int height = 0;
        double time_base_den = 0.0;
        double time_base_num = 0.0;
        bool draining_ = false;
        bool finished_ = false;
    };
};

} // namespace analyzer
} // namespace videoeye
