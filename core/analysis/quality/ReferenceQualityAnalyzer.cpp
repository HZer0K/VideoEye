#include "core/analysis/quality/ReferenceQualityAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "core/analysis/quality/QualityAnalyzer.h"

namespace videoeye {
namespace {

// 采样序列的首末时间戳差（少于 2 帧时无法估算，返回 0）
double EstimateDuration(const std::vector<model::FrameSample>& samples) {
    if (samples.size() < 2)
        return 0.0;
    const double duration = samples.back().timestamp_seconds - samples.front().timestamp_seconds;
    return duration > 0.0 ? duration : 0.0;
}

} // namespace

ReferenceQualityAccumulator::ReferenceQualityAccumulator(const model::ReferenceQualityOptions& options)
    : options_(options), min_psnr_(std::numeric_limits<double>::infinity()),
      min_ssim_(std::numeric_limits<double>::infinity()) {
    result_.options = options;
}

void ReferenceQualityAccumulator::SetSampleCounts(int reference_count, int distorted_count) {
    result_.summary.reference_sample_count = reference_count;
    result_.summary.distorted_sample_count = distorted_count;
}

void ReferenceQualityAccumulator::SetDurations(double reference_seconds, double distorted_seconds) {
    result_.summary.reference_duration_seconds = reference_seconds;
    result_.summary.distorted_duration_seconds = distorted_seconds;
}

void ReferenceQualityAccumulator::SetTargetCount(int target_count) {
    result_.summary.target_count = target_count;
}

bool ReferenceQualityAccumulator::AddPair(const model::FrameSample& reference, const model::FrameSample& distorted,
                                          double reference_time_seconds, double distorted_time_seconds) {
    if (options_.max_frames > 0 && attempts_ >= options_.max_frames) {
        result_.summary.truncated = true;
        return false;
    }
    ++attempts_;

    const double delta = distorted_time_seconds - reference_time_seconds - options_.offset_seconds;

    model::ReferenceFrameMetric frame;
    frame.index = static_cast<int>(result_.frames.size());
    frame.timestamp_seconds = reference_time_seconds;
    frame.reference_frame_index = reference.frame_index;
    frame.distorted_frame_index = distorted.frame_index;
    frame.reference_timestamp_seconds = reference_time_seconds;
    frame.distorted_timestamp_seconds = distorted_time_seconds;
    frame.alignment_delta_seconds = delta;
    result_.summary.max_alignment_delta_seconds =
        std::max(result_.summary.max_alignment_delta_seconds, std::fabs(delta));

    const auto metrics = QualityAnalyzer::CompareSamples(reference, distorted);
    // 失败时不要让 QualityMetrics 的 0 默认值流进结果: 0 dB 看起来像"算出来了但极差"，
    // 而这里的语义是"没算出来"，按模型约定留 NaN
    frame.width = reference.width;
    frame.height = reference.height;
    frame.valid = metrics.valid;
    frame.error_message = metrics.error_message;

    if (metrics.valid) {
        frame.width = metrics.width;
        frame.height = metrics.height;
        frame.psnr_db = metrics.psnr_nb;
        frame.ssim = metrics.ssim;
        ++result_.summary.compared_frames;
        ssim_sum_ += metrics.ssim;
        if (metrics.ssim < min_ssim_) {
            min_ssim_ = metrics.ssim;
            result_.summary.min_ssim_frame_index = frame.index;
        }
        if (std::isfinite(metrics.psnr_nb)) {
            finite_psnr_sum_ += metrics.psnr_nb;
            ++finite_psnr_count_;
            if (metrics.psnr_nb < min_psnr_) {
                min_psnr_ = metrics.psnr_nb;
                result_.summary.min_psnr_frame_index = frame.index;
            }
        } else {
            ++result_.summary.lossless_frames;
        }
    } else {
        ++result_.summary.failed_frames;
    }
    result_.frames.push_back(std::move(frame));
    return true;
}

void ReferenceQualityAccumulator::AddFailure(double target_time_seconds, const std::string& error) {
    ++attempts_;
    ++result_.summary.failed_frames;

    model::ReferenceFrameMetric frame;
    frame.index = static_cast<int>(result_.frames.size());
    frame.timestamp_seconds = target_time_seconds;
    frame.valid = false;
    frame.error_message = error;
    result_.frames.push_back(std::move(frame));
}

void ReferenceQualityAccumulator::AddSkipped() {
    ++result_.summary.skipped_frames;
}

model::ReferenceQualityResult ReferenceQualityAccumulator::Finish() {
    auto& summary = result_.summary;
    if (summary.compared_frames > 0) {
        summary.mean_ssim = ssim_sum_ / summary.compared_frames;
        summary.min_ssim = min_ssim_;
        if (finite_psnr_count_ > 0) {
            summary.mean_psnr_db = finite_psnr_sum_ / finite_psnr_count_;
            summary.min_psnr_db = min_psnr_;
        } else {
            // 全部无损: 均值与最小值都是 +inf（真值），不是"没算出来"
            summary.mean_psnr_db = std::numeric_limits<double>::infinity();
            summary.min_psnr_db = std::numeric_limits<double>::infinity();
        }
    }
    summary.coverage =
        summary.target_count > 0 ? static_cast<double>(summary.compared_frames) / summary.target_count : 0.0;

    const double duration_tolerance = std::max(0.5, 0.02 * summary.reference_duration_seconds);
    summary.duration_mismatch =
        std::fabs(summary.reference_duration_seconds - summary.distorted_duration_seconds) > duration_tolerance;

    result_.valid = summary.compared_frames > 0;
    if (!result_.valid) {
        if (summary.failed_frames > 0) {
            // 把第一个具体错误抛出来（通常是两侧分辨率/宽高比不一致），别只报"没有可比对帧"
            for (const auto& frame : result_.frames) {
                if (!frame.valid) {
                    result_.error_message = "所有帧比较失败: " + frame.error_message;
                    break;
                }
            }
        } else {
            result_.error_message = "按当前对齐参数没有可比对的帧（检查 offset / tolerance，或两侧时长是否一致）";
        }
    }
    return std::move(result_);
}

model::ReferenceQualityResult ReferenceQualityAnalyzer::Compare(const std::vector<model::FrameSample>& reference,
                                                                const std::vector<model::FrameSample>& distorted,
                                                                const model::ReferenceQualityOptions& options) {
    ReferenceQualityAccumulator accumulator(options);
    accumulator.SetSampleCounts(static_cast<int>(reference.size()), static_cast<int>(distorted.size()));
    accumulator.SetDurations(EstimateDuration(reference), EstimateDuration(distorted));

    if (reference.empty()) {
        auto result = accumulator.Finish();
        result.valid = false;
        result.error_message = "参考视频没有可用的采样帧";
        return result;
    }
    if (distorted.empty()) {
        auto result = accumulator.Finish();
        result.valid = false;
        result.error_message = "待测视频没有可用的采样帧";
        return result;
    }
    if (!(options.alignment_tolerance_seconds >= 0.0)) {
        // 也挡住 NaN: 容差是 NaN 时所有比较都会变成"没对上"
        auto result = accumulator.Finish();
        result.valid = false;
        result.error_message = "对齐容差必须是非负数";
        return result;
    }

    const double reference_base = reference.front().timestamp_seconds;
    const double distorted_base = distorted.front().timestamp_seconds;
    const double tolerance = options.alignment_tolerance_seconds;

    // 采样窗口内的参考时间点
    std::vector<size_t> targets;
    targets.reserve(reference.size());
    for (size_t i = 0; i < reference.size(); ++i) {
        const double t = reference[i].timestamp_seconds - reference_base;
        if (t < options.start_seconds - 1e-9)
            continue;
        if (options.end_seconds >= 0.0 && t > options.end_seconds + 1e-9)
            continue;
        targets.push_back(i);
    }
    accumulator.SetTargetCount(static_cast<int>(targets.size()));

    // 待测侧单调游标: 已消费过的帧不会再被后面更晚的时间点抓回来重复使用
    size_t cursor = 0;
    for (size_t target_pos = 0; target_pos < targets.size(); ++target_pos) {
        const model::FrameSample& ref = reference[targets[target_pos]];
        const double target_time = (ref.timestamp_seconds - reference_base) + options.offset_seconds;

        // 跳过明显早于目标时间（含容差）的待测帧
        while (cursor < distorted.size() &&
               (distorted[cursor].timestamp_seconds - distorted_base) < target_time - tolerance) {
            ++cursor;
        }
        if (cursor >= distorted.size()) {
            // 待测序列已耗尽: 剩下的时间点逐个记跳过（不 break，否则 skipped 与
            // coverage 会对不上 target_count）
            accumulator.AddSkipped();
            continue;
        }

        double delta = (distorted[cursor].timestamp_seconds - distorted_base) - target_time;
        double abs_delta = std::fabs(delta);
        size_t matched = cursor;
        if (cursor + 1 < distorted.size()) {
            const double next_delta = (distorted[cursor + 1].timestamp_seconds - distorted_base) - target_time;
            if (std::fabs(next_delta) < abs_delta) {
                matched = cursor + 1;
                delta = next_delta;
                abs_delta = std::fabs(next_delta);
            }
        }

        if (abs_delta > tolerance) {
            // 不硬配: 没有对应时刻的画面就不出指标
            accumulator.AddSkipped();
            continue;
        }

        // 消费该待测帧；下一轮从它后面开始找
        cursor = matched + 1;
        if (!accumulator.AddPair(ref, distorted[matched], ref.timestamp_seconds - reference_base,
                                 distorted[matched].timestamp_seconds - distorted_base))
            break; // 命中 max_frames
    }
    return accumulator.Finish();
}

} // namespace videoeye
