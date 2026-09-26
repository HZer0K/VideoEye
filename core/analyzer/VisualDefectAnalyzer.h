#pragma once

// 视觉缺陷检测器（黑场 / 冻结 / 花屏马赛克 / 模糊 / 闪烁 / 过曝欠曝 / 色偏 / 隔行梳齿 / 黑边）。
//
// 设计要点:
//   1. 本文件**不依赖 FFmpeg**（只吃 model::FrameSample 里的降采样像素），
//      这样算法可以脱离解码器直接单测，测试里造图就行 —— 见 tests/unit/test_visual_defect.cpp。
//      AVFrame -> FrameSample 的降采样在 QualityAnalyzer::BuildSample()。
//   2. 实时模式走**有上限的投递队列 + 工作线程**: Submit() 永不阻塞解码线程，
//      队列满了直接丢帧（丢帧数记在 report.dropped_frames，UI 会显示），
//      播放流畅优先于分析完整。离线模式用 Feed() 同步全帧分析，一帧不丢。
//   3. 缺陷按"时间段"输出而不是逐帧刷屏: 连续满足条件的采样帧合并成一段，
//      停止后（或 Flush）才落一条 VisualDefect。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/model/QualityMetric.h"
#include "core/model/VisualDefect.h"

namespace videoeye {
namespace analyzer {

// 采样档位（对应 UI 的"快速 / 标准 / 精细 / 离线全帧"）
enum class VisualSamplingPreset {
    Fast = 0,        // 1 fps，160 宽：播放时几乎无感
    Standard,        // 2 fps，256 宽：默认
    Fine,            // 5 fps，384 宽：细节更全，开销约 3 倍
    OfflineFull,     // 每帧都分析（256 宽）：离线 QC，不丢帧，播放会明显变慢
};

struct VisualDefectOptions {
    VisualSamplingPreset preset = VisualSamplingPreset::Standard;

    // 下面两项为 0 时按 preset 推导；显式填值可覆盖（供单测与高级用法）
    double sample_fps = 0.0;    // 每秒采样帧数，0 = 每帧（离线）
    int analysis_width = 0;     // 分析用的灰度图宽度，高度按原比例推

    // 各缺陷的检测开关（关掉可以省掉对应的计算，但多数指标是一次遍历顺带算的，省不了多少）
    bool detect_black = true;
    bool detect_freeze = true;
    bool detect_blockiness = true;
    bool detect_blur = true;
    bool detect_flicker = true;
    bool detect_exposure = true;
    bool detect_color_cast = true;
    bool detect_combing = true;
    bool detect_borders = true;

    // 是否采集 RGB 小图（色偏检测与证据缩略图需要，成本约 +20%）
    bool capture_rgb = true;

    // ---- 黑场 ----
    double black_luma = 24.0;         // Y <= 该值算"黑像素"（兼容 limited range 的 16）
    double black_ratio = 0.95;        // 黑像素占比 >= 该值
    double black_luma_mean = 24.0;    // 且全帧亮度均值 <= 该值
    double black_min_seconds = 0.3;

    // ---- 冻结帧 ----
    double freeze_diff = 0.01;        // 与上一采样帧的平均绝对差 / 255（约 2.5 个灰阶）
    double freeze_min_seconds = 1.0;

    // ---- 模糊 ----
    // blur_score = 拉普拉斯方差 / 1000。实测（256 宽采样）:
    // 清晰画面通常 > 5，明显模糊（高斯/均值模糊后）< 1。低于阈值判模糊。
    double blur_threshold = 1.0;
    // 画面本身得有内容才谈"糊": 纯色帧/黑场的拉普拉斯方差和亮度标准差都天然是 0，
    // 那是"没细节"不是"糊了"，交给黑场/欠曝去报。阈值取 0.5 而不是更大的值，
    // 是因为重度模糊本身也会把标准差压到很低，卡太严会把真模糊漏掉。
    double blur_min_luma_std = 0.5;
    double blur_min_seconds = 0.5;

    // ---- 闪烁 ----
    double flicker_delta = 8.0;       // 相邻采样帧亮度均值跳变 (>= 该值算一次亮暗跳变)
    int flicker_min_alternations = 3; // 窗口内亮->暗->亮交替次数
    double flicker_min_seconds = 0.5;

    // ---- 曝光 ----
    double highlight_luma = 240.0;      // 高光像素阈值
    double clip_high_luma = 250.0;      // 硬削波阈值
    double clip_low_luma = 5.0;         // 暗部压死阈值
    double shadow_luma = 40.0;          // 暗部像素阈值（欠曝统计用）
    double over_exposure_highlight_ratio = 0.20;  // 高光占比
    double over_exposure_clip_ratio = 0.08;       // 削波占比（两者满足其一即判过曝）
    double under_exposure_dark_ratio = 0.60;      // 暗部占比
    double under_exposure_luma_mean = 35.0;       // 或全帧均值过低
    double exposure_min_seconds = 0.3;

    // ---- 色偏 ----
    double color_cast_score = 0.08;     // 约 20 个灰阶的 R/B 偏离（再低会把暖色调风格误判成色偏）
    double color_cast_min_seconds = 1.0;

    // ---- 隔行梳齿 ----
    double combing_score = 0.30;        // 0..1，(跨场差 - 同场差) / (跨场差 + 同场差)
    double combing_motion_gate = 0.002; // 静止画面没有梳齿，需要一点运动才判定
    double combing_min_seconds = 0.5;

    // ---- 花屏 / 马赛克（块效应）----
    int block_size = 8;                 // 以采样图像素计的块边长
    double blockiness_score = 0.35;     // 块边界不连续度
    double blockiness_min_seconds = 0.3;

    // ---- 黑边 ----
    double border_luma = 24.0;          // 行/列均值低于该值算黑边
    double border_bar_ratio = 0.04;     // 上下（或左右）合计占比达到该值才算
    double border_min_seconds = 0.5;

    // ---- 通用 ----
    size_t max_defects = 2000;          // 缺陷条数上限（超长素材兜底）
    size_t max_samples = 200000;        // 采样指标条数上限（图表用，超了就丢最早的）

    // 相邻同类缺陷间隔小于该秒数时合并成一条（避免同一段问题被切成好几条）
    double MergeGapSeconds() const { return 0.5; }

    // 采样帧率（preset 推导后的实际值，0 表示每帧）
    double EffectiveSampleFps() const;
    // 分析用的灰度宽度（preset 推导后的实际值）
    int EffectiveAnalysisWidth() const;
    // 证据缩略图宽度（灰度图的一半，够看清问题又不占内存）
    int EffectiveEvidenceWidth() const;
};

class VisualDefectAnalyzer {
public:
    VisualDefectAnalyzer();
    ~VisualDefectAnalyzer();

    // 重新配置并清空已有结果。会先停掉工作线程（如果开着）。
    void Reset(const VisualDefectOptions& options);
    const VisualDefectOptions& options() const { return options_; }

    // ---- 离线 / 单测: 同步分析一帧，返回后指标与已结束的缺陷都已落库 ----
    void Feed(const model::FrameSample& sample);

    // ---- 实时: 投递一帧。队列满直接丢弃并返回 false，绝不阻塞调用线程 ----
    bool Submit(const model::FrameSample& sample);
    void StartWorker(size_t queue_capacity = 8);
    void StopWorker();
    bool worker_running() const { return worker_running_.load(); }

    // 取走工作线程产出但还没交给 UI 的结果（在调用线程执行）
    std::vector<model::FrameQualityMetric> TakePendingMetrics();
    std::vector<model::VisualDefect> TakePendingDefects();

    // 收尾: 素材结束 / seek / 关闭文件时调用，把所有还开着的缺陷段落闭合。
    // end_timestamp_seconds 作为最后一段的结束时间。
    void Flush(double end_timestamp_seconds);

    // ---- 结果 ----
    // 只在"分析已停止"时直接读（内部不再加锁）；跨线程请用 Snapshot()。
    const std::vector<model::FrameQualityMetric>& metrics() const { return metrics_; }
    const std::vector<model::VisualDefect>& defects() const { return defects_; }
    model::ActivePictureArea EffectiveArea() const;
    model::VisualDefectReport Snapshot() const;

    int dropped_samples() const { return dropped_samples_.load(); }
    int analyzed_samples() const { return analyzed_samples_.load(); }

    // ---- 纯算法（单测直接调用）----
    // previous 为上一采样帧（首帧为 nullptr），previous_luma_mean 为上一采样帧的亮度均值（首帧 NaN）。
    static model::FrameQualityMetric ComputeFrameMetrics(const model::FrameSample& sample,
                                                         const model::FrameSample* previous,
                                                         double previous_luma_mean,
                                                         const VisualDefectOptions& options);

private:
    struct SegmentState {
        bool active = false;
        double start_ts = 0.0;
        double last_ts = 0.0;
        int start_frame = -1;
        int last_frame = -1;
        double worst = 0.0;      // 触发指标的极值（越大越差 / 越小越差由 reverse 决定）
        double sum = 0.0;
        int count = 0;
        double aux = model::kQualityNoValue;       // 描述里要用的辅助数值（亮度均值/削波占比/...）
        double threshold = model::kQualityNoValue; // 当时的判定阈值
        bool has_evidence = false;
        model::EvidenceFrame evidence;
    };

    // 一次"某类型是否命中"的更新
    struct SegmentUpdate {
        model::VisualDefectType type = model::VisualDefectType::BlackFrame;
        bool hit = false;
        double ts = 0.0;
        int frame = -1;
        double score = model::kQualityNoValue;
        double aux = model::kQualityNoValue;
        double threshold = model::kQualityNoValue;
        bool reverse = false;              // true = 分数越小越严重（模糊、帧差）
        double start_ts = model::kQualityNoValue;  // 覆盖段首时间（闪烁用窗口起点）
    };

    // 在 m_ 已加锁的前提下处理一帧
    void ProcessSampleLocked(const model::FrameSample& sample);
    void UpdateSegmentLocked(const SegmentUpdate& update, const model::FrameSample& sample);
    void CloseSegmentLocked(model::VisualDefectType type, double end_ts, int end_frame);
    void CloseAllSegmentsLocked(double end_ts, int end_frame);
    bool FlickerHitLocked(double ts, double* amplitude) const;
    static model::VisualDefectSeverity SeverityFor(model::VisualDefectType type,
                                                   double duration_seconds,
                                                   double score,
                                                   const VisualDefectOptions& options);
    void WorkerLoop();
    void PushDefectLocked(model::VisualDefect defect);

    VisualDefectOptions options_;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> worker_running_{false};
    std::deque<model::FrameSample> queue_;
    size_t queue_capacity_ = 8;

    std::atomic<int> dropped_samples_{0};
    std::atomic<int> analyzed_samples_{0};

    // 时域状态（受 m_ 保护）
    bool has_prev_ = false;
    model::FrameSample prev_sample_;
    double prev_luma_mean_ = model::kQualityNoValue;
    std::deque<double> luma_history_;       // 最近的亮度均值，闪烁检测用
    std::deque<double> luma_history_ts_;
    std::map<model::VisualDefectType, SegmentState> segments_;

    std::vector<model::FrameQualityMetric> metrics_;
    std::vector<model::VisualDefect> defects_;
    std::vector<model::FrameQualityMetric> pending_metrics_;
    std::vector<model::VisualDefect> pending_defects_;
    std::vector<model::ActivePictureArea> active_areas_;
    int next_defect_id_ = 1;
};

}  // namespace analyzer
}  // namespace videoeye
