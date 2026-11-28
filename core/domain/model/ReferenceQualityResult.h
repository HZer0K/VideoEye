#pragma once

// 参考帧质量对比（双视频 PSNR / SSIM）的结果模型。
//
// 依赖方向: ReferenceQualityAnalyzer（生产） -> 本文件 -> reporting / ui（消费）。
// 这里不出现 AVFrame、FFmpeg 类型和 Qt —— 报告与界面只认这些纯值对象。
//
// 取值约定与 QualityMetrics / FrameQualityMetric 保持一致:
//   * 算不出来写 kQualityNoValue（NaN），不用 0 顶替
//   * 无损帧（两帧逐像素一致）的 PSNR 是 +inf，这是真值，不是异常
//   * valid == false 时先读 error_message，再去读 frames / summary 里的细节

#include <string>
#include <vector>

#include "core/domain/model/QualityMetric.h"

namespace videoeye {
namespace model {

// 单帧对比结果。
// 只记录"真正尝试比较过"的帧: 容差内找不到待测帧的时间点不会生成记录，
// 只在 summary.skipped_frames 里计数，避免把"没对上"伪装成"对上了但很差"。
struct ReferenceFrameMetric {
    int index = 0;                  // 比较序号（0 起）
    double timestamp_seconds = 0.0; // 对齐后的参考时间点（首帧归一后）
    int reference_frame_index = -1; // 两侧源解码帧序号，便于回查
    int distorted_frame_index = -1;
    double reference_timestamp_seconds = 0.0; // 首帧归一后的参考帧时间
    double distorted_timestamp_seconds = 0.0; // 首帧归一后的待测帧时间
    // 待测帧时间 - (参考时间 + offset)，正数表示待测帧更晚。
    double alignment_delta_seconds = 0.0;
    int width = 0; // 实际参与比较的样本尺寸（两侧必须一致）
    int height = 0;
    double psnr_db = kQualityNoValue;
    double ssim = kQualityNoValue;
    bool valid = false;
    std::string error_message;
};

// 对比参数。
struct ReferenceQualityOptions {
    // 采样窗口（以参考视频首帧归一后的时间计）。end_seconds < 0 表示到参考视频末尾。
    double start_seconds = 0.0;
    double end_seconds = -1.0;
    // 待测视频相对参考的整体时间偏移，>0 表示待测内容出现得更晚。
    double offset_seconds = 0.0;
    // 单帧对齐容差: 最近的待测帧与目标时间差超过它就算"没对上"，不硬配。
    // 默认 0.042s 约等于 24fps 一帧，避免把一个时间点强行配给隔了好几帧的画面。
    double alignment_tolerance_seconds = 0.042;
    // 最多尝试比较的帧数，0 = 不限。命中上限时 summary.truncated = true。
    int max_frames = 0;
    // 说明: 抽帧密度由调用方（解码侧）决定 —— 只有目标时间附近的帧才会被转成
    // FrameSample，分析器不再二次抽帧，避免"两处各抽一次"导致覆盖率无法解释。
};

// 汇总指标。
struct ReferenceQualitySummary {
    double reference_duration_seconds = 0.0; // 按采样序列首末时间戳估算，调用方可覆盖
    double distorted_duration_seconds = 0.0;
    int reference_sample_count = 0;
    int distorted_sample_count = 0;
    int target_count = 0;    // 采样窗口内的参考时间点数（截断前）
    int compared_frames = 0; // 成功算出指标的帧数
    int failed_frames = 0;   // 对上了但比较失败（尺寸不一致等），错误在 frames[i].error_message
    int skipped_frames = 0;  // 容差内找不到待测帧
    int lossless_frames = 0; // PSNR 为 +inf 的帧数（逐帧完全一致）
    double coverage = 0.0;   // compared_frames / target_count，[0, 1]
    // 均值只统计有限值: 全部无损时 mean_psnr_db 为 +inf；混合情况下无穷值
    // 不参与平均，避免一帧无损把均值拉成 inf。
    double mean_psnr_db = kQualityNoValue;
    double mean_ssim = kQualityNoValue;
    double min_psnr_db = kQualityNoValue;
    double min_ssim = kQualityNoValue;
    int min_psnr_frame_index = -1; // 最差帧（PSNR / SSIM 各一）
    int min_ssim_frame_index = -1;
    double max_alignment_delta_seconds = 0.0;
    bool truncated = false;         // 因 max_frames 提前结束
    bool duration_mismatch = false; // 两侧时长差异超过容差（max(0.5s, 参考时长 2%)）
};

struct ReferenceQualityResult {
    ReferenceQualityOptions options;
    std::vector<ReferenceFrameMetric> frames;
    ReferenceQualitySummary summary;
    bool valid = false;     // 至少有一帧比较成功
    bool cancelled = false; // 解码执行层被取消（已完成的帧仍然保留在 frames/summary 里）
    std::string error_message;
};

} // namespace model
} // namespace videoeye
