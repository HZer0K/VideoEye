#include "core/domain/model/BitrateGopResult.h"

#include <cstdio>

namespace videoeye {
namespace model {

double model::BitrateGopAnalysis::IFrameRatio() const {
    const int64_t total = i_frame_count + p_frame_count + b_frame_count + unknown_frame_count;
    if (total <= 0) return 0.0;
    return static_cast<double>(i_frame_count) / static_cast<double>(total);
}

double model::BitrateGopAnalysis::PFrameRatio() const {
    const int64_t total = i_frame_count + p_frame_count + b_frame_count + unknown_frame_count;
    if (total <= 0) return 0.0;
    return static_cast<double>(p_frame_count) / static_cast<double>(total);
}

double model::BitrateGopAnalysis::BFrameRatio() const {
    const int64_t total = i_frame_count + p_frame_count + b_frame_count + unknown_frame_count;
    if (total <= 0) return 0.0;
    return static_cast<double>(b_frame_count) / static_cast<double>(total);
}

double model::BitrateGopAnalysis::AverageFrameBytes() const {
    if (total_frames <= 0) return 0.0;
    return static_cast<double>(total_bytes) / static_cast<double>(total_frames);
}

double model::BitrateGopAnalysis::AverageIFrameBytes() const {
    if (i_frame_count <= 0) return 0.0;
    if (i_frame_bytes_ <= 0) return AverageFrameBytes();
    return static_cast<double>(i_frame_bytes_) / static_cast<double>(i_frame_count);
}

int model::BitrateGopAnalysis::CountAnomalies(model::BitrateAnomalyType type) const {
    int n = 0;
    for (const auto& a : anomalies) {
        if (a.type == type) ++n;
    }
    return n;
}

std::vector<const model::BitrateAnomaly*> model::BitrateGopAnalysis::AnomaliesOf(BitrateAnomalyType type) const {
    std::vector<const BitrateAnomaly*> out;
    for (const auto& a : anomalies) {
        if (a.type == type) out.push_back(&a);
    }
    return out;
}

std::string model::BitrateGopAnalysis::ToString() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "码率: 平均 %.0f kbps / 峰值 %.0f kbps / 峰均比 %.2f (窗口 %.2fs) | "
                  "GOP: %zu 个, 平均 %.2fs (%d~%d 帧), 最长 %.2fs, 超长 %d 个 | "
                  "帧类型: I=%lld P=%lld B=%lld 未知=%lld | 异常 %zu 条",
                  avg_bitrate_kbps, peak_bitrate_kbps, peak_to_mean_ratio, window_seconds,
                  gops.size(), gop_duration_mean, gop_frames_min, gop_frames_max,
                  gop_duration_max, long_gop_count,
                  static_cast<long long>(i_frame_count), static_cast<long long>(p_frame_count),
                  static_cast<long long>(b_frame_count),
                  static_cast<long long>(unknown_frame_count), anomalies.size());
    return std::string(buf);
}

const char* ToString(model::BitrateAnomalyType type) {
    switch (type) {
        case BitrateAnomalyType::LongGop:                    return "超长 GOP";
        case BitrateAnomalyType::IrregularKeyInterval:       return "关键帧间隔不均匀";
        case BitrateAnomalyType::PeakOvershoot:              return "码率峰值超标";
        case BitrateAnomalyType::OversizedFrame:             return "异常大帧";
        case BitrateAnomalyType::OversizedIFrame:            return "I 帧过大";
        case BitrateAnomalyType::SceneChangeWithoutKeyframe: return "场景切换缺少关键帧";
        case BitrateAnomalyType::SparseKeyframes:            return "关键帧过于稀疏";
    }
    return "未知";
}

}  // namespace model
}  // namespace videoeye
