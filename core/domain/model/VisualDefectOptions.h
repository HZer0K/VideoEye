#pragma once

// 视觉缺陷检测的参数（采样档位 + 各缺陷阈值）。
//
// 原先与 VisualDefectAnalyzer 实现放在同一头文件；作为纯值对象下放到 domain/model，
// 与 StreamStats / SceneChangeResult / ColorHdrAnalysis 一致的"类型下放"策略——
// UI 与报告层只依赖 domain，不再直接 include 分析器实现头。

#include <string>

#include "core/domain/model/QualityMetric.h"
#include "core/domain/model/VisualDefect.h"

namespace videoeye {
namespace model {

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
    double EffectiveSampleFps() const {
        if (sample_fps > 0.0) return sample_fps;
        switch (preset) {
            case VisualSamplingPreset::Fast:        return 1.0;
            case VisualSamplingPreset::Standard:    return 2.0;
            case VisualSamplingPreset::Fine:        return 5.0;
            case VisualSamplingPreset::OfflineFull: return 0.0;   // 每帧
        }
        return 2.0;
    }
    // 分析用的灰度宽度（preset 推导后的实际值）
    int EffectiveAnalysisWidth() const {
        if (analysis_width > 0) return analysis_width;
        switch (preset) {
            case VisualSamplingPreset::Fast:        return 160;
            case VisualSamplingPreset::Standard:    return 256;
            case VisualSamplingPreset::Fine:        return 384;
            case VisualSamplingPreset::OfflineFull: return 256;
        }
        return 256;
    }
    // 证据缩略图宽度（灰度图的一半，够看清问题又不占内存）
    int EffectiveEvidenceWidth() const {
        const int w = EffectiveAnalysisWidth() / 2;
        return w < 32 ? 32 : w;
    }
};
} // namespace model
} // namespace videoeye
