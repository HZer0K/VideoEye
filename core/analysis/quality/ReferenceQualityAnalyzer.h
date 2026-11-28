#pragma once

#include <vector>

#include "core/domain/model/QualityMetric.h"
#include "core/domain/model/ReferenceQualityResult.h"

namespace videoeye {

// 双视频参考质量对比的纯逻辑层: 对齐 + 逐帧 PSNR/SSIM + 汇总。
//
// 不做解码、不碰 FFmpeg —— 输入是两侧按时间递增的降采样样本（FrameSample），
// 时间戳由解码侧按 pts * time_base 填写。这样对齐/统计逻辑可以脱离媒体文件单测。
//
// 对齐规则:
//   1. 首帧归一: 两侧时间戳各自减去自己首帧时间（容器起始 PTS 常不是 0）
//   2. 参考时间 t 对应的待测时间 = t + options.offset_seconds
//   3. 仅当最近的待测帧与目标时间差 <= options.alignment_tolerance_seconds 才算命中；
//      超出容差记入 skipped_frames，绝不在没有对应画面的情况下伪造指标
//   4. 待测帧按时间单调消费，同一个待测帧不会被两个参考时间点重复使用
class ReferenceQualityAnalyzer {
public:
    static model::ReferenceQualityResult Compare(const std::vector<model::FrameSample>& reference,
                                                 const std::vector<model::FrameSample>& distorted,
                                                 const model::ReferenceQualityOptions& options);
};

// 逐帧累积器: Compare() 与解码执行层（ReferenceQualityRunner）共用同一套
// 指标计算与汇总规则 —— 两条入口产出的 ReferenceQualityResult 必须同口径，
// 否则"离线批量比"与"流式解码比"的均值/最小值会对不上。
class ReferenceQualityAccumulator {
public:
    explicit ReferenceQualityAccumulator(const model::ReferenceQualityOptions& options);

    void SetSampleCounts(int reference_count, int distorted_count);
    void SetDurations(double reference_seconds, double distorted_seconds);
    void SetTargetCount(int target_count);

    // 一对已经对齐好的样本。两侧时间都传"首帧归一后"的值，对齐偏差由两者差
    // 减去 options.offset_seconds 推出。
    // 返回 false 表示已达到 options.max_frames，调用方应停止并标记 truncated。
    bool AddPair(const model::FrameSample& reference, const model::FrameSample& distorted,
                 double reference_time_seconds, double distorted_time_seconds);

    // 命中了时间点但样本构建失败（解码侧缩放失败等），错误原文保留到结果里
    void AddFailure(double target_time_seconds, const std::string& error);

    // 容差内没有对应待测帧
    void AddSkipped();

    // 汇总均值/最小值/覆盖率，并给出 valid 与 error_message
    model::ReferenceQualityResult Finish();

private:
    model::ReferenceQualityOptions options_;
    model::ReferenceQualityResult result_;
    int attempts_ = 0;
    double finite_psnr_sum_ = 0.0;
    int finite_psnr_count_ = 0;
    double ssim_sum_ = 0.0;
    double min_psnr_ = 0.0;
    double min_ssim_ = 0.0;
};

} // namespace videoeye
