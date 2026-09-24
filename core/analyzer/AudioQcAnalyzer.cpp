#include "core/analyzer/AudioQcAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

namespace videoeye {
namespace analyzer {
namespace {

constexpr double kPi = 3.14159265358979323846;

// BS.1770-4 时间常数
constexpr double kBlockSeconds = 0.4;    // 400 ms 块
constexpr double kHopSeconds = 0.1;      // 75% 重叠 -> 每 100 ms 一个块
constexpr double kShortTermSeconds = 3.0;

// 真峰值过采样系数（BS.1770-4 要求至少 4×）
constexpr int kOversampleFactor = 4;

std::string Fixed(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ("%." + std::to_string(decimals) + "f").c_str(), value);
    return std::string(buf);
}

// K 加权 stage 1: 高频搁架（spherical head diffraction）
// 参数与 FFmpeg ebur128.c / libebur128 完全一致（含原标准里的量化偏置）
BiquadFilter MakeKWeightingShelf(int sample_rate) {
    const double fs = static_cast<double>(sample_rate);
    const double f0 = 1681.974450955533;
    const double g_db = 3.999843853973347;
    const double q = 0.7071752369554196;

    const double k = std::tan(kPi * f0 / fs);
    const double vh = std::pow(10.0, g_db / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;

    BiquadFilter f;
    f.b0 = (vh + vb * k / q + k * k) / a0;
    f.b1 = 2.0 * (k * k - vh) / a0;
    f.b2 = (vh - vb * k / q + k * k) / a0;
    f.a1 = 2.0 * (k * k - 1.0) / a0;
    f.a2 = (1.0 - k / q + k * k) / a0;
    return f;
}

// K 加权 stage 2: RLB 高通
BiquadFilter MakeKWeightingRlb(int sample_rate) {
    const double fs = static_cast<double>(sample_rate);
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;

    const double k = std::tan(kPi * f0 / fs);
    const double denom = 1.0 + k / q + k * k;

    BiquadFilter f;
    f.b0 = 1.0;
    f.b1 = -2.0;
    f.b2 = 1.0;
    f.a1 = 2.0 * (k * k - 1.0) / denom;
    f.a2 = (1.0 - k / q + k * k) / denom;
    return f;
}

// 4× 过采样插值核: Hann 窗 sinc，截止频率 = 原始奈奎斯特（与 libebur128 同一设计）
std::vector<double> MakeInterpolationKernel(int sample_rate) {
    const int factor = kOversampleFactor;
    int taps = 48;   // 48 kHz -> 48 抽头（每相位 12）；采样率每翻倍抽头翻倍
    while (taps < 192 && sample_rate > (taps / 48) * 48000) taps *= 2;

    std::vector<double> h(taps, 0.0);
    for (int j = 0; j < taps; ++j) {
        const double m = static_cast<double>(j) - (static_cast<double>(taps) - 1.0) / 2.0;
        double c = 1.0;
        if (std::abs(m) > 1e-12) {
            c = std::sin(m * kPi / factor) / (m * kPi / factor);
        }
        c *= 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(j) /
                                   static_cast<double>(taps - 1)));
        h[j] = c;
    }
    return h;
}

}  // namespace

// ---------------------------------------------------------------- 生命周期

void AudioQcAnalyzer::Reset(const AudioQcOptions& options) {
    options_ = options;
    result_ = model::AudioQcResult{};
    channels_.clear();
    channel_info_.clear();
    block_powers_.clear();
    short_term_window_.clear();
    short_term_values_.clear();
    short_term_pos_ = 0;
    short_term_sum_ = 0.0;
    sample_rate_ = 0;
    channel_count_ = 0;
    block_samples_ = 0;
    hop_samples_ = 0;
    ring_size_ = 0;
    ring_write_ = 0;
    sample_index_ = 0;
    clock_seconds_ = 0.0;
    running_sample_peak_ = 0.0;
    running_true_peak_ = 0.0;
    correlation_sum_ = 0.0;
    correlation_min_ = 1.0;
    correlation_blocks_ = 0;
    out_of_phase_blocks_ = 0;
    blocks_emitted_ = 0;
    point_stride_ = 1;
    momentary_max_ = model::kSilenceLufs;
    short_term_max_ = model::kSilenceLufs;
    short_term_min_ = 100.0;   // 未取到值时的哨兵（LUFS 不可能为正 100）
    abs_gated_sum_ = 0.0;
    abs_gated_count_ = 0;
    rel_gated_sum_ = 0.0;
    rel_gated_count_ = 0;
    silence_open_ = false;
    silence_start_ = 0.0;
    silence_acc_dbfs_ = 0.0;
    silence_acc_blocks_ = 0;
    stream_info_set_ = false;
    filters_ready_ = false;
    finished_ = false;
}

void AudioQcAnalyzer::SetOptions(const AudioQcOptions& options) { options_ = options; }

// ---------------------------------------------------------------- 元数据

void AudioQcAnalyzer::SetStreamInfo(int sample_rate, int channels,
                                    const std::string& sample_format, int bits_per_sample) {
    sample_rate_ = sample_rate;
    channel_count_ = channels;
    result_.metadata.sample_rate = sample_rate;
    result_.metadata.channels = channels;
    result_.metadata.sample_format = sample_format;
    result_.metadata.bits_per_sample = bits_per_sample;
    result_.has_audio = (channels > 0 && sample_rate > 0);

    if (sample_rate <= 0 || channels <= 0) return;

    block_samples_ = std::max(1, static_cast<int>(std::llround(kBlockSeconds * sample_rate)));
    hop_samples_ = std::max(1, static_cast<int>(std::llround(kHopSeconds * sample_rate)));

    // 插值核 -> 多相分解（4 个相位）+ 历史余量
    interp_coeffs_ = MakeInterpolationKernel(sample_rate);
    double abs_sum = 0.0;
    for (double c : interp_coeffs_) abs_sum += std::abs(c);
    interp_abs_sum_ = std::max(abs_sum, 1e-9);
    for (int p = 0; p < kOversampleFactor; ++p) interp_phase_[p].clear();
    for (size_t j = 0; j < interp_coeffs_.size(); ++j) {
        interp_phase_[j % kOversampleFactor].push_back(interp_coeffs_[j]);
    }
    interp_margin_ = 0;
    for (int p = 0; p < kOversampleFactor; ++p) {
        interp_margin_ = std::max(interp_margin_, static_cast<int>(interp_phase_[p].size()));
    }

    ring_size_ = block_samples_ + interp_margin_ + 1;
    scratch_.assign(static_cast<size_t>(block_samples_ + interp_margin_), 0.0f);
    channels_.assign(static_cast<size_t>(channels), ChannelState{});
    for (auto& ch : channels_) {
        ch.raw_ring.assign(static_cast<size_t>(ring_size_), 0.0f);
        ch.kw_ring.assign(static_cast<size_t>(ring_size_), 0.0f);
    }
    PrepareFilters();
    stream_info_set_ = true;
}

void AudioQcAnalyzer::SetChannelInfo(const std::vector<model::AudioChannelInfo>& channels) {
    channel_info_ = channels;
    if (channels.empty()) return;

    result_.channels.clear();
    result_.channels.reserve(channels.size());
    std::vector<model::AudioChannelRole> roles;
    roles.reserve(channels.size());
    for (size_t i = 0; i < channels.size(); ++i) {
        model::AudioChannelStat stat;
        stat.index = static_cast<int>(i);
        stat.name = channels[i].name;
        stat.role = channels[i].role;
        stat.weight = ChannelWeight(channels[i].role);
        result_.channels.push_back(stat);
        roles.push_back(channels[i].role);
    }
    result_.metadata.channels = static_cast<int>(channels.size());
    result_.metadata.layout_confirmed =
        std::none_of(roles.begin(), roles.end(), [](model::AudioChannelRole r) {
            return r == model::AudioChannelRole::Unknown;
        });
    result_.metadata.channel_layout = DescribeLayout(static_cast<int>(channels.size()), roles);
}

void AudioQcAnalyzer::SetDurations(double stream_seconds, double container_seconds,
                                   double video_seconds, bool has_video) {
    result_.metadata.stream_duration_seconds = stream_seconds;
    result_.metadata.container_duration_seconds = container_seconds;
    result_.metadata.video_duration_seconds = video_seconds;
    result_.metadata.has_video = has_video;
    result_.metadata.container_delta_seconds = stream_seconds - container_seconds;
    result_.metadata.video_delta_seconds = has_video ? (stream_seconds - video_seconds) : 0.0;
}

double AudioQcAnalyzer::ChannelWeight(model::AudioChannelRole role) {
    switch (role) {
        case model::AudioChannelRole::Surround:     return 1.41;
        case model::AudioChannelRole::LowFrequency: return 0.0;
        default:                                    return 1.0;
    }
}

std::string AudioQcAnalyzer::DescribeLayout(int channels,
                                            const std::vector<model::AudioChannelRole>& roles) {
    if (channels <= 0) return "未知";
    if (roles.size() != static_cast<size_t>(channels)) {
        switch (channels) {
            case 1: return "单声道 (1.0)";
            case 2: return "立体声 (2.0)";
            default: return std::to_string(channels) + " 声道";
        }
    }
    int front = 0, surround = 0, lfe = 0;
    for (auto r : roles) {
        if (r == model::AudioChannelRole::LowFrequency) ++lfe;
        else if (r == model::AudioChannelRole::Surround) ++surround;
        else ++front;
    }
    if (channels == 1) return "单声道 (1.0)";
    if (channels == 2) return "立体声 (2.0)";
    if (front == 3 && surround == 2 && lfe == 1) return "5.1 (6ch)";
    if (front == 3 && surround == 2 && lfe == 0) return "5.0 (5ch)";
    if (front == 3 && surround == 4 && lfe == 1) return "7.1 (8ch)";
    if (front == 3 && surround == 0 && lfe == 1) return "3.1 (4ch)";
    if (front == 3 && surround == 0 && lfe == 0) return "3.0 (3ch)";
    if (front == 2 && surround == 2 && lfe == 0) return "四声道 (4.0)";
    return std::to_string(channels) + " 声道";
}

// ---------------------------------------------------------------- 输入

void AudioQcAnalyzer::PrepareFilters() {
    if (sample_rate_ <= 0) return;
    for (auto& ch : channels_) {
        ch.shelf = MakeKWeightingShelf(sample_rate_);
        ch.rlb = MakeKWeightingRlb(sample_rate_);
    }
    filters_ready_ = true;
}

void AudioQcAnalyzer::EnsureReady() {
    if (!filters_ready_ && sample_rate_ > 0) PrepareFilters();
}

void AudioQcAnalyzer::OnSamples(const float* const* planar, int frames, double timestamp_seconds) {
    if (!stream_info_set_ || frames <= 0 || finished_ || planar == nullptr) return;
    EnsureReady();
    if (channels_.empty()) return;

    if (timestamp_seconds >= 0.0) clock_seconds_ = timestamp_seconds;
    const double dt = 1.0 / static_cast<double>(sample_rate_);

    for (int i = 0; i < frames; ++i) {
        for (int ch = 0; ch < channel_count_; ++ch) {
            const float* src = planar[ch];
            const float sample = (src != nullptr) ? src[i] : 0.0f;
            PushSample(ch, sample, clock_seconds_);
            channels_[ch].kw_ring[static_cast<size_t>(ring_write_)] =
                channels_[ch].rlb.Process(channels_[ch].shelf.Process(sample));
        }
        ring_write_ = (ring_write_ + 1) % ring_size_;
        ++sample_index_;
        clock_seconds_ += dt;

        if (sample_index_ >= block_samples_ &&
            (sample_index_ - block_samples_) % hop_samples_ == 0) {
            ProcessBlock(clock_seconds_);
        }
    }
    result_.analyzed = true;
}

void AudioQcAnalyzer::OnSamplesInterleaved(const float* data, int frames,
                                           double timestamp_seconds) {
    if (!stream_info_set_ || data == nullptr || frames <= 0) return;
    std::vector<std::vector<float>> planes(static_cast<size_t>(channel_count_));
    std::vector<const float*> ptrs(static_cast<size_t>(channel_count_));
    for (int ch = 0; ch < channel_count_; ++ch) {
        planes[static_cast<size_t>(ch)].resize(static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            planes[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
                data[static_cast<size_t>(i) * channel_count_ + ch];
        }
        ptrs[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
    }
    OnSamples(ptrs.data(), frames, timestamp_seconds);
}

void AudioQcAnalyzer::PushSample(int ch, float sample, double seconds) {
    ChannelState& state = channels_[static_cast<size_t>(ch)];
    state.raw_ring[static_cast<size_t>(ring_write_)] = sample;

    const double d = static_cast<double>(sample);
    const double a = std::abs(d);
    state.sum += d;
    state.sum_sq += d * d;
    if (a > state.peak) state.peak = a;
    ++state.samples;
    if (a > running_sample_peak_) running_sample_peak_ = a;

    if (a < options_.clip_threshold) return;

    ++state.clip_samples;
    ++result_.clipping_sample_count;
    if (state.clip_open && seconds - state.clip_last > options_.clip_merge_gap_seconds) {
        CloseClipEvent(ch, state.clip_last);   // 间隔过大 -> 先收尾上一段
    }
    if (!state.clip_open) {
        state.clip_open = true;
        state.clip_start = seconds;
        state.clip_count = 0;
        state.clip_peak = 0.0;
    }
    state.clip_last = seconds;
    ++state.clip_count;
    if (a > state.clip_peak) state.clip_peak = a;
}

void AudioQcAnalyzer::CloseClipEvent(int ch, double seconds) {
    ChannelState& state = channels_[static_cast<size_t>(ch)];
    if (!state.clip_open || state.clip_count == 0) {
        state.clip_open = false;
        return;
    }
    ++result_.clipping_event_count;
    if (static_cast<int>(result_.clipping_events.size()) < options_.max_clip_events) {
        model::AudioClipEvent event;
        event.start_seconds = state.clip_start;
        event.end_seconds = std::max(seconds, state.clip_start);
        event.channel = ch;
        event.sample_count = state.clip_count;
        event.peak = state.clip_peak;
        result_.clipping_events.push_back(event);
    }
    state.clip_open = false;
    state.clip_count = 0;
}

// ---------------------------------------------------------------- 块处理

void AudioQcAnalyzer::ProcessBlock(double end_seconds) {
    const int n = block_samples_;
    const int margin = interp_margin_;
    const int64_t base = sample_index_ - n;   // 窗口首样本的绝对序号
    const int ring = ring_size_;

    // ---- 1) 响度: K 加权后按声道加权求均方 ----
    double weighted_power = 0.0;
    if (options_.enable_loudness) {
        for (int ch = 0; ch < channel_count_; ++ch) {
            const auto& buf = channels_[static_cast<size_t>(ch)].kw_ring;
            double sum_sq = 0.0;
            int idx = static_cast<int>(base % ring);
            if (idx < 0) idx += ring;
            for (int k = 0; k < n; ++k) {
                const double v = buf[static_cast<size_t>(idx)];
                sum_sq += v * v;
                if (++idx == ring) idx = 0;
            }
            const double w = (ch < static_cast<int>(result_.channels.size()))
                                 ? result_.channels[static_cast<size_t>(ch)].weight
                                 : 1.0;
            weighted_power += w * (sum_sq / static_cast<double>(n));
        }
    }

    // ---- 2) 电平: 窗口 RMS + 局部峰值（多扫 margin 个历史样本供真峰值定界）----
    double total_sum_sq = 0.0;
    double block_peak = 0.0;
    for (int ch = 0; ch < channel_count_; ++ch) {
        const auto& buf = channels_[static_cast<size_t>(ch)].raw_ring;
        double sum_sq = 0.0;
        int idx = static_cast<int>((base - margin) % ring);
        if (idx < 0) idx += ring;
        for (int k = 0; k < n + margin; ++k) {
            const double v = buf[static_cast<size_t>(idx)];
            const double a = std::abs(v);
            if (a > block_peak) block_peak = a;
            if (k >= margin) sum_sq += v * v;
            if (++idx == ring) idx = 0;
        }
        total_sum_sq += sum_sq;
    }
    const double rms_ms = total_sum_sq / static_cast<double>(n * std::max(1, channel_count_));

    // ---- 3) 真峰值（4× 过采样，仅"可能刷新记录"的块才做）----
    double block_true_peak = block_peak;
    if (options_.enable_true_peak && block_peak * interp_abs_sum_ > running_true_peak_) {
        double candidate = 0.0;
        for (int ch = 0; ch < channel_count_; ++ch) {
            const double tp = ComputeTruePeak(ch);
            if (tp > candidate) candidate = tp;
            if (ch < static_cast<int>(result_.channels.size()) &&
                tp > 0.0 &&
                model::AmplitudeToDb(tp) > result_.channels[static_cast<size_t>(ch)].true_peak_dbtp) {
                result_.channels[static_cast<size_t>(ch)].true_peak_dbtp = model::AmplitudeToDb(tp);
            }
        }
        if (candidate > running_true_peak_) running_true_peak_ = candidate;
        block_true_peak = candidate;
    }

    // ---- 4) 声道相关性（取所有声道对中最差的一对）----
    double corr = 1.0;
    int corr_a = -1;
    int corr_b = -1;
    if (options_.enable_correlation && channel_count_ >= 2) {
        // 长窗口按步长抽稀，把 O(N·C²) 的开销压到每对 ~4096 个样本
        const int stride = std::max(1, n / 4096);
        const int used = (n + stride - 1) / stride;
        const double inv_n = 1.0 / static_cast<double>(used);
        std::vector<double> s1(static_cast<size_t>(channel_count_), 0.0);
        std::vector<double> s2(static_cast<size_t>(channel_count_), 0.0);

        for (int ch = 0; ch < channel_count_; ++ch) {
            const auto& buf = channels_[static_cast<size_t>(ch)].raw_ring;
            int idx = static_cast<int>(base % ring);
            if (idx < 0) idx += ring;
            double acc1 = 0.0;
            double acc2 = 0.0;
            for (int k = 0; k < n; k += stride) {
                const double v = buf[static_cast<size_t>(idx)];
                acc1 += v;
                acc2 += v * v;
                for (int s = 0; s < stride && (k + s) < n; ++s) {
                    if (++idx == ring) idx = 0;
                }
            }
            s1[static_cast<size_t>(ch)] = acc1;
            s2[static_cast<size_t>(ch)] = acc2;
        }

        bool first = true;
        for (int a = 0; a < channel_count_; ++a) {
            const double var_a = s2[static_cast<size_t>(a)] * inv_n -
                                 std::pow(s1[static_cast<size_t>(a)] * inv_n, 2.0);
            if (var_a <= 1e-12) continue;   // 空声道不参与
            for (int b = a + 1; b < channel_count_; ++b) {
                const double var_b = s2[static_cast<size_t>(b)] * inv_n -
                                     std::pow(s1[static_cast<size_t>(b)] * inv_n, 2.0);
                if (var_b <= 1e-12) continue;

                const auto& buf_a = channels_[static_cast<size_t>(a)].raw_ring;
                const auto& buf_b = channels_[static_cast<size_t>(b)].raw_ring;
                int idx_a = static_cast<int>(base % ring);
                if (idx_a < 0) idx_a += ring;
                int idx_b = idx_a;
                double sxy = 0.0;
                for (int k = 0; k < n; k += stride) {
                    sxy += static_cast<double>(buf_a[static_cast<size_t>(idx_a)]) *
                           static_cast<double>(buf_b[static_cast<size_t>(idx_b)]);
                    for (int s = 0; s < stride && (k + s) < n; ++s) {
                        if (++idx_a == ring) idx_a = 0;
                        if (++idx_b == ring) idx_b = 0;
                    }
                }
                const double cov = sxy * inv_n -
                                   (s1[static_cast<size_t>(a)] * inv_n) *
                                       (s1[static_cast<size_t>(b)] * inv_n);
                const double r = std::clamp(cov / std::sqrt(var_a * var_b), -1.0, 1.0);
                if (first || r < corr) {
                    corr = r;
                    corr_a = a;
                    corr_b = b;
                    first = false;
                }
            }
        }
        if (corr_a >= 0) {
            result_.correlation_available = true;
            correlation_sum_ += corr;
            if (corr < correlation_min_) correlation_min_ = corr;
            ++correlation_blocks_;
            if (corr < options_.out_of_phase_threshold) ++out_of_phase_blocks_;
        }
    }

    // ---- 5) 组装曲线点 ----
    model::LoudnessPoint point;
    point.timestamp_seconds = end_seconds;
    point.rms_dbfs = model::PowerToDb(rms_ms);
    point.sample_peak_dbfs = model::AmplitudeToDb(block_peak);
    point.true_peak_dbtp = model::AmplitudeToDb(block_true_peak);
    point.correlation = corr;
    point.correlation_ch0 = corr_a;
    point.correlation_ch1 = corr_b;
    point.silent = point.rms_dbfs < options_.silence_threshold_dbfs;

    if (options_.enable_loudness) {
        point.momentary_lufs = model::PowerToLufs(weighted_power);
        block_powers_.push_back(weighted_power);
        if (point.momentary_lufs > momentary_max_) momentary_max_ = point.momentary_lufs;

        // 短期响度: 3 s 窗口 = 最近 30 个块（100 ms 步进）
        const size_t window_blocks = std::max<size_t>(
            1, static_cast<size_t>(std::llround(kShortTermSeconds / kHopSeconds)));
        if (short_term_window_.size() != window_blocks) {
            short_term_window_.assign(window_blocks, 0.0);
            short_term_pos_ = 0;
            short_term_sum_ = 0.0;
        }
        short_term_sum_ -= short_term_window_[short_term_pos_];
        short_term_window_[short_term_pos_] = weighted_power;
        short_term_sum_ += weighted_power;
        short_term_pos_ = (short_term_pos_ + 1) % window_blocks;

        if (static_cast<int64_t>(block_powers_.size()) >= static_cast<int64_t>(window_blocks)) {
            const double st_power = short_term_sum_ / static_cast<double>(window_blocks);
            point.short_term_lufs = model::PowerToLufs(st_power);
            short_term_values_.push_back(point.short_term_lufs);
            if (point.short_term_lufs > short_term_max_) short_term_max_ = point.short_term_lufs;
            if (point.short_term_lufs < short_term_min_) short_term_min_ = point.short_term_lufs;
        }

        // 累计积分响度: 增量近似（门限随累计均值前移，已入选的块不回退）
        // 精确的双门限结果在 Finish() 里做两遍计算，此处只供曲线展示
        const double lufs = point.momentary_lufs;
        if (lufs >= options_.absolute_gate_lufs) {
            abs_gated_sum_ += weighted_power;
            ++abs_gated_count_;
            const double ref =
                model::PowerToLufs(abs_gated_sum_ / static_cast<double>(abs_gated_count_)) +
                options_.relative_gate_lu;
            if (lufs >= ref) {
                rel_gated_sum_ += weighted_power;
                ++rel_gated_count_;
            }
        }
        if (rel_gated_count_ > 0) {
            point.integrated_lufs =
                model::PowerToLufs(rel_gated_sum_ / static_cast<double>(rel_gated_count_));
        }
    }

    // ---- 6) 静音段归并 ----
    UpdateSilence(point);

    // ---- 7) 曲线抽稀 ----
    if (blocks_emitted_ % point_stride_ == 0) {
        result_.loudness_points.push_back(point);
        DecimatePointsIfNeeded();
    }
    ++blocks_emitted_;
}

void AudioQcAnalyzer::DecimatePointsIfNeeded() {
    if (static_cast<int>(result_.loudness_points.size()) < options_.max_loudness_points) return;
    // 达到上限时对折抽稀：保留偶数下标，之后每 2 个块才记一个点（时间轴仍均匀）
    std::vector<model::LoudnessPoint> compacted;
    compacted.reserve(result_.loudness_points.size() / 2 + 1);
    for (size_t i = 0; i < result_.loudness_points.size(); i += 2) {
        compacted.push_back(result_.loudness_points[i]);
    }
    result_.loudness_points.swap(compacted);
    point_stride_ *= 2;
    result_.loudness_points_decimated = true;
}

void AudioQcAnalyzer::UpdateSilence(const model::LoudnessPoint& point) {
    if (point.silent) {
        if (!silence_open_) {
            silence_open_ = true;
            // 块是 400 ms 窗口，窗口"完全静音"要等到静音开始 400 ms 后才会被判定，
            // 因此回退 (block - hop) = 300 ms 作为起点估计；第一个块则直接取 0。
            silence_start_ =
                (blocks_emitted_ == 0)
                    ? 0.0
                    : std::max(0.0, point.timestamp_seconds - (kBlockSeconds - kHopSeconds));
            silence_acc_dbfs_ = 0.0;
            silence_acc_blocks_ = 0;
        }
        silence_acc_dbfs_ += point.rms_dbfs;
        ++silence_acc_blocks_;
        return;
    }
    if (!silence_open_) return;

    const double end = point.timestamp_seconds;
    if (end - silence_start_ >= options_.min_silence_seconds &&
        static_cast<int>(result_.silence_ranges.size()) < options_.max_silence_ranges) {
        model::AudioSilenceRange range;
        range.start_seconds = silence_start_;
        range.end_seconds = end;
        range.duration_seconds = end - silence_start_;
        range.rms_dbfs = silence_acc_blocks_ > 0 ? silence_acc_dbfs_ / silence_acc_blocks_
                                                  : model::kSilenceLevelDb;
        result_.silence_ranges.push_back(range);
    }
    silence_open_ = false;
}

double AudioQcAnalyzer::ComputeTruePeak(int ch) const {
    const auto& buf = channels_[static_cast<size_t>(ch)].raw_ring;
    const int n = block_samples_;
    const int margin = interp_margin_;
    const int ring = ring_size_;
    const size_t size = static_cast<size_t>(n + margin);

    // 复制到线性暂存区，避免内层循环做取模
    std::vector<float>& scratch = const_cast<std::vector<float>&>(scratch_);
    if (scratch.size() < size) scratch.assign(size, 0.0f);
    int idx = static_cast<int>((sample_index_ - n - margin) % ring);
    if (idx < 0) idx += ring;
    for (size_t k = 0; k < size; ++k) {
        scratch[k] = buf[static_cast<size_t>(idx)];
        if (++idx == ring) idx = 0;
    }

    double max_value = 0.0;
    for (int i = 0; i < n; ++i) {
        const float* x = scratch.data() + static_cast<size_t>(i + margin);
        for (int p = 0; p < kOversampleFactor; ++p) {
            const std::vector<double>& coeffs = interp_phase_[p];
            double acc = 0.0;
            for (size_t m = 0; m < coeffs.size(); ++m) {
                acc += coeffs[m] * static_cast<double>(x[-static_cast<ptrdiff_t>(m)]);
            }
            const double a = std::abs(acc);
            if (a > max_value) max_value = a;
        }
    }
    return max_value;
}

// ---------------------------------------------------------------- 收尾

const model::AudioQcResult& AudioQcAnalyzer::Finish() {
    if (finished_) return result_;
    finished_ = true;

    if (!stream_info_set_ || channel_count_ <= 0) {
        result_.analyzed = false;
        return result_;
    }

    // 收尾尚未关闭的削波段 / 静音段
    for (int ch = 0; ch < channel_count_; ++ch) {
        CloseClipEvent(ch, channels_[static_cast<size_t>(ch)].clip_last);
    }
    if (silence_open_) {
        const double end = clock_seconds_;
        if (end - silence_start_ >= options_.min_silence_seconds &&
            static_cast<int>(result_.silence_ranges.size()) < options_.max_silence_ranges) {
            model::AudioSilenceRange range;
            range.start_seconds = silence_start_;
            range.end_seconds = end;
            range.duration_seconds = end - silence_start_;
            range.rms_dbfs = silence_acc_blocks_ > 0 ? silence_acc_dbfs_ / silence_acc_blocks_
                                                     : model::kSilenceLevelDb;
            result_.silence_ranges.push_back(range);
        }
        silence_open_ = false;
    }

    // ---- 声道统计 ----
    double sum_mean_sq = 0.0;
    double max_dc = 0.0;
    bool any_signal = false;
    for (size_t ch = 0; ch < channels_.size(); ++ch) {
        const ChannelState& state = channels_[ch];
        if (ch >= result_.channels.size()) {
            model::AudioChannelStat stat;
            stat.index = static_cast<int>(ch);
            stat.name = "Ch" + std::to_string(ch + 1);
            result_.channels.push_back(stat);
        }
        model::AudioChannelStat& stat = result_.channels[ch];
        stat.sample_count = state.samples;
        stat.peak = state.peak;
        stat.peak_dbfs = model::AmplitudeToDb(state.peak);
        stat.clipping_samples = state.clip_samples;
        if (state.samples > 0) {
            const double mean_sq = state.sum_sq / static_cast<double>(state.samples);
            stat.rms_dbfs = model::PowerToDb(mean_sq);
            stat.dc_offset = state.sum / static_cast<double>(state.samples);
            stat.dc_offset_dbfs = model::AmplitudeToDb(std::abs(stat.dc_offset));
            sum_mean_sq += mean_sq;
            if (state.peak > 0.0) any_signal = true;
        }
        stat.silent = stat.rms_dbfs < options_.silence_threshold_dbfs;
        if (!options_.enable_true_peak) stat.true_peak_dbtp = stat.peak_dbfs;
        if (std::abs(stat.dc_offset) > max_dc) max_dc = std::abs(stat.dc_offset);
    }

    result_.total_samples = sample_index_;
    result_.duration_seconds =
        static_cast<double>(sample_index_) / static_cast<double>(sample_rate_);
    result_.sample_peak = running_sample_peak_;
    result_.sample_peak_dbfs = model::AmplitudeToDb(running_sample_peak_);
    result_.true_peak_dbtp = options_.enable_true_peak ? model::AmplitudeToDb(running_true_peak_)
                                                       : result_.sample_peak_dbfs;
    result_.rms_dbfs = channels_.empty()
                           ? model::kSilenceLevelDb
                           : model::PowerToDb(sum_mean_sq / static_cast<double>(channels_.size()));
    result_.max_dc_offset = max_dc;
    result_.max_dc_offset_dbfs = model::AmplitudeToDb(max_dc);
    result_.momentary_max_lufs = momentary_max_;
    result_.short_term_max_lufs = short_term_max_;
    result_.short_term_min_lufs = (short_term_min_ > 50.0) ? short_term_max_ : short_term_min_;
    if (result_.metadata.stream_duration_seconds <= 0.0) {
        result_.metadata.stream_duration_seconds = result_.duration_seconds;
    }

    // ---- 响度（精确双门限）----
    result_.block_count = static_cast<int64_t>(block_powers_.size());
    FinalizeIntegrated();
    FinalizeLoudnessRange();

    // ---- 相关性 ----
    if (correlation_blocks_ > 0) {
        result_.correlation_available = true;
        result_.correlation_mean = correlation_sum_ / static_cast<double>(correlation_blocks_);
        result_.correlation_min = correlation_min_;
        result_.out_of_phase_ratio =
            static_cast<double>(out_of_phase_blocks_) / static_cast<double>(correlation_blocks_);
    }

    // ---- 静音占比 ----
    double silence_seconds = 0.0;
    for (const auto& range : result_.silence_ranges) {
        silence_seconds += range.duration_seconds;
        if (range.duration_seconds > result_.longest_silence_seconds) {
            result_.longest_silence_seconds = range.duration_seconds;
        }
    }
    if (result_.duration_seconds > 0.0) {
        result_.silence_ratio = std::min(1.0, silence_seconds / result_.duration_seconds);
    }

    FinalizeMetadata();

    if (!any_signal) {
        AddNote("整轨无信号（数字静音），响度与真峰值以 -120 表示。");
    }
    if (result_.loudness_points_decimated) {
        AddNote("响度曲线点数超过上限，已按 2:1 抽稀（统计值仍基于全部样本）。");
    }
    if (!options_.enable_true_peak) {
        AddNote("未启用真峰值检测，dBTP 回落为采样峰值 dBFS。");
    }
    if (result_.block_count > 0 &&
        result_.duration_seconds < kShortTermSeconds) {
        AddNote("音频时长不足 3 秒，短期响度(S)与 LRA 不可用。");
    }
    return result_;
}

void AudioQcAnalyzer::FinalizeIntegrated() {
    result_.integrated_lufs = model::kSilenceLufs;
    if (block_powers_.empty()) return;

    // 第一遍: 绝对门限
    double abs_sum = 0.0;
    int abs_count = 0;
    bool all_zero = true;
    for (double p : block_powers_) {
        if (p > 0.0) all_zero = false;
        if (model::PowerToLufs(p) >= options_.absolute_gate_lufs) {
            abs_sum += p;
            ++abs_count;
        }
    }
    if (abs_count == 0) {
        result_.integrated_lufs = all_zero ? model::kSilenceLufs : options_.absolute_gate_lufs;
        return;
    }

    // 第二遍: 相对门限（剔除低于"绝对门限后均值" 10 LU 的块）
    const double abs_mean = abs_sum / static_cast<double>(abs_count);
    const double rel_gate = model::PowerToLufs(abs_mean) + options_.relative_gate_lu;
    double rel_sum = 0.0;
    int rel_count = 0;
    for (double p : block_powers_) {
        const double l = model::PowerToLufs(p);
        if (l >= options_.absolute_gate_lufs && l >= rel_gate) {
            rel_sum += p;
            ++rel_count;
        }
    }
    result_.gated_block_count = rel_count;
    result_.integrated_lufs = (rel_count > 0)
                                  ? model::PowerToLufs(rel_sum / static_cast<double>(rel_count))
                                  : model::PowerToLufs(abs_mean);
}

void AudioQcAnalyzer::FinalizeLoudnessRange() {
    result_.loudness_range_lu = 0.0;
    if (short_term_values_.empty()) return;

    // EBU Tech 3341: 绝对门限 -70 LUFS -> 相对门限（低于均值 20 LU 剔除）-> P95 - P10
    std::vector<double> values;
    values.reserve(short_term_values_.size());
    for (double v : short_term_values_) {
        if (v >= options_.absolute_gate_lufs) values.push_back(v);
    }
    if (values.empty()) return;

    double power_sum = 0.0;
    for (double v : values) power_sum += model::DbToPower(v);
    const double mean_lufs = model::PowerToLufs(power_sum / static_cast<double>(values.size()));
    const double rel_gate = mean_lufs + options_.lra_relative_gate_lu;

    std::vector<double> gated;
    gated.reserve(values.size());
    for (double v : values) {
        if (v >= rel_gate) gated.push_back(v);
    }
    if (gated.size() < 2) return;
    std::sort(gated.begin(), gated.end());

    const auto percentile = [](const std::vector<double>& sorted, double p) {
        const double pos = p * (static_cast<double>(sorted.size()) - 1.0);
        const size_t lo = static_cast<size_t>(std::floor(pos));
        const size_t hi = std::min(lo + 1, sorted.size() - 1);
        const double frac = pos - static_cast<double>(lo);
        return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
    };

    result_.loudness_range_low_lufs = percentile(gated, 0.10);
    result_.loudness_range_high_lufs = percentile(gated, 0.95);
    result_.loudness_range_lu = result_.loudness_range_high_lufs - result_.loudness_range_low_lufs;
}

void AudioQcAnalyzer::FinalizeMetadata() {
    auto& meta = result_.metadata;
    meta.inconsistencies.clear();

    if (meta.sample_rate <= 0) {
        meta.inconsistencies.push_back("音频采样率缺失");
    }
    if (meta.channels <= 0) {
        meta.inconsistencies.push_back("声道数缺失");
    } else if (meta.channels > 2 && !meta.layout_confirmed) {
        meta.inconsistencies.push_back("多声道（" + std::to_string(meta.channels) +
                                      " 声道）但容器未明确标注声道位置");
    }
    if (meta.sample_format.empty() && meta.bits_per_sample <= 0) {
        meta.inconsistencies.push_back("采样格式/位深未标注");
    }
    if (meta.container_duration_seconds > 0.0 && meta.stream_duration_seconds > 0.0 &&
        std::abs(meta.container_delta_seconds) > 0.5) {
        meta.inconsistencies.push_back("音频流时长与容器时长相差 " +
                                      Fixed(meta.container_delta_seconds, 2) + " 秒");
    }
    if (meta.has_video && meta.video_duration_seconds > 0.0 &&
        meta.stream_duration_seconds > 0.0 && std::abs(meta.video_delta_seconds) > 0.5) {
        meta.inconsistencies.push_back("音频流与视频流时长相差 " +
                                      Fixed(meta.video_delta_seconds, 2) + " 秒");
    }
    if (meta.channel_layout.empty()) {
        meta.channel_layout = DescribeLayout(meta.channels, {});
    }
}

void AudioQcAnalyzer::AddNote(const std::string& note) { result_.notes.push_back(note); }

}  // namespace analyzer
}  // namespace videoeye
