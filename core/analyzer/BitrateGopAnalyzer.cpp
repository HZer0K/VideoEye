#include "core/analyzer/BitrateGopAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace videoeye {
namespace analyzer {
namespace {

// 单条曲线最多保留的采样点。超过则自动加大 hop（步进），避免长视频把 UI 图表打爆。
constexpr int kMaxCurvePoints = 20000;
// 折线图上 I 帧 / 关键帧标记的最大数量
constexpr int kMaxMarkerPoints = 20000;

std::string FormatValue(double value, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

std::string FormatBytes(double bytes) {
    if (bytes >= 1024.0 * 1024.0) return FormatValue(bytes / 1024.0 / 1024.0, 2) + " MB";
    if (bytes >= 1024.0) return FormatValue(bytes / 1024.0, 1) + " KB";
    return FormatValue(bytes, 0) + " B";
}

std::string FormatTime(double seconds) {
    const int total_ms = static_cast<int>(seconds * 1000.0);
    const int h = total_ms / 3600000;
    const int m = (total_ms % 3600000) / 60000;
    const int s = (total_ms % 60000) / 1000;
    const int ms = total_ms % 1000;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", h, m, s, ms);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", m, s, ms);
    }
    return std::string(buf);
}

double MeanOf(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double StdDevOf(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    const double mean = MeanOf(values);
    double acc = 0.0;
    for (const double v : values) {
        const double d = v - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(values.size() - 1));
}

// 在已排序的时刻表中找最近距离
double NearestDistance(const std::vector<double>& sorted_values, double target) {
    if (sorted_values.empty()) return -1.0;
    auto it = std::lower_bound(sorted_values.begin(), sorted_values.end(), target);
    double best = std::numeric_limits<double>::max();
    if (it != sorted_values.end()) best = std::min(best, std::abs(*it - target));
    if (it != sorted_values.begin()) {
        --it;
        best = std::min(best, std::abs(*it - target));
    }
    return best;
}

}  // namespace

const char* ToString(BitrateAnomalyType type) {
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

// ---------------- BitrateGopAnalysis ----------------

double BitrateGopAnalysis::IFrameRatio() const {
    const int64_t total = i_frame_count + p_frame_count + b_frame_count + unknown_frame_count;
    if (total <= 0) return 0.0;
    return static_cast<double>(i_frame_count) / static_cast<double>(total);
}

double BitrateGopAnalysis::PFrameRatio() const {
    const int64_t total = i_frame_count + p_frame_count + b_frame_count + unknown_frame_count;
    if (total <= 0) return 0.0;
    return static_cast<double>(p_frame_count) / static_cast<double>(total);
}

double BitrateGopAnalysis::BFrameRatio() const {
    const int64_t total = i_frame_count + p_frame_count + b_frame_count + unknown_frame_count;
    if (total <= 0) return 0.0;
    return static_cast<double>(b_frame_count) / static_cast<double>(total);
}

double BitrateGopAnalysis::AverageFrameBytes() const {
    if (total_frames <= 0) return 0.0;
    return static_cast<double>(total_bytes) / static_cast<double>(total_frames);
}

double BitrateGopAnalysis::AverageIFrameBytes() const {
    if (i_frame_count <= 0) return 0.0;
    if (i_frame_bytes_ <= 0) return AverageFrameBytes();
    return static_cast<double>(i_frame_bytes_) / static_cast<double>(i_frame_count);
}

int BitrateGopAnalysis::CountAnomalies(BitrateAnomalyType type) const {
    int n = 0;
    for (const auto& a : anomalies) {
        if (a.type == type) ++n;
    }
    return n;
}

std::vector<const BitrateAnomaly*> BitrateGopAnalysis::AnomaliesOf(BitrateAnomalyType type) const {
    std::vector<const BitrateAnomaly*> out;
    for (const auto& a : anomalies) {
        if (a.type == type) out.push_back(&a);
    }
    return out;
}

std::string BitrateGopAnalysis::ToString() const {
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

// ---------------- BitrateGopAnalyzer ----------------

void BitrateGopAnalyzer::Reset(const BitrateGopOptions& options) {
    options_ = options;
    result_ = BitrateGopAnalysis{};
    samples_.clear();
    scene_changes_.clear();
    current_gop_ = model::GopInfo{};
    gop_open_ = false;
    video_stream_index_ = -1;
    last_timestamp_ = -1.0;
    finished_ = false;
}

void BitrateGopAnalyzer::SetOptions(const BitrateGopOptions& options) {
    options_ = options;
}

void BitrateGopAnalyzer::OnPacket(int stream_index, double timestamp_seconds, int64_t size,
                                  bool is_key) {
    AddSample(stream_index, timestamp_seconds, size, model::FrameType::Unknown, is_key);
}

void BitrateGopAnalyzer::OnFrame(int stream_index, double timestamp_seconds, int64_t size,
                                 model::FrameType type, bool is_idr) {
    AddSample(stream_index, timestamp_seconds, size, type, is_idr);
}

void BitrateGopAnalyzer::AddSample(int stream_index, double timestamp_seconds, int64_t size,
                                   model::FrameType type, bool is_idr) {
    if (finished_) return;  // Finish() 之后忽略新数据，避免结果被追加污染
    if (timestamp_seconds < 0.0) timestamp_seconds = std::max(last_timestamp_, 0.0);
    if (last_timestamp_ >= 0.0 && timestamp_seconds < last_timestamp_) {
        timestamp_seconds = last_timestamp_;  // 非单调保护（B 帧/时间戳回退）
    }
    last_timestamp_ = timestamp_seconds;

    if (video_stream_index_ < 0) video_stream_index_ = stream_index;
    if (stream_index != video_stream_index_) return;  // 只跟踪第一条视频流

    if (is_idr && type == model::FrameType::Unknown) type = model::FrameType::I;

    result_.total_frames += 1;
    result_.total_bytes += size;
    switch (type) {
        case model::FrameType::I:
        case model::FrameType::SI:
            result_.i_frame_count += 1;
            result_.i_frame_bytes_ += size;
            break;
        case model::FrameType::P:
        case model::FrameType::SP:
            result_.p_frame_count += 1;
            break;
        case model::FrameType::B:
        case model::FrameType::BI:
            result_.b_frame_count += 1;
            break;
        default:
            result_.unknown_frame_count += 1;
            break;
    }

    // ---- GOP 边界 ----
    if (is_idr) {
        if (gop_open_) CloseCurrentGop(timestamp_seconds, /*complete=*/true);
        StartGop(timestamp_seconds, stream_index, /*closed=*/true);
    } else if (!gop_open_) {
        // 首个样本不是关键帧（可能是 open GOP 或截断文件）
        StartGop(timestamp_seconds, stream_index, /*closed=*/false);
    }

    model::GopInfo& gop = current_gop_;
    gop.frame_count += 1;
    gop.byte_count += size;
    switch (type) {
        case model::FrameType::I:
        case model::FrameType::SI:
            gop.i_count += 1;
            if (size > gop.max_i_frame_bytes) gop.max_i_frame_bytes = size;
            break;
        case model::FrameType::P:
        case model::FrameType::SP:
            gop.p_count += 1;
            break;
        case model::FrameType::B:
        case model::FrameType::BI:
            gop.b_count += 1;
            break;
        default:
            gop.unknown_count += 1;
            break;
    }
    if (type != model::FrameType::Unknown) gop.frame_types_known = true;
    if (size > gop.max_frame_bytes) {
        gop.max_frame_bytes = size;
        gop.max_frame_seconds = timestamp_seconds;
    }

    if (is_idr) {
        if (result_.key_frame_seconds.size() < kMaxMarkerPoints) {
            result_.key_frame_seconds.push_back(timestamp_seconds);
        }
    }
    if (type == model::FrameType::I || type == model::FrameType::SI) {
        if (result_.i_frame_seconds.size() < kMaxMarkerPoints) {
            result_.i_frame_seconds.push_back(timestamp_seconds);
        }
    }

    samples_.push_back(Sample{timestamp_seconds, size, type, is_idr});
}

void BitrateGopAnalyzer::StartGop(double timestamp_seconds, int stream_index, bool closed) {
    current_gop_ = model::GopInfo{};
    current_gop_.index = static_cast<int>(result_.gops.size());
    current_gop_.stream_index = stream_index;
    current_gop_.start_seconds = timestamp_seconds;
    current_gop_.end_seconds = timestamp_seconds;
    current_gop_.closed_gop = closed;
    gop_open_ = true;
}

void BitrateGopAnalyzer::CloseCurrentGop(double end_seconds, bool complete) {
    if (!gop_open_) return;
    current_gop_.end_seconds = std::max(end_seconds, current_gop_.start_seconds);
    current_gop_.complete = complete;
    result_.gops.push_back(current_gop_);
    gop_open_ = false;
}

void BitrateGopAnalyzer::AssociateSceneChanges(const std::vector<SceneChangeResult>& changes) {
    scene_changes_ = changes;
    // 若已 Finish()，允许补充关联并重新跑匹配（不改码率/GOP 统计）
    if (finished_) {
        ApplySceneChanges(result_, changes, options_);
    }
}

void BitrateGopAnalyzer::ApplySceneChanges(BitrateGopAnalysis& result,
                                           const std::vector<SceneChangeResult>& changes,
                                           const BitrateGopOptions& options) {
    // 先剔除旧的同类异常，避免重复累积
    result.anomalies.erase(
        std::remove_if(result.anomalies.begin(), result.anomalies.end(),
                       [](const BitrateAnomaly& a) {
                           return a.type == BitrateAnomalyType::SceneChangeWithoutKeyframe;
                       }),
        result.anomalies.end());
    MatchSceneChanges(result, changes, options);
    std::sort(result.anomalies.begin(), result.anomalies.end(),
              [](const BitrateAnomaly& a, const BitrateAnomaly& b) {
                  if (a.start_seconds != b.start_seconds) return a.start_seconds < b.start_seconds;
                  return a.type < b.type;
              });
    RebuildSuggestions(result, options);
}

const BitrateGopAnalysis& BitrateGopAnalyzer::Finish() {
    if (finished_) return result_;

    // 收尾最后一个未闭合的 GOP：末尾补一个估测帧间隔
    if (gop_open_) {
        double frame_interval = 0.0;
        if (samples_.size() > 1) {
            frame_interval = (last_timestamp_ - samples_.front().timestamp_seconds) /
                             static_cast<double>(samples_.size() - 1);
        }
        CloseCurrentGop(last_timestamp_ + std::max(frame_interval, 0.0), /*complete=*/false);
    }

    const int64_t known = result_.i_frame_count + result_.p_frame_count + result_.b_frame_count;
    result_.frame_types_known =
        (result_.total_frames > 0) &&
        (static_cast<double>(known) / static_cast<double>(result_.total_frames) > 0.5);

    BuildBitrateCurves();
    SummarizeBitrate();

    // ---- GOP 统计 ----
    std::vector<double> durations;
    std::vector<double> intervals;  // 仅统计已收尾的 GOP（完整间隔）
    std::vector<int> frame_counts;
    durations.reserve(result_.gops.size());
    intervals.reserve(result_.gops.size());
    frame_counts.reserve(result_.gops.size());
    for (const auto& gop : result_.gops) {
        durations.push_back(gop.DurationSeconds());
        frame_counts.push_back(gop.frame_count);
        if (gop.complete) intervals.push_back(gop.DurationSeconds());
        if (gop.closed_gop) {
            result_.closed_gop_count += 1;
        } else {
            result_.open_gop_count += 1;
        }
    }
    result_.gop_duration_mean = MeanOf(durations);
    result_.gop_duration_stddev = StdDevOf(durations);
    result_.gop_duration_max = durations.empty() ? 0.0
                                                 : *std::max_element(durations.begin(), durations.end());
    if (!frame_counts.empty()) {
        result_.gop_frames_min = *std::min_element(frame_counts.begin(), frame_counts.end());
        result_.gop_frames_max = *std::max_element(frame_counts.begin(), frame_counts.end());
        result_.gop_frames_mean = std::accumulate(frame_counts.begin(), frame_counts.end(), 0.0) /
                                  static_cast<double>(frame_counts.size());
    }
    result_.key_interval_mean = MeanOf(intervals);
    result_.key_interval_stddev = StdDevOf(intervals);
    result_.key_interval_irregularity =
        (result_.key_interval_mean > 0.0)
            ? result_.key_interval_stddev / result_.key_interval_mean
            : 0.0;

    DetectGopAnomalies();
    DetectPeakOvershoot();
    DetectOversizedFrames();
    DetectSparseKeyframes();
    MatchSceneChanges(result_, scene_changes_, options_);
    RebuildSuggestions(result_, options_);

    std::sort(result_.anomalies.begin(), result_.anomalies.end(),
              [](const BitrateAnomaly& a, const BitrateAnomaly& b) {
                  if (a.start_seconds != b.start_seconds) return a.start_seconds < b.start_seconds;
                  return a.type < b.type;
              });

    finished_ = true;
    return result_;
}

void BitrateGopAnalyzer::BuildBitrateCurves() {
    if (samples_.empty()) return;

    // B 帧存在时解码顺序 != 显示顺序，码率应按显示时间(PTS)统计
    std::vector<Sample> sorted = samples_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Sample& a, const Sample& b) {
                  return a.timestamp_seconds < b.timestamp_seconds;
              });

    std::vector<int64_t> prefix(sorted.size() + 1, 0);
    for (size_t i = 0; i < sorted.size(); ++i) {
        prefix[i + 1] = prefix[i] + sorted[i].size;
    }

    const double first = sorted.front().timestamp_seconds;
    const double last = sorted.back().timestamp_seconds;
    const double span = std::max(last - first, 0.0);
    const double frame_interval =
        (sorted.size() > 1) ? span / static_cast<double>(sorted.size() - 1) : 0.0;
    result_.duration_seconds = span + frame_interval;

    const double default_window =
        options_.windows_seconds.empty() ? 1.0 : options_.windows_seconds.front();
    result_.window_seconds = default_window;

    for (double window : options_.windows_seconds) {
        if (window <= 0.0) continue;

        model::MetricSeries series;
        series.name = "bitrate_" + FormatValue(window, 2) + "s";
        series.unit = "kbps";

        double hop = window / 4.0;  // 75% 重叠，曲线更平滑
        if (span > 0.0 && (span / hop) + 1.0 > static_cast<double>(kMaxCurvePoints)) {
            hop = span / static_cast<double>(kMaxCurvePoints - 1);
        }
        if (hop <= 0.0) hop = window;

        const bool is_default = std::abs(window - default_window) < 1e-9;
        size_t lo = 0;
        size_t hi = 0;
        double cursor = first;
        int guard = 0;
        while (cursor <= last + 1e-9 && guard <= kMaxCurvePoints + 2) {
            ++guard;
            while (lo < sorted.size() && sorted[lo].timestamp_seconds < cursor) ++lo;
            if (hi < lo) hi = lo;
            const double window_end = cursor + window;
            while (hi < sorted.size() && sorted[hi].timestamp_seconds < window_end) ++hi;
            if (lo >= sorted.size()) break;

            const int64_t bytes = prefix[hi] - prefix[lo];
            const double kbps = model::BytesToKbps(bytes, window);
            const double center = cursor + window * 0.5;
            series.Add(center, kbps);
            if (is_default) {
                model::BitratePoint point;
                point.timestamp_seconds = center;
                point.window_seconds = window;
                point.bytes = bytes;
                point.frame_count = static_cast<int>(hi - lo);
                point.bitrate_kbps = kbps;
                result_.bitrate_points.push_back(point);
            }
            cursor += hop;
        }
        result_.window_curves.push_back(std::move(series));
    }
}

void BitrateGopAnalyzer::SummarizeBitrate() {
    if (result_.duration_seconds > 0.0) {
        result_.avg_bitrate_kbps = static_cast<double>(result_.total_bytes) * 8.0 / 1000.0 /
                                   result_.duration_seconds;
    }
    if (result_.bitrate_points.empty()) return;

    model::MetricSeries curve;
    curve.name = "bitrate";
    curve.unit = "kbps";
    curve.samples.reserve(result_.bitrate_points.size());
    for (const auto& p : result_.bitrate_points) {
        curve.Add(p.timestamp_seconds, p.bitrate_kbps);
    }
    result_.peak_bitrate_kbps = curve.Max();
    result_.min_bitrate_kbps = curve.Min();
    result_.median_bitrate_kbps = curve.Percentile(0.5);
    result_.p95_bitrate_kbps = curve.Percentile(0.95);

    result_.target_peak_kbps = (options_.target_peak_kbps > 0.0)
                                   ? options_.target_peak_kbps
                                   : result_.avg_bitrate_kbps * options_.auto_peak_ratio;
    result_.peak_to_mean_ratio = (result_.avg_bitrate_kbps > 0.0)
                                     ? result_.peak_bitrate_kbps / result_.avg_bitrate_kbps
                                     : 0.0;

    for (auto& point : result_.bitrate_points) {
        point.overshoot = point.bitrate_kbps > result_.target_peak_kbps;
    }
}

void BitrateGopAnalyzer::DetectGopAnomalies() {
    const int max_anomalies = std::max(options_.max_anomalies_per_type, 1);

    for (const auto& gop : result_.gops) {
        const double duration = gop.DurationSeconds();
        const bool long_by_time = duration > options_.max_gop_seconds;
        const bool long_by_frames = gop.frame_count > options_.max_gop_frames;
        if (!long_by_time && !long_by_frames) continue;

        result_.long_gop_count += 1;
        if (result_.CountAnomalies(BitrateAnomalyType::LongGop) >= max_anomalies) continue;

        BitrateAnomaly anomaly;
        anomaly.type = BitrateAnomalyType::LongGop;
        anomaly.start_seconds = gop.start_seconds;
        anomaly.end_seconds = gop.end_seconds;
        anomaly.gop_index = gop.index;
        if (long_by_time) {
            anomaly.value = duration;
            anomaly.threshold = options_.max_gop_seconds;
        } else {
            anomaly.value = static_cast<double>(gop.frame_count);
            anomaly.threshold = static_cast<double>(options_.max_gop_frames);
        }
        anomaly.detail = "GOP #" + std::to_string(gop.index) + " 时长 " +
                         FormatValue(duration, 2) + " 秒 / " + std::to_string(gop.frame_count) +
                         " 帧（阈值 " + FormatValue(options_.max_gop_seconds, 1) + " 秒 / " +
                         std::to_string(options_.max_gop_frames) + " 帧）。区间 [" +
                         FormatTime(gop.start_seconds) + " - " + FormatTime(gop.end_seconds) + "]";
        anomaly.suggestion =
            "缩短 GOP：设置 -g " + std::to_string(options_.max_gop_frames) + " -keyint_min " +
            std::to_string(options_.max_gop_frames) + "（或按帧率换算为 " +
            FormatValue(options_.max_gop_seconds, 0) + " 秒以内）。";
        result_.anomalies.push_back(std::move(anomaly));
    }

    // 关键帧间隔不均匀（整体统计，只在最偏离的 GOP 上挂一条）
    if (result_.key_interval_irregularity > options_.gop_irregular_ratio &&
        result_.key_interval_mean > 0.0 && !result_.gops.empty()) {
        const model::GopInfo* worst = nullptr;
        double worst_delta = -1.0;
        for (const auto& gop : result_.gops) {
            if (!gop.complete) continue;
            const double delta = std::abs(gop.DurationSeconds() - result_.key_interval_mean);
            if (delta > worst_delta) {
                worst_delta = delta;
                worst = &gop;
            }
        }
        if (worst != nullptr) {
            BitrateAnomaly anomaly;
            anomaly.type = BitrateAnomalyType::IrregularKeyInterval;
            anomaly.start_seconds = worst->start_seconds;
            anomaly.end_seconds = worst->end_seconds;
            anomaly.gop_index = worst->index;
            anomaly.value = result_.key_interval_irregularity;
            anomaly.threshold = options_.gop_irregular_ratio;
            anomaly.detail = "关键帧间隔标准差/均值 = " +
                             FormatValue(result_.key_interval_irregularity) + "（均值 " +
                             FormatValue(result_.key_interval_mean, 2) + " 秒，标准差 " +
                             FormatValue(result_.key_interval_stddev, 2) + " 秒，阈值 " +
                             FormatValue(options_.gop_irregular_ratio, 2) + "）。";
            anomaly.suggestion =
                "若不需要自适应关键帧，关闭场景切换插入：-sc_threshold 0 并固定 -g；"
                "直播/HLS 场景建议固定 GOP 以保证切片等长。";
            result_.anomalies.push_back(std::move(anomaly));
        }
    }
}

void BitrateGopAnalyzer::DetectPeakOvershoot() {
    if (result_.bitrate_points.empty() || result_.target_peak_kbps <= 0.0) return;
    const int max_anomalies = std::max(options_.max_anomalies_per_type, 1);

    // 把连续的 overshoot 采样点合并为一个"峰值区间"，避免逐点刷屏
    std::vector<BitrateAnomaly> runs;
    size_t i = 0;
    while (i < result_.bitrate_points.size()) {
        if (!result_.bitrate_points[i].overshoot) {
            ++i;
            continue;
        }
        size_t j = i;
        double peak = 0.0;
        while (j < result_.bitrate_points.size() && result_.bitrate_points[j].overshoot) {
            peak = std::max(peak, result_.bitrate_points[j].bitrate_kbps);
            result_.bitrate_points[j].peak_region = true;
            ++j;
        }
        const auto& first_point = result_.bitrate_points[i];
        const auto& last_point = result_.bitrate_points[j - 1];
        BitrateAnomaly anomaly;
        anomaly.type = BitrateAnomalyType::PeakOvershoot;
        anomaly.start_seconds = first_point.WindowStart();
        anomaly.end_seconds = last_point.WindowEnd();
        anomaly.value = peak;
        anomaly.threshold = result_.target_peak_kbps;
        anomaly.unit = "kbps";
        anomaly.detail = "区间 [" + FormatTime(anomaly.start_seconds) + " - " +
                         FormatTime(anomaly.end_seconds) + "] 峰值码率 " +
                         FormatValue(peak, 0) + " kbps，超过目标峰值 " +
                         FormatValue(result_.target_peak_kbps, 0) + " kbps（" +
                         FormatValue(result_.target_peak_kbps > 0.0
                                         ? peak / result_.target_peak_kbps : 0.0, 2) +
                         " 倍）。";
        anomaly.suggestion =
            "收紧 VBV（-maxrate / -bufsize）或改用 CBR；若允许波动，请确认 CDN 缓冲与 "
            "首屏策略能吸收该峰值。";
        runs.push_back(std::move(anomaly));
        i = j;
    }

    if (static_cast<int>(runs.size()) > max_anomalies) {
        std::sort(runs.begin(), runs.end(),
                  [](const BitrateAnomaly& a, const BitrateAnomaly& b) {
                      return a.value > b.value;
                  });
        runs.resize(max_anomalies);
        std::sort(runs.begin(), runs.end(),
                  [](const BitrateAnomaly& a, const BitrateAnomaly& b) {
                      return a.start_seconds < b.start_seconds;
                  });
    }
    for (auto& run : runs) result_.anomalies.push_back(std::move(run));
}

void BitrateGopAnalyzer::DetectOversizedFrames() {
    if (samples_.empty()) return;
    const int max_anomalies = std::max(options_.max_anomalies_per_type, 1);
    const double avg_frame = result_.AverageFrameBytes();
    if (avg_frame <= 0.0) return;

    struct Candidate {
        double timestamp_seconds;
        int64_t size;
        double ratio;
    };
    std::vector<Candidate> big_frames;
    std::vector<Candidate> big_i_frames;

    const double avg_i_frame = result_.AverageIFrameBytes();
    for (const auto& sample : samples_) {
        const bool is_intra = (sample.type == model::FrameType::I ||
                               sample.type == model::FrameType::SI);
        if (is_intra && avg_i_frame > 0.0 &&
            static_cast<double>(sample.size) > avg_i_frame * options_.i_frame_oversize_ratio) {
            big_i_frames.push_back({sample.timestamp_seconds, sample.size,
                                    static_cast<double>(sample.size) / avg_i_frame});
        }
        if (static_cast<double>(sample.size) > avg_frame * options_.large_frame_ratio) {
            big_frames.push_back({sample.timestamp_seconds, sample.size,
                                  static_cast<double>(sample.size) / avg_frame});
        }
    }

    auto emit = [this, max_anomalies](std::vector<Candidate>& candidates,
                                      BitrateAnomalyType type, double threshold,
                                      const char* kind_text) {
        if (candidates.empty()) return;
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.size > b.size; });
        if (static_cast<int>(candidates.size()) > max_anomalies) {
            candidates.resize(max_anomalies);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.timestamp_seconds < b.timestamp_seconds;
                  });
        for (const auto& c : candidates) {
            BitrateAnomaly anomaly;
            anomaly.type = type;
            anomaly.start_seconds = c.timestamp_seconds;
            anomaly.end_seconds = c.timestamp_seconds;
            anomaly.value = static_cast<double>(c.size);
            anomaly.threshold = threshold;
            anomaly.detail = std::string(kind_text) + "于 " + FormatTime(c.timestamp_seconds) +
                             " 达到 " + FormatBytes(static_cast<double>(c.size)) + "，是同类的 " +
                             FormatValue(c.ratio, 1) + " 倍。";
            anomaly.suggestion =
                (type == BitrateAnomalyType::OversizedIFrame)
                    ? "降低 I 帧体积：调大 -g 或提高 -qmin/-qp 下限；若用于点播首帧，"
                      "确认该 I 帧不是被强制插入的超大 IDR。"
                    : "检查该时刻是否为场景切换/高动态画面；必要时开启 VBV 限制单帧上限。";
            result_.anomalies.push_back(std::move(anomaly));
        }
    };

    emit(big_frames, BitrateAnomalyType::OversizedFrame,
         avg_frame * options_.large_frame_ratio, "异常大帧出现");
    emit(big_i_frames, BitrateAnomalyType::OversizedIFrame,
         avg_i_frame * options_.i_frame_oversize_ratio, "I 帧过大出现");
}

void BitrateGopAnalyzer::DetectSparseKeyframes() {
    // 整片只有 1 个（或 0 个）关键帧且有一定时长 → 几乎无法 seek
    if (result_.duration_seconds < 30.0) return;
    if (result_.key_frame_seconds.size() > 1) return;
    if (result_.key_frame_seconds.empty()) return;

    BitrateAnomaly anomaly;
    anomaly.type = BitrateAnomalyType::SparseKeyframes;
    anomaly.start_seconds = 0.0;
    anomaly.end_seconds = result_.duration_seconds;
    anomaly.value = static_cast<double>(result_.key_frame_seconds.size());
    anomaly.threshold = 2.0;
    anomaly.unit = "个";
    anomaly.detail = "全片仅 " + std::to_string(result_.key_frame_seconds.size()) +
                     " 个关键帧（时长 " + FormatValue(result_.duration_seconds, 1) +
                     " 秒），拖动/seek 需要解码大量帧。";
    anomaly.suggestion = "按目标 seek 粒度插入关键帧，例如每 2~5 秒一个 IDR（-g / -force_key_frames）。";
    result_.anomalies.push_back(std::move(anomaly));
}

void BitrateGopAnalyzer::MatchSceneChanges(BitrateGopAnalysis& result,
                                           const std::vector<SceneChangeResult>& scene_changes,
                                           const BitrateGopOptions& options) {
    result.scene_matches.clear();
    if (scene_changes.empty()) return;

    std::vector<double> keys = result.key_frame_seconds;
    std::sort(keys.begin(), keys.end());

    const int max_anomalies = std::max(options.max_anomalies_per_type, 1);
    for (const auto& change : scene_changes) {
        if (change.score < options.scene_score_threshold) continue;
        const double distance = NearestDistance(keys, change.timestamp);
        const bool has_key = (distance >= 0.0) && (distance <= options.scene_key_tolerance_seconds);

        BitrateGopAnalysis::SceneKeyMatch match;
        match.timestamp_seconds = change.timestamp;
        match.score = change.score;
        match.has_nearby_keyframe = has_key;
        match.nearest_keyframe_distance = distance;
        result.scene_matches.push_back(match);

        if (has_key) continue;
        if (result.CountAnomalies(BitrateAnomalyType::SceneChangeWithoutKeyframe) >=
            max_anomalies) {
            continue;
        }

        BitrateAnomaly anomaly;
        anomaly.type = BitrateAnomalyType::SceneChangeWithoutKeyframe;
        anomaly.start_seconds = change.timestamp;
        anomaly.end_seconds = change.timestamp;
        anomaly.value = distance;
        anomaly.threshold = options.scene_key_tolerance_seconds;
        anomaly.detail = "场景切换点 " + FormatTime(change.timestamp) + "（强度 " +
                         FormatValue(change.score, 2) + "）附近 " +
                         FormatValue(options.scene_key_tolerance_seconds, 2) +
                         " 秒内没有关键帧" +
                         (distance >= 0.0 ? "（最近的关键帧相距 " + FormatValue(distance, 2) + " 秒）"
                                          : "（文件中无关键帧）") +
                         "。";
        anomaly.suggestion =
            "在场景切换处强制插入关键帧（x264 -sc_threshold 40 或 -force_key_frames），"
            "否则新场景的首批帧只能从旧场景预测，画质与 seek 精度都会受损。";
        result.anomalies.push_back(std::move(anomaly));
    }
}

void BitrateGopAnalyzer::RebuildSuggestions(BitrateGopAnalysis& result,
                                            const BitrateGopOptions& options) {
    result.suggestions.clear();

    const int long_gops = result.CountAnomalies(BitrateAnomalyType::LongGop);
    if (long_gops > 0) {
        result.suggestions.push_back(
            "检测到 " + std::to_string(long_gops) + " 个超长 GOP（> " +
            FormatValue(options.max_gop_seconds, 1) + " 秒 / " +
            std::to_string(options.max_gop_frames) + " 帧）。建议固定 GOP 或使用 " +
            "-force_key_frames 限制最大间隔，可显著改善 seek 与拖动体验。");
    }
    if (result.key_interval_irregularity > options.gop_irregular_ratio) {
        result.suggestions.push_back(
            "关键帧间隔不均匀（标准差/均值 = " + FormatValue(result.key_interval_irregularity, 2) +
            "）。自适应关键帧会让 HLS/DASH 切片时长不一致，直播场景建议关闭 -sc_threshold。");
    }
    if (result.peak_to_mean_ratio > options.auto_peak_ratio) {
        result.suggestions.push_back(
            "峰值/平均码率比为 " + FormatValue(result.peak_to_mean_ratio, 2) + "（峰值 " +
            FormatValue(result.peak_bitrate_kbps, 0) + " kbps，平均 " +
            FormatValue(result.avg_bitrate_kbps, 0) + " kbps）。VBR 波动偏大，"
            "若目标为直播/CDN 分发，建议收紧 VBV（-maxrate/-bufsize）或改用 CBR。");
    }
    const int overshoot = result.CountAnomalies(BitrateAnomalyType::PeakOvershoot);
    if (overshoot > 0) {
        result.suggestions.push_back(
            "有 " + std::to_string(overshoot) + " 段瞬时码率超过目标峰值 " +
            FormatValue(result.target_peak_kbps, 0) +
            " kbps。请确认播放器/CDN 缓冲区能吸收这些突发，否则会出现卡顿。");
    }
    const int scene_missing = result.CountAnomalies(BitrateAnomalyType::SceneChangeWithoutKeyframe);
    if (scene_missing > 0) {
        result.suggestions.push_back(
            "有 " + std::to_string(scene_missing) +
            " 处场景切换点附近没有关键帧。建议开启场景切换自适应关键帧（x264 默认开启；"
            "若被 -sc_threshold 0 关闭请重新评估），保证新镜头从 IDR 开始编码。");
    }
    const int big_i = result.CountAnomalies(BitrateAnomalyType::OversizedIFrame);
    if (big_i > 0) {
        result.suggestions.push_back(
            "有 " + std::to_string(big_i) +
            " 个 I 帧显著大于平均值。大 I 帧会造成首屏与 seek 后的瞬时带宽尖峰，"
            "可通过 -x264opts 的 VBV 或降低 I 帧质量权重来平滑。");
    }
    if (!result.frame_types_known && result.total_frames > 0) {
        result.suggestions.push_back(
            "未获取到逐帧类型（I/P/B），I/P/B 比例仅按关键帧推断。勾选「精确帧类型（解码）」"
            "重新分析可获得完整帧类型分布。");
    }
    if (result.suggestions.empty()) {
        result.suggestions.push_back("码率与 GOP 结构未见明显异常。");
    }
}

const model::MetricSeries* BitrateGopAnalyzer::WindowCurve(double window_seconds) const {
    for (const auto& curve : result_.window_curves) {
        const std::string expected = "bitrate_" + FormatValue(window_seconds, 2) + "s";
        if (curve.name == expected) return &curve;
    }
    return nullptr;
}

}  // namespace analyzer
}  // namespace videoeye
