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

#include "core/domain/model/QualityMetric.h"
#include "core/domain/model/VisualDefect.h"
// 纯值对象已下放到 domain/model（与 StreamStats / SceneChangeResult 一致）。
// 放在 namespace 外、用顶层 include，避免"include 嵌在 namespace 内"导致 MSVC 的
// using 别名对 .cpp 定义不可见。
#include "core/domain/model/VisualDefectOptions.h"

namespace videoeye {
namespace analyzer {

// 采样档位（对应 UI 的"快速 / 标准 / 精细 / 离线全帧"）
// 向后兼容别名：player / ui / tests 里仍用 analyzer::VisualDefectOptions / VisualSamplingPreset。
using model::VisualDefectOptions;
using model::VisualSamplingPreset;

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
