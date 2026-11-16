#pragma once

// 画面质量 / 视觉缺陷分析的数据模型。
//
// 这里放两类东西:
//   1. FrameSample       —— 供分析使用的降采样帧快照（GRAY8 + 可选 RGB24 小图）
//   2. FrameQualityMetric —— 单帧的画面质量指标（亮度/黑场/模糊/帧差/色偏/梳齿/块效应/黑边 ...）
//
// 与 core/analysis/quality/QualityAnalyzer.h 里已有的 QualityMetrics（PSNR/SSIM，需要参考帧）
// 是互补关系: 那个是"有参考"的比较，这个是"无参考"的单帧体检。
//
// 取值约定（与 QualityMetrics 一致，不要再引入哨兵值）:
//   * 指标算不出来时写 NaN（kQualityNoValue），绝不用 0 顶替 ——
//     0 在绝大多数指标里都是合法且有意义的值（黑场比例 0 = 没有黑像素，帧差 0 = 完全静止）。
//   * 比例类指标值域 [0, 1]；亮度类值域 [0, 255]。

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
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

    bool letterbox = false; // 上下黑边
    bool pillarbox = false; // 左右黑边

    // 四条边各自占整帧宽/高的比例（0 表示没有黑边）
    double top_bar_ratio = 0.0;
    double bottom_bar_ratio = 0.0;
    double left_bar_ratio = 0.0;
    double right_bar_ratio = 0.0;

    double active_ratio = 1.0; // 有效区面积 / 整帧面积

    // 采样图与源图尺寸，用于把上面的采样坐标换算回源分辨率展示 / 导出。
    // 为 0 时表示未知（手造样本），调用方按采样坐标处理。
    int sample_width = 0;
    int sample_height = 0;
    int source_width = 0;
    int source_height = 0;

    bool HasBars() const {
        return letterbox || pillarbox;
    }

    bool HasSourceSize() const {
        return sample_width > 0 && sample_height > 0 && source_width > 0 && source_height > 0;
    }

    int SourceX() const {
        return HasSourceSize() ? static_cast<int>(std::lround(static_cast<double>(x) * source_width / sample_width))
                               : x;
    }
    int SourceY() const {
        return HasSourceSize() ? static_cast<int>(std::lround(static_cast<double>(y) * source_height / sample_height))
                               : y;
    }
    int SourceWidth() const {
        return HasSourceSize() ? static_cast<int>(std::lround(static_cast<double>(width) * source_width / sample_width))
                               : width;
    }
    int SourceHeight() const {
        return HasSourceSize()
                   ? static_cast<int>(std::lround(static_cast<double>(height) * source_height / sample_height))
                   : height;
    }
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
    // 降采样前的源帧宽度（为 0 表示未知，例如单测里手造的样本）。
    // 视觉缺陷检测要用它把"源图像素"量级的参数（如编码块边长）换算到样本像素。
    int source_width = 0;
    int source_height = 0;
    std::vector<uint8_t> gray; // width * height，GRAY8，无行对齐

    // 源分辨率 1:1 裁剪（不缩放）的灰度小图，仅用于块效应检测。
    // 8x8 编码块在 256 宽的降采样图上只剩 1px 多，周期结构已被抹平；
    // 只有 1:1 的局部裁剪才能保住块边界，让 block_size 真正按源像素生效。
    struct BlockProbeRegion {
        int x = 0; // 左上角在源帧中的坐标（便于复现/排查）
        int y = 0;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> gray; // width * height，GRAY8，无行对齐

        bool valid() const {
            return width >= 16 && height >= 16 &&
                   gray.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
        }
    };
    // 中心 + 四角最多 5 块: 马赛克/花屏常只出现在画面局部，只看中心会被
    // 局部损伤拉低甚至完全漏掉。每块 <= 256x256，合计约 320KB，随采样帧释放。
    std::vector<BlockProbeRegion> block_probes;

    // 源帧的亮度范围 / 传输特性。limited_range 时曝光阈值需要按 16-235 重新标定；
    // hdr（PQ/HLG）且 tone_mapped=false 时曝光/色偏指标不可信，置为不可用。
    bool limited_range = false;
    bool hdr = false;
    // PQ / HLG 帧已在采样后做过近似色调映射（见 QualityAnalyzer::ApplyHdrToneMap）:
    // 样本码值已换算到 SDR 显示口径，曝光/色偏判定可以照常进行。
    bool tone_mapped = false;

    int rgb_width = 0;
    int rgb_height = 0;
    std::vector<uint8_t> rgb; // RGB24，色偏检测与证据缩略图用；可为空

    // 该时刻音频是否静音。冻结帧判定要用: 静止画面 + 静音 = 正常的静帧（片尾/黑屏留白），
    // 静止画面 + 有声音 = 真正的冻结故障。没有音频流时填 false（无从判断，按"不静音"处理）。
    bool audio_silent = false;

    bool valid() const {
        return width > 0 && height > 0 && gray.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    bool has_block_probes() const {
        for (const auto& probe : block_probes) {
            if (probe.valid())
                return true;
        }
        return false;
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
    double luma_mean = kQualityNoValue;       // 全帧亮度均值 [0,255]
    double luma_std = kQualityNoValue;        // 亮度标准差（对比度参考）
    double black_ratio = kQualityNoValue;     // Y <= black_luma 的像素占比（黑场判定）
    double dark_ratio = kQualityNoValue;      // Y <= shadow_luma 的像素占比（欠曝判定）
    double highlight_ratio = kQualityNoValue; // Y >= highlight_luma 的像素占比（过曝判定）
    double clip_high_ratio = kQualityNoValue; // Y >= clip_high_luma，硬削波（高光死白）
    double clip_low_ratio = kQualityNoValue;  // Y <= clip_low_luma，暗部压死

    // ---- 清晰度 ----
    // 拉普拉斯方差 / 1000（见 VisualDefectAnalyzer 的说明）。越小越模糊。
    double blur_score = kQualityNoValue;

    // ---- 时域（与上一采样帧比较）----
    double frame_diff = kQualityNoValue; // 平均绝对差 / 255，0 = 完全相同（冻结判定）
    double luma_delta = kQualityNoValue; // 亮度均值变化的绝对值 [0,255]（闪烁判定）

    // ---- 色彩 ----
    double color_cast_score = kQualityNoValue; // 色偏强度 0..1（R/B 偏离亮度的合成量 / 255）
    double color_cast_rb = kQualityNoValue;    // mean(R) - mean(B)，>0 偏红，<0 偏蓝

    // ---- 结构 ----
    double combing_score = kQualityNoValue;    // 隔行梳齿 0..1
    double blockiness_score = kQualityNoValue; // 马赛克 / 块效应 0..1

    // ---- 有效画面区域（letterbox / pillarbox）----
    ActivePictureArea active_area;

    // ---- 有参考指标（需要参考帧，见 QualityAnalyzer::CompareSamples）----
    double psnr_db = kQualityNoValue;
    double ssim = kQualityNoValue;
    // VMAF 需要可选依赖 libvmaf，默认构建未接入 —— 用 optional 而非 NaN：
    // "未启用"（nullopt）必须与"算失败"（NaN）可区分，否则报告与曲线分不清
    std::optional<double> vmaf;

    bool valid = false;
    std::string error_message;
};

} // namespace model
} // namespace videoeye
