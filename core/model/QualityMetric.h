#pragma once

// 画面质量 / 视觉缺陷分析的数据模型。
//
// 这里放两类东西:
//   1. FrameSample       —— 供分析使用的降采样帧快照（GRAY8 + 可选 RGB24 小图）
//   2. FrameQualityMetric —— 单帧的画面质量指标（亮度/黑场/模糊/帧差/色偏/梳齿/块效应/黑边 ...）
//
// 与 core/analyzer/QualityAnalyzer.h 里已有的 QualityMetrics（PSNR/SSIM，需要参考帧）
// 是互补关系: 那个是"有参考"的比较，这个是"无参考"的单帧体检。
//
// 取值约定（与 QualityMetrics 一致，不要再引入哨兵值）:
//   * 指标算不出来时写 NaN（kQualityNoValue），绝不用 0 顶替 ——
//     0 在绝大多数指标里都是合法且有意义的值（黑场比例 0 = 没有黑像素，帧差 0 = 完全静止）。
//   * 比例类指标值域 [0, 1]；亮度类值域 [0, 255]。

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace videoeye {
namespace model {

// 指标未计算 / 不可用时的占位值。
inline constexpr double kQualityNoValue = std::numeric_limits<double>::quiet_NaN();

inline bool QualityValueValid(double v) {
    return std::isfinite(v);
}

// 有效画面区域（去掉黑边之后的矩形）。
// 采样帧坐标系: 原点左上，单位是"采样图的像素"，换算回原图需按 sample_width/height 缩放。
struct ActivePictureArea {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool valid = false;

    bool letterbox = false;   // 上下黑边
    bool pillarbox = false;   // 左右黑边

    // 四条边各自占整帧宽/高的比例（0 表示没有黑边）
    double top_bar_ratio = 0.0;
    double bottom_bar_ratio = 0.0;
    double left_bar_ratio = 0.0;
    double right_bar_ratio = 0.0;

    double active_ratio = 1.0;   // 有效区面积 / 整帧面积

    bool HasBars() const { return letterbox || pillarbox; }
};

// 供视觉分析使用的降采样帧快照。
//
// 为什么要降采样: 播放线程里做全分辨率的拉普拉斯/边缘统计太贵，
// 而黑场、模糊、黑边这类判断在 256 宽的图上和在全分辨率图上结论一致（模糊反而更明显）。
// 证据缩略图（evidence）也直接取自这里的 rgb，省一次额外缩放。
struct FrameSample {
    int frame_index = -1;
    double timestamp_seconds = 0.0;

    int width = 0;
    int height = 0;
    std::vector<uint8_t> gray;      // width * height，GRAY8，无行对齐

    int rgb_width = 0;
    int rgb_height = 0;
    std::vector<uint8_t> rgb;       // RGB24，色偏检测与证据缩略图用；可为空

    // 该时刻音频是否静音。冻结帧判定要用: 静止画面 + 静音 = 正常的静帧（片尾/黑屏留白），
    // 静止画面 + 有声音 = 真正的冻结故障。没有音频流时填 false（无从判断，按"不静音"处理）。
    bool audio_silent = false;

    bool valid() const {
        return width > 0 && height > 0 &&
               gray.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    bool has_rgb() const {
        return rgb_width > 0 && rgb_height > 0 &&
               rgb.size() >= static_cast<size_t>(rgb_width) * static_cast<size_t>(rgb_height) * 3u;
    }
};

// 单帧画面质量指标。
struct FrameQualityMetric {
    int frame_index = -1;
    double timestamp_seconds = 0.0;
    int sample_width = 0;
    int sample_height = 0;

    // ---- 亮度 ----
    double luma_mean = kQualityNoValue;        // 全帧亮度均值 [0,255]
    double luma_std = kQualityNoValue;         // 亮度标准差（对比度参考）
    double black_ratio = kQualityNoValue;      // Y <= black_luma 的像素占比（黑场判定）
    double dark_ratio = kQualityNoValue;       // Y <= shadow_luma 的像素占比（欠曝判定）
    double highlight_ratio = kQualityNoValue;  // Y >= highlight_luma 的像素占比（过曝判定）
    double clip_high_ratio = kQualityNoValue;  // Y >= clip_high_luma，硬削波（高光死白）
    double clip_low_ratio = kQualityNoValue;   // Y <= clip_low_luma，暗部压死

    // ---- 清晰度 ----
    // 拉普拉斯方差 / 1000（见 VisualDefectAnalyzer 的说明）。越小越模糊。
    double blur_score = kQualityNoValue;

    // ---- 时域（与上一采样帧比较）----
    double frame_diff = kQualityNoValue;   // 平均绝对差 / 255，0 = 完全相同（冻结判定）
    double luma_delta = kQualityNoValue;   // 亮度均值变化的绝对值 [0,255]（闪烁判定）

    // ---- 色彩 ----
    double color_cast_score = kQualityNoValue;  // 色偏强度 0..1（R/B 偏离亮度的合成量 / 255）
    double color_cast_rb = kQualityNoValue;     // mean(R) - mean(B)，>0 偏红，<0 偏蓝

    // ---- 结构 ----
    double combing_score = kQualityNoValue;    // 隔行梳齿 0..1
    double blockiness_score = kQualityNoValue; // 马赛克 / 块效应 0..1

    // ---- 有效画面区域（letterbox / pillarbox）----
    ActivePictureArea active_area;

    // ---- 有参考指标（需要参考帧，见 QualityAnalyzer::CompareSamples）----
    double psnr_db = kQualityNoValue;
    double ssim = kQualityNoValue;
    double vmaf = kQualityNoValue;   // 需要外部 libvmaf，默认构建恒为 NaN

    bool valid = false;
    std::string error_message;
};

}  // namespace model
}  // namespace videoeye
