#include "core/analyzer/VisualDefectAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace videoeye {
namespace analyzer {

namespace {

constexpr double kEpsilon = 1e-9;

double Clamp01(double v) {
    if (!(v == v)) return 0.0;   // NaN
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

double NoValue() { return std::numeric_limits<double>::quiet_NaN(); }

bool Valid(double v) { return std::isfinite(v); }

// 分数方向: true = 越小越严重（模糊、帧差），false = 越大越严重
bool IsReverseScore(model::VisualDefectType type) {
    return type == model::VisualDefectType::Blur ||
           type == model::VisualDefectType::FreezeFrame;
}

double MinSecondsFor(model::VisualDefectType type, const VisualDefectOptions& o) {
    switch (type) {
        case model::VisualDefectType::BlackFrame:       return o.black_min_seconds;
        case model::VisualDefectType::FreezeFrame:      return o.freeze_min_seconds;
        case model::VisualDefectType::Blur:             return o.blur_min_seconds;
        case model::VisualDefectType::Flicker:          return o.flicker_min_seconds;
        case model::VisualDefectType::Blockiness:       return o.blockiness_min_seconds;
        case model::VisualDefectType::OverExposure:
        case model::VisualDefectType::UnderExposure:    return o.exposure_min_seconds;
        case model::VisualDefectType::ColorCast:        return o.color_cast_min_seconds;
        case model::VisualDefectType::InterlaceCombing: return o.combing_min_seconds;
        case model::VisualDefectType::Letterbox:
        case model::VisualDefectType::Pillarbox:        return o.border_min_seconds;
    }
    return 0.3;
}

// 缺陷描述（含触发数值，方便事后复核"为什么算缺陷"）
std::string DescribeDefect(model::VisualDefectType type, double duration,
                           double score, double aux, double threshold) {
    char buf[320];
    switch (type) {
        case model::VisualDefectType::BlackFrame:
            std::snprintf(buf, sizeof(buf),
                          "黑场 %.2fs：黑像素占比 %.1f%%（阈值 %.0f%%），亮度均值 %.1f",
                          duration, score * 100.0, threshold * 100.0, aux);
            break;
        case model::VisualDefectType::FreezeFrame:
            std::snprintf(buf, sizeof(buf),
                          "冻结 %.2fs：帧间差异 %.4f（阈值 %.4f），期间音频非静音",
                          duration, score, threshold);
            break;
        case model::VisualDefectType::Blur:
            std::snprintf(buf, sizeof(buf),
                          "模糊 %.2fs：锐度指数 %.3f（阈值 %.3f，越小越糊）",
                          duration, score, threshold);
            break;
        case model::VisualDefectType::Flicker:
            std::snprintf(buf, sizeof(buf),
                          "闪烁 %.2fs：亮度均值平均跳变 %.1f（阈值 %.1f）",
                          duration, score, threshold);
            break;
        case model::VisualDefectType::Blockiness:
            std::snprintf(buf, sizeof(buf),
                          "花屏/马赛克 %.2fs：块效应强度 %.2f（阈值 %.2f）",
                          duration, score, threshold);
            break;
        case model::VisualDefectType::OverExposure:
            std::snprintf(buf, sizeof(buf),
                          "过曝 %.2fs：高光占比 %.1f%%，削波占比 %.1f%%",
                          duration, score * 100.0, aux * 100.0);
            break;
        case model::VisualDefectType::UnderExposure:
            std::snprintf(buf, sizeof(buf),
                          "欠曝 %.2fs：暗部占比 %.1f%%，亮度均值 %.1f",
                          duration, score * 100.0, aux);
            break;
        case model::VisualDefectType::ColorCast:
            std::snprintf(buf, sizeof(buf),
                          "色偏 %.2fs：偏移强度 %.3f（阈值 %.3f），R-B = %+.1f",
                          duration, score, threshold, aux);
            break;
        case model::VisualDefectType::InterlaceCombing:
            std::snprintf(buf, sizeof(buf),
                          "隔行梳齿 %.2fs：梳齿强度 %.2f（阈值 %.2f）",
                          duration, score, threshold);
            break;
        case model::VisualDefectType::Letterbox:
        case model::VisualDefectType::Pillarbox: {
            // score = 两侧黑边占比之和，aux = 起始侧（上/左）占比
            const double first = Valid(aux) ? aux : score;
            const double second = std::max(0.0, score - first);
            if (type == model::VisualDefectType::Letterbox) {
                std::snprintf(buf, sizeof(buf),
                              "上下黑边：合计 %.1f%%（上 %.1f%% / 下 %.1f%%），有效画面只剩 %.1f%%",
                              score * 100.0, first * 100.0, second * 100.0,
                              (1.0 - score) * 100.0);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "左右黑边：合计 %.1f%%（左 %.1f%% / 右 %.1f%%），有效画面只剩 %.1f%%",
                              score * 100.0, first * 100.0, second * 100.0,
                              (1.0 - score) * 100.0);
            }
            break;
        }
        default:
            std::snprintf(buf, sizeof(buf), "缺陷 %.2fs", duration);
            break;
    }
    return std::string(buf);
}

}  // namespace

// ===========================================================================
// VisualDefectOptions
// ===========================================================================

double VisualDefectOptions::EffectiveSampleFps() const {
    if (sample_fps > 0.0) return sample_fps;
    switch (preset) {
        case VisualSamplingPreset::Fast:        return 1.0;
        case VisualSamplingPreset::Standard:    return 2.0;
        case VisualSamplingPreset::Fine:        return 5.0;
        case VisualSamplingPreset::OfflineFull: return 0.0;   // 每帧
    }
    return 2.0;
}

int VisualDefectOptions::EffectiveAnalysisWidth() const {
    if (analysis_width > 0) return analysis_width;
    switch (preset) {
        case VisualSamplingPreset::Fast:        return 160;
        case VisualSamplingPreset::Standard:    return 256;
        case VisualSamplingPreset::Fine:        return 384;
        case VisualSamplingPreset::OfflineFull: return 256;
    }
    return 256;
}

int VisualDefectOptions::EffectiveEvidenceWidth() const {
    const int w = EffectiveAnalysisWidth() / 2;
    return w < 32 ? 32 : w;
}

// ===========================================================================
// 单帧指标（纯算法，不碰任何内部状态）
// ===========================================================================

model::FrameQualityMetric VisualDefectAnalyzer::ComputeFrameMetrics(
        const model::FrameSample& sample,
        const model::FrameSample* previous,
        double previous_luma_mean,
        const VisualDefectOptions& options) {

    model::FrameQualityMetric m;
    m.frame_index = sample.frame_index;
    m.timestamp_seconds = sample.timestamp_seconds;
    m.sample_width = sample.width;
    m.sample_height = sample.height;

    if (!sample.valid()) {
        m.error_message = "Invalid frame sample";
        return m;
    }

    const int w = sample.width;
    const int h = sample.height;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    const uint8_t* g = sample.gray.data();

    // ---- 亮度统计（一次遍历顺带把各档像素占比数出来）----
    long double sum = 0.0;
    long double sum_sq = 0.0;
    long long cnt_black = 0;
    long long cnt_dark = 0;
    long long cnt_high = 0;
    long long cnt_clip_high = 0;
    long long cnt_clip_low = 0;
    for (size_t i = 0; i < n; ++i) {
        const long double v = static_cast<long double>(g[i]);
        sum += v;
        sum_sq += v * v;
        if (v <= options.black_luma) ++cnt_black;
        if (v <= options.shadow_luma) ++cnt_dark;
        if (v >= options.highlight_luma) ++cnt_high;
        if (v >= options.clip_high_luma) ++cnt_clip_high;
        if (v <= options.clip_low_luma) ++cnt_clip_low;
    }

    const long double mean = sum / static_cast<long double>(n);
    long double variance = sum_sq / static_cast<long double>(n) - mean * mean;
    if (variance < 0.0) variance = 0.0;

    const double inv_n = 1.0 / static_cast<double>(n);
    m.luma_mean = static_cast<double>(mean);
    m.luma_std = std::sqrt(static_cast<double>(variance));
    m.black_ratio = static_cast<double>(cnt_black) * inv_n;
    m.dark_ratio = static_cast<double>(cnt_dark) * inv_n;
    m.highlight_ratio = static_cast<double>(cnt_high) * inv_n;
    m.clip_high_ratio = static_cast<double>(cnt_clip_high) * inv_n;
    m.clip_low_ratio = static_cast<double>(cnt_clip_low) * inv_n;

    // ---- 模糊: 拉普拉斯方差 ----
    // 清晰画面有强边缘 -> 二阶差分大且分布分散（方差大）；
    // 模糊后高频被削掉 -> 二阶差分整体趋近 0，方差塌下去。
    if (options.detect_blur && w >= 3 && h >= 3) {
        long double lap_sum = 0.0;
        long double lap_sq = 0.0;
        long long cnt = 0;
        for (int y = 1; y < h - 1; ++y) {
            const uint8_t* row = g + static_cast<size_t>(y) * w;
            const uint8_t* up = row - w;
            const uint8_t* down = row + w;
            for (int x = 1; x < w - 1; ++x) {
                const int lap = 4 * static_cast<int>(row[x]) - static_cast<int>(row[x - 1]) -
                                static_cast<int>(row[x + 1]) - static_cast<int>(up[x]) -
                                static_cast<int>(down[x]);
                lap_sum += lap;
                lap_sq += static_cast<long double>(lap) * lap;
                ++cnt;
            }
        }
        if (cnt > 0) {
            const long double mu = lap_sum / cnt;
            long double var = lap_sq / cnt - mu * mu;
            if (var < 0.0) var = 0.0;
            m.blur_score = static_cast<double>(var) / 1000.0;
        }
    }

    // ---- 隔行梳齿 ----
    // 逐行相邻的差（跨场，含时间差）远大于隔行相邻（同场）时，说明素材是隔行的。
    // 归一化成 (full - field) / (full + field)：逐行素材这个值 <= 0，隔行有运动时接近 1。
    if (options.detect_combing && h >= 4 && w >= 2) {
        long double full = 0.0;
        long double field = 0.0;
        long long cnt = 0;
        for (int y = 0; y + 2 < h; ++y) {
            const uint8_t* r0 = g + static_cast<size_t>(y) * w;
            const uint8_t* r1 = r0 + w;
            const uint8_t* r2 = r1 + w;
            for (int x = 0; x < w; ++x) {
                full += std::abs(static_cast<int>(r0[x]) - static_cast<int>(r1[x]));
                field += std::abs(static_cast<int>(r0[x]) - static_cast<int>(r2[x]));
            }
            cnt += w;
        }
        if (cnt > 0) {
            const double fd = static_cast<double>(full / cnt);
            const double ff = static_cast<double>(field / cnt);
            const double denom = fd + ff;
            m.combing_score = (denom > 1e-6) ? Clamp01((fd - ff) / denom) : 0.0;
        }
    }

    // ---- 花屏 / 马赛克（块效应）----
    // 块边界处的像素落差明显大于块内部落差时，画面就是被 8x8 块切过的。
    // 内部统计跳过紧邻边界的一列/一行（x%bs == 1），否则会把边界落差算进"内部"。
    if (options.detect_blockiness && w >= 16 && h >= 16) {
        const int bs = options.block_size > 1 ? options.block_size : 8;
        long double boundary = 0.0;
        long double interior = 0.0;
        long long bc = 0;
        long long ic = 0;
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = g + static_cast<size_t>(y) * w;
            for (int x = 1; x < w; ++x) {
                const int d = std::abs(static_cast<int>(row[x]) - static_cast<int>(row[x - 1]));
                const int mod = x % bs;
                if (mod == 0) {
                    boundary += d;
                    ++bc;
                } else if (mod >= 2) {
                    interior += d;
                    ++ic;
                }
            }
        }
        for (int y = 1; y < h; ++y) {
            const uint8_t* r0 = g + static_cast<size_t>(y - 1) * w;
            const uint8_t* r1 = r0 + w;
            const int mod = y % bs;
            for (int x = 0; x < w; ++x) {
                const int d = std::abs(static_cast<int>(r1[x]) - static_cast<int>(r0[x]));
                if (mod == 0) {
                    boundary += d;
                    ++bc;
                } else if (mod >= 2) {
                    interior += d;
                    ++ic;
                }
            }
        }
        if (bc > 0 && ic > 0) {
            const double b = static_cast<double>(boundary / bc);
            const double i = static_cast<double>(interior / ic);
            const double denom = b + i;
            m.blockiness_score = (denom > 1e-6) ? Clamp01((b - i) / denom) : 0.0;
        }
    }

    // ---- 黑边 / 有效画面区域 ----
    if (options.detect_borders && w >= 16 && h >= 16) {
        std::vector<double> row_mean(static_cast<size_t>(h), 0.0);
        std::vector<double> col_mean(static_cast<size_t>(w), 0.0);
        for (int y = 0; y < h; ++y) {
            long double s = 0.0;
            const uint8_t* row = g + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) s += row[x];
            row_mean[static_cast<size_t>(y)] = static_cast<double>(s / w);
        }
        for (int x = 0; x < w; ++x) {
            long double s = 0.0;
            for (int y = 0; y < h; ++y) s += g[static_cast<size_t>(y) * w + x];
            col_mean[static_cast<size_t>(x)] = static_cast<double>(s / h);
        }

        int top = 0;
        while (top < h && row_mean[static_cast<size_t>(top)] <= options.border_luma) ++top;
        int bottom = 0;
        while (bottom < h && row_mean[static_cast<size_t>(h - 1 - bottom)] <= options.border_luma) ++bottom;
        int left = 0;
        while (left < w && col_mean[static_cast<size_t>(left)] <= options.border_luma) ++left;
        int right = 0;
        while (right < w && col_mean[static_cast<size_t>(w - 1 - right)] <= options.border_luma) ++right;

        // 整帧都黑（黑场帧）谈不上黑边；暗场画面被误吃一半以上时也放弃判定
        const bool whole_frame_dark = (top >= h) || (left >= w);
        if (!whole_frame_dark) {
            if (top + bottom >= h / 2) { top = 0; bottom = 0; }
            if (left + right >= w / 2) { left = 0; right = 0; }
            model::ActivePictureArea area;
            area.x = left;
            area.y = top;
            area.width = w - left - right;
            area.height = h - top - bottom;
            if (area.width > 0 && area.height > 0) {
                area.valid = true;
                area.top_bar_ratio = static_cast<double>(top) / h;
                area.bottom_bar_ratio = static_cast<double>(bottom) / h;
                area.left_bar_ratio = static_cast<double>(left) / w;
                area.right_bar_ratio = static_cast<double>(right) / w;
                area.letterbox = (top > 0 && bottom > 0 &&
                                  (area.top_bar_ratio + area.bottom_bar_ratio) >= options.border_bar_ratio);
                area.pillarbox = (left > 0 && right > 0 &&
                                  (area.left_bar_ratio + area.right_bar_ratio) >= options.border_bar_ratio);
                area.active_ratio = static_cast<double>(area.width) * area.height /
                                    (static_cast<double>(w) * h);
                m.active_area = area;
            }
        }
    }

    // ---- 色偏 ----
    if (options.detect_color_cast && sample.has_rgb()) {
        const size_t pixels = static_cast<size_t>(sample.rgb_width) * sample.rgb_height;
        long double sr = 0.0;
        long double sg = 0.0;
        long double sb = 0.0;
        const uint8_t* rgb = sample.rgb.data();
        for (size_t i = 0; i < pixels; ++i) {
            sr += rgb[i * 3 + 0];
            sg += rgb[i * 3 + 1];
            sb += rgb[i * 3 + 2];
        }
        const double mr = static_cast<double>(sr / pixels);
        const double mg = static_cast<double>(sg / pixels);
        const double mb = static_cast<double>(sb / pixels);
        const double my = 0.299 * mr + 0.587 * mg + 0.114 * mb;
        const double dr = mr - my;
        const double db = mb - my;
        m.color_cast_score = Clamp01(std::sqrt(dr * dr + db * db) / 255.0);
        m.color_cast_rb = mr - mb;
    }

    // ---- 时域: 与上一采样帧的差异 ----
    if (previous && previous->valid() && previous->width == w && previous->height == h) {
        long double mad = 0.0;
        const uint8_t* p = previous->gray.data();
        for (size_t i = 0; i < n; ++i) {
            mad += std::abs(static_cast<int>(g[i]) - static_cast<int>(p[i]));
        }
        m.frame_diff = static_cast<double>(mad / n) / 255.0;
        if (Valid(previous_luma_mean)) {
            m.luma_delta = std::abs(m.luma_mean - previous_luma_mean);
        }
    }

    m.valid = true;
    return m;
}

// ===========================================================================
// 生命周期
// ===========================================================================

VisualDefectAnalyzer::VisualDefectAnalyzer() = default;

VisualDefectAnalyzer::~VisualDefectAnalyzer() { StopWorker(); }

void VisualDefectAnalyzer::Reset(const VisualDefectOptions& options) {
    StopWorker();
    std::lock_guard<std::mutex> lock(m_);
    options_ = options;
    metrics_.clear();
    defects_.clear();
    pending_metrics_.clear();
    pending_defects_.clear();
    active_areas_.clear();
    segments_.clear();
    luma_history_.clear();
    luma_history_ts_.clear();
    has_prev_ = false;
    prev_sample_ = model::FrameSample();
    prev_luma_mean_ = NoValue();
    next_defect_id_ = 1;
    dropped_samples_.store(0);
    analyzed_samples_.store(0);
}

void VisualDefectAnalyzer::StartWorker(size_t queue_capacity) {
    StopWorker();
    {
        std::lock_guard<std::mutex> lock(m_);
        queue_capacity_ = (queue_capacity == 0) ? 1 : queue_capacity;
        queue_.clear();
    }
    worker_running_.store(true);
    worker_ = std::thread(&VisualDefectAnalyzer::WorkerLoop, this);
}

void VisualDefectAnalyzer::StopWorker() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_);
            worker_running_.store(false);
        }
        cv_.notify_all();
        worker_.join();
    } else {
        worker_running_.store(false);
    }
    std::lock_guard<std::mutex> lock(m_);
    queue_.clear();
}

bool VisualDefectAnalyzer::Submit(const model::FrameSample& sample) {
    if (!worker_running_.load()) return false;
    std::unique_lock<std::mutex> lock(m_);
    if (queue_.size() >= queue_capacity_) {
        // 播放压力大时宁可丢分析帧，也不能反压解码线程
        dropped_samples_.fetch_add(1);
        return false;
    }
    queue_.push_back(sample);
    lock.unlock();
    cv_.notify_one();
    return true;
}

void VisualDefectAnalyzer::WorkerLoop() {
    for (;;) {
        model::FrameSample sample;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return !queue_.empty() || !worker_running_.load(); });
            if (queue_.empty()) {
                if (!worker_running_.load()) return;
                continue;
            }
            sample = std::move(queue_.front());
            queue_.pop_front();
        }
        std::lock_guard<std::mutex> lock(m_);
        ProcessSampleLocked(sample);
    }
}

void VisualDefectAnalyzer::Feed(const model::FrameSample& sample) {
    std::lock_guard<std::mutex> lock(m_);
    ProcessSampleLocked(sample);
}

void VisualDefectAnalyzer::Flush(double end_timestamp_seconds) {
    std::lock_guard<std::mutex> lock(m_);
    CloseAllSegmentsLocked(end_timestamp_seconds, -1);
}

std::vector<model::FrameQualityMetric> VisualDefectAnalyzer::TakePendingMetrics() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<model::FrameQualityMetric> out;
    out.swap(pending_metrics_);
    return out;
}

std::vector<model::VisualDefect> VisualDefectAnalyzer::TakePendingDefects() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<model::VisualDefect> out;
    out.swap(pending_defects_);
    return out;
}

model::ActivePictureArea VisualDefectAnalyzer::EffectiveArea() const {
    std::vector<int> xs;
    std::vector<int> ys;
    std::vector<int> ws;
    std::vector<int> hs;
    std::vector<double> ratios;
    {
        std::lock_guard<std::mutex> lock(m_);
        xs.reserve(active_areas_.size());
        ys.reserve(active_areas_.size());
        ws.reserve(active_areas_.size());
        hs.reserve(active_areas_.size());
        ratios.reserve(active_areas_.size());
        for (const auto& a : active_areas_) {
            if (!a.valid) continue;
            xs.push_back(a.x);
            ys.push_back(a.y);
            ws.push_back(a.width);
            hs.push_back(a.height);
            ratios.push_back(a.active_ratio);
        }
    }
    model::ActivePictureArea area;
    if (xs.empty()) return area;

    auto median_int = [](std::vector<int>& v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::sort(ratios.begin(), ratios.end());
    area.valid = true;
    area.x = median_int(xs);
    area.y = median_int(ys);
    area.width = median_int(ws);
    area.height = median_int(hs);
    area.active_ratio = ratios[ratios.size() / 2];
    area.letterbox = area.y > 0;
    area.pillarbox = area.x > 0;
    return area;
}

model::VisualDefectReport VisualDefectAnalyzer::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_);
    model::VisualDefectReport report;
    report.defects = defects_;
    report.samples = metrics_;
    report.analyzed = analyzed_samples_.load() > 0;
    report.analyzed_frames = analyzed_samples_.load();
    report.dropped_frames = dropped_samples_.load();
    if (!metrics_.empty()) {
        report.duration_seconds = metrics_.back().timestamp_seconds - metrics_.front().timestamp_seconds;
    }
    return report;
}

// ===========================================================================
// 逐帧处理
// ===========================================================================

void VisualDefectAnalyzer::ProcessSampleLocked(const model::FrameSample& sample) {
    if (!sample.valid()) return;

    model::FrameQualityMetric m = ComputeFrameMetrics(
        sample, has_prev_ ? &prev_sample_ : nullptr, prev_luma_mean_, options_);
    if (!m.valid) return;

    metrics_.push_back(m);
    pending_metrics_.push_back(m);
    if (metrics_.size() > options_.max_samples) {
        metrics_.erase(metrics_.begin(),
                       metrics_.begin() + static_cast<long>(metrics_.size() - options_.max_samples));
    }
    if (m.active_area.valid) active_areas_.push_back(m.active_area);
    analyzed_samples_.fetch_add(1);

    const double ts = m.timestamp_seconds;
    const int frame = m.frame_index;

    // 闪烁窗口要用到历史亮度，先更新再判定
    luma_history_.push_back(m.luma_mean);
    luma_history_ts_.push_back(ts);
    while (luma_history_.size() > 8) {
        luma_history_.pop_front();
        luma_history_ts_.pop_front();
    }

    const bool black_hit =
        options_.detect_black && Valid(m.black_ratio) &&
        m.black_ratio >= options_.black_ratio &&
        m.luma_mean <= options_.black_luma_mean;

    const bool freeze_hit =
        options_.detect_freeze && has_prev_ && Valid(m.frame_diff) &&
        m.frame_diff <= options_.freeze_diff &&
        !sample.audio_silent && !black_hit;

    // 模糊判定要排除"画面本来就没有内容"的帧: 纯色/黑场的拉普拉斯方差天然接近 0，
    // 那是"没细节"不是"糊了"，交给黑场/欠曝去报。
    const bool blur_hit =
        options_.detect_blur && Valid(m.blur_score) &&
        m.blur_score <= options_.blur_threshold &&
        m.black_ratio < 0.5 &&
        m.luma_std >= options_.blur_min_luma_std;

    double flicker_amplitude = 0.0;
    const bool flicker_hit = options_.detect_flicker &&
                             FlickerHitLocked(ts, &flicker_amplitude) && !black_hit;

    const bool over_hit =
        options_.detect_exposure && Valid(m.highlight_ratio) &&
        (m.clip_high_ratio >= options_.over_exposure_clip_ratio ||
         (m.highlight_ratio >= options_.over_exposure_highlight_ratio &&
          m.clip_high_ratio >= 0.02));

    const bool under_hit =
        options_.detect_exposure && !black_hit && Valid(m.dark_ratio) &&
        (m.dark_ratio >= options_.under_exposure_dark_ratio ||
         m.luma_mean <= options_.under_exposure_luma_mean);

    const bool cast_hit =
        options_.detect_color_cast && Valid(m.color_cast_score) &&
        m.color_cast_score >= options_.color_cast_score;

    const bool combing_hit =
        options_.detect_combing && Valid(m.combing_score) &&
        m.combing_score >= options_.combing_score &&
        (!Valid(m.frame_diff) || m.frame_diff >= options_.combing_motion_gate);

    SegmentUpdate u;
    u.ts = ts;
    u.frame = frame;

    if (options_.detect_black) {
        u.type = model::VisualDefectType::BlackFrame;
        u.hit = black_hit;
        u.score = m.black_ratio;
        u.aux = m.luma_mean;
        u.threshold = options_.black_ratio;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_freeze) {
        u.type = model::VisualDefectType::FreezeFrame;
        u.hit = freeze_hit;
        u.score = m.frame_diff;
        u.aux = NoValue();
        u.threshold = options_.freeze_diff;
        u.reverse = true;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_blur) {
        u.type = model::VisualDefectType::Blur;
        u.hit = blur_hit;
        u.score = m.blur_score;
        u.aux = NoValue();
        u.threshold = options_.blur_threshold;
        u.reverse = true;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_flicker) {
        u.type = model::VisualDefectType::Flicker;
        u.hit = flicker_hit;
        u.score = flicker_amplitude;
        u.aux = NoValue();
        u.threshold = options_.flicker_delta;
        u.reverse = false;
        u.start_ts = luma_history_ts_.empty() ? ts : luma_history_ts_.front();
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_exposure) {
        u.type = model::VisualDefectType::OverExposure;
        u.hit = over_hit;
        u.score = m.highlight_ratio;
        u.aux = m.clip_high_ratio;
        u.threshold = options_.over_exposure_highlight_ratio;
        u.reverse = false;
        u.start_ts = NoValue();
        UpdateSegmentLocked(u, sample);

        u.type = model::VisualDefectType::UnderExposure;
        u.hit = under_hit;
        u.score = m.dark_ratio;
        u.aux = m.luma_mean;
        u.threshold = options_.under_exposure_dark_ratio;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_color_cast) {
        u.type = model::VisualDefectType::ColorCast;
        u.hit = cast_hit;
        u.score = m.color_cast_score;
        u.aux = m.color_cast_rb;
        u.threshold = options_.color_cast_score;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_combing) {
        u.type = model::VisualDefectType::InterlaceCombing;
        u.hit = combing_hit;
        u.score = m.combing_score;
        u.aux = NoValue();
        u.threshold = options_.combing_score;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);
    }
    if (options_.detect_borders && m.active_area.valid) {
        u.type = model::VisualDefectType::Letterbox;
        u.hit = m.active_area.letterbox;
        u.score = m.active_area.top_bar_ratio + m.active_area.bottom_bar_ratio;
        u.aux = m.active_area.top_bar_ratio;
        u.threshold = options_.border_bar_ratio;
        u.reverse = false;
        UpdateSegmentLocked(u, sample);

        u.type = model::VisualDefectType::Pillarbox;
        u.hit = m.active_area.pillarbox;
        u.score = m.active_area.left_bar_ratio + m.active_area.right_bar_ratio;
        u.aux = m.active_area.left_bar_ratio;
        u.threshold = options_.border_bar_ratio;
        UpdateSegmentLocked(u, sample);
    }

    // 时域状态推进（赋值的 gray 是小图，拷贝成本可接受）
    prev_sample_ = sample;
    prev_luma_mean_ = m.luma_mean;
    has_prev_ = true;
}

void VisualDefectAnalyzer::UpdateSegmentLocked(const SegmentUpdate& u,
                                               const model::FrameSample& sample) {
    SegmentState& st = segments_[u.type];
    if (u.hit) {
        if (!st.active) {
            st.active = true;
            st.start_ts = Valid(u.start_ts) ? u.start_ts : u.ts;
            st.start_frame = u.frame;
            st.worst = u.score;
            st.sum = u.score;
            st.count = 1;
            st.aux = u.aux;
            st.threshold = u.threshold;
            if (sample.has_rgb()) {
                st.has_evidence = true;
                st.evidence.width = sample.rgb_width;
                st.evidence.height = sample.rgb_height;
                st.evidence.rgb = sample.rgb;
            } else {
                st.has_evidence = false;
                st.evidence = model::EvidenceFrame();
            }
        } else {
            st.worst = u.reverse ? std::min(st.worst, u.score) : std::max(st.worst, u.score);
            st.sum += u.score;
            ++st.count;
            // 辅助数值取"更极端"的那个方向：曝光取更大，色偏取绝对值更大
            if (Valid(u.aux) && Valid(st.aux)) {
                st.aux = (std::abs(u.aux) > std::abs(st.aux)) ? u.aux : st.aux;
            } else if (Valid(u.aux)) {
                st.aux = u.aux;
            }
        }
        st.last_ts = u.ts;
        st.last_frame = u.frame;
    } else if (st.active) {
        CloseSegmentLocked(u.type, u.ts, u.frame);
    }
}

void VisualDefectAnalyzer::CloseSegmentLocked(model::VisualDefectType type, double end_ts,
                                              int end_frame) {
    auto it = segments_.find(type);
    if (it == segments_.end() || !it->second.active) return;

    const SegmentState st = it->second;
    it->second = SegmentState();

    const double duration = std::max(0.0, st.last_ts - st.start_ts);
    if (duration + kEpsilon < MinSecondsFor(type, options_)) return;

    // 与上一条同类缺陷挨得太近就合并，避免同一段问题被切成好几条
    if (!defects_.empty()) {
        model::VisualDefect& last = defects_.back();
        if (last.type == type &&
            (st.start_ts - last.end_seconds) <= options_.MergeGapSeconds()) {
            last.end_seconds = st.last_ts;
            last.end_frame = st.last_frame;
            const bool reverse = IsReverseScore(type);
            last.score = reverse ? std::min(last.score, st.worst) : std::max(last.score, st.worst);
            const double total = std::max(0.0, last.end_seconds - last.start_seconds);
            const auto merged = SeverityFor(type, total, last.score, options_);
            if (static_cast<int>(merged) > static_cast<int>(last.severity)) last.severity = merged;
            last.description = DescribeDefect(type, total, last.score, st.aux, st.threshold);
            return;
        }
    }

    model::VisualDefect defect;
    defect.id = next_defect_id_++;
    defect.type = type;
    defect.severity = SeverityFor(type, duration, st.worst, options_);
    defect.start_seconds = st.start_ts;
    defect.end_seconds = st.last_ts;
    defect.start_frame = st.start_frame;
    defect.end_frame = st.last_frame;
    defect.score = st.worst;
    defect.threshold = st.threshold;
    defect.description = DescribeDefect(type, duration, st.worst, st.aux, st.threshold);
    if (st.has_evidence) defect.evidence = st.evidence;
    PushDefectLocked(std::move(defect));
}

void VisualDefectAnalyzer::CloseAllSegmentsLocked(double end_ts, int end_frame) {
    // 先拷贝一份 key，CloseSegmentLocked 会改 segments_
    std::vector<model::VisualDefectType> types;
    types.reserve(segments_.size());
    for (const auto& kv : segments_) {
        if (kv.second.active) types.push_back(kv.first);
    }
    for (const auto& type : types) {
        CloseSegmentLocked(type, end_ts, end_frame);
    }
}

void VisualDefectAnalyzer::PushDefectLocked(model::VisualDefect defect) {
    if (defects_.size() >= options_.max_defects) return;
    pending_defects_.push_back(defect);
    defects_.push_back(std::move(defect));
}

bool VisualDefectAnalyzer::FlickerHitLocked(double ts, double* amplitude) const {
    (void)ts;
    if (luma_history_.size() < 4) return false;

    double sum_abs = 0.0;
    int pairs = 0;
    int alternations = 0;
    for (size_t i = 1; i < luma_history_.size(); ++i) {
        const double d = luma_history_[i] - luma_history_[i - 1];
        sum_abs += std::abs(d);
        ++pairs;
        if (i >= 2) {
            const double prev_d = luma_history_[i - 1] - luma_history_[i - 2];
            // 亮->暗->亮 才算闪烁；单向缓慢变化（淡入淡出）不算
            if (d * prev_d < 0.0 &&
                std::abs(d) >= options_.flicker_delta &&
                std::abs(prev_d) >= options_.flicker_delta) {
                ++alternations;
            }
        }
    }
    const double mean_abs = (pairs > 0) ? (sum_abs / pairs) : 0.0;
    if (amplitude) *amplitude = mean_abs;
    return alternations >= options_.flicker_min_alternations &&
           mean_abs >= options_.flicker_delta;
}

model::VisualDefectSeverity VisualDefectAnalyzer::SeverityFor(model::VisualDefectType type,
                                                              double duration_seconds,
                                                              double score,
                                                              const VisualDefectOptions& options) {
    using model::VisualDefectSeverity;
    using model::VisualDefectType;
    switch (type) {
        case VisualDefectType::BlackFrame:
            if (duration_seconds >= 3.0) return VisualDefectSeverity::Error;
            if (duration_seconds >= 1.0) return VisualDefectSeverity::Warning;
            return VisualDefectSeverity::Info;
        case VisualDefectType::FreezeFrame:
            if (duration_seconds >= 3.0) return VisualDefectSeverity::Error;
            return VisualDefectSeverity::Warning;
        case VisualDefectType::Blur:
            // 越低于阈值越离谱
            if (score <= options.blur_threshold * 0.25) return VisualDefectSeverity::Warning;
            return VisualDefectSeverity::Info;
        case VisualDefectType::Flicker:
            if (score >= options.flicker_delta * 3.0) return VisualDefectSeverity::Error;
            return VisualDefectSeverity::Warning;
        case VisualDefectType::Blockiness:
            if (score >= 0.6) return VisualDefectSeverity::Error;
            return VisualDefectSeverity::Warning;
        case VisualDefectType::OverExposure:
        case VisualDefectType::UnderExposure:
            if (score >= 0.9) return VisualDefectSeverity::Error;
            return VisualDefectSeverity::Warning;
        case VisualDefectType::ColorCast:
            if (score >= options.color_cast_score * 2.0) return VisualDefectSeverity::Warning;
            return VisualDefectSeverity::Info;
        case VisualDefectType::InterlaceCombing:
            if (score >= 0.6) return VisualDefectSeverity::Error;
            return VisualDefectSeverity::Warning;
        case VisualDefectType::Letterbox:
        case VisualDefectType::Pillarbox:
            // 有黑边本身不是错，只是要让人知道有效画面区域在哪
            return VisualDefectSeverity::Info;
    }
    return VisualDefectSeverity::Info;
}

}  // namespace analyzer
}  // namespace videoeye
