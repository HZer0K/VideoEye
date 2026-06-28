#include "AudioVisualizer.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace videoeye {
namespace player {

namespace {
constexpr int kWaveformPointCount = 256;
constexpr int kSpectrumBinCount = 64;
constexpr double kPi = 3.14159265358979323846;

// 计算不小于 n 的最小 2 的幂
int NextPow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// 位反转置换索引
int BitReverse(int x, int log2n) {
    int result = 0;
    for (int i = 0; i < log2n; ++i) {
        result = (result << 1) | (x & 1);
        x >>= 1;
    }
    return result;
}

// 全局 FFT 表缓存 (线程安全)
std::mutex g_tables_mutex;
std::unordered_map<int, AudioVisualizer::FftTables> g_tables_cache;
} // namespace

AudioVisualizer::AudioVisualizer() = default;

AudioVisualizer::FftTables& AudioVisualizer::GetTables(int fft_size) {
    std::lock_guard<std::mutex> lock(g_tables_mutex);
    auto it = g_tables_cache.find(fft_size);
    if (it != g_tables_cache.end()) {
        return it->second;
    }

    FftTables& t = g_tables_cache[fft_size];
    t.size = fft_size;
    const int N = fft_size;
    const int log2N = static_cast<int>(std::log2(N));

    // Hann 窗
    t.hann_window.resize(N);
    for (int n = 0; n < N; ++n) {
        t.hann_window[n] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (N - 1)));
    }

    // 旋转因子 (twiddle factors): e^(-j*2*pi*k/N) for k=0..N/2-1
    const int half_N = N / 2;
    t.twiddle_cos.resize(half_N);
    t.twiddle_sin.resize(half_N);
    for (int k = 0; k < half_N; ++k) {
        const double angle = -2.0 * kPi * k / N;
        t.twiddle_cos[k] = std::cos(angle);
        t.twiddle_sin[k] = std::sin(angle);
    }

    // 位反转置换表
    t.bit_reverse.resize(N);
    for (int i = 0; i < N; ++i) {
        t.bit_reverse[i] = BitReverse(i, log2N);
    }

    return t;
}

void AudioVisualizer::FftInPlace(std::vector<double>& re, std::vector<double>& im,
                                  const FftTables& tables) {
    const int N = tables.size;
    const int log2N = static_cast<int>(std::log2(N));

    // 位反转置换
    for (int i = 0; i < N; ++i) {
        const int j = tables.bit_reverse[i];
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    // Cooley-Tukey 蝶形运算
    for (int stage = 1; stage <= log2N; ++stage) {
        const int m = 1 << stage;        // 当前子 DFT 大小
        const int half_m = m >> 1;
        const int step = N / m;           // twiddle 步长

        for (int k = 0; k < N; k += m) {
            for (int j = 0; j < half_m; ++j) {
                const int twiddle_idx = j * step;
                const double wr = tables.twiddle_cos[twiddle_idx];
                const double wi = tables.twiddle_sin[twiddle_idx];

                const int idx_even = k + j;
                const int idx_odd = k + j + half_m;

                // 蝶形: t = W * odd
                const double tr = wr * re[idx_odd] - wi * im[idx_odd];
                const double ti = wr * im[idx_odd] + wi * re[idx_odd];

                re[idx_odd] = re[idx_even] - tr;
                im[idx_odd] = im[idx_even] - ti;
                re[idx_even] += tr;
                im[idx_even] += ti;
            }
        }
    }
}

AudioVisualizationResult AudioVisualizer::Process(const int16_t* samples, int sample_count,
                                                   int sample_rate, int channels) const {
    AudioVisualizationResult result;
    if (!samples || sample_count <= 0 || channels <= 0) {
        return result;
    }

    const int frame_count = sample_count / channels;
    if (frame_count <= 0) {
        return result;
    }

    // ===== 1. 一次性混缩为 mono 浮点缓冲 =====
    std::vector<double> mono(frame_count);
    const double inv_channels = 1.0 / static_cast<double>(channels);
    for (int i = 0; i < frame_count; ++i) {
        double sum = 0.0;
        const int base = i * channels;
        for (int ch = 0; ch < channels; ++ch) {
            sum += static_cast<double>(samples[base + ch]);
        }
        mono[i] = sum * inv_channels / 32768.0;
    }

    // ===== 2. 波形抽取 (从 mono 缓冲直接采样) =====
    const int waveform_points = std::min(kWaveformPointCount, frame_count);
    result.waveform_points.resize(waveform_points);
    for (int i = 0; i < waveform_points; ++i) {
        const int idx = (i * frame_count) / waveform_points;
        result.waveform_points[i] = std::clamp(mono[idx], -1.0, 1.0);
    }

    // ===== 3. FFT 频谱 (Cooley-Tukey, 预计算表) =====
    // 找到不超过 frame_count 的最大 2 的幂, 最大 kMaxFftSize
    int fft_size = NextPow2(frame_count);
    if (fft_size > kMaxFftSize) fft_size = kMaxFftSize;
    if (fft_size > frame_count) fft_size >>= 1;

    if (fft_size >= 8) {
        const auto& tables = GetTables(fft_size);

        // 填充 FFT 输入: mono 样本 × Hann 窗, 零填充到 fft_size
        std::vector<double> re(fft_size, 0.0);
        std::vector<double> im(fft_size, 0.0);
        for (int n = 0; n < fft_size; ++n) {
            re[n] = mono[n] * tables.hann_window[n];
        }

        // In-place FFT
        FftInPlace(re, im, tables);

        // 计算幅度谱并映射到 kSpectrumBinCount 个显示柱
        // 使用对数频率映射 (更接近听觉感知)
        result.spectrum_bins.resize(kSpectrumBinCount);
        const int half_fft = fft_size / 2;
        const double log_half = std::log(static_cast<double>(half_fft));

        for (int bin = 0; bin < kSpectrumBinCount; ++bin) {
            // 对数映射: bin -> FFT bin index
            const double frac = static_cast<double>(bin) / static_cast<double>(kSpectrumBinCount);
            const int k = std::max(0, std::min(half_fft - 1,
                                               static_cast<int>(std::exp(frac * log_half))));

            // 取相邻几个 bin 的平均值以平滑
            const int k_lo = std::max(0, k - 1);
            const int k_hi = std::min(half_fft - 1, k + 1);
            double sum_mag = 0.0;
            int count = 0;
            for (int kk = k_lo; kk <= k_hi; ++kk) {
                sum_mag += std::sqrt(re[kk] * re[kk] + im[kk] * im[kk]);
                count++;
            }
            result.spectrum_bins[bin] = (sum_mag / count) / fft_size * 2.0;
        }
    }

    // ===== 4. Peak dBFS (从 mono 缓冲) =====
    double max_abs = 0.0;
    for (int i = 0; i < frame_count; ++i) {
        const double a = std::abs(mono[i]);
        if (a > max_abs) max_abs = a;
    }
    if (max_abs > 1e-10) {
        result.peak_dbfs = 20.0 * std::log10(max_abs);
    } else {
        result.peak_dbfs = -70.0;
    }

    // ===== 5. Momentary LUFS (从 mono 缓冲) =====
    double sum_sq = 0.0;
    for (int i = 0; i < frame_count; ++i) {
        sum_sq += mono[i] * mono[i];
    }
    const double mean_sq = sum_sq / static_cast<double>(frame_count);
    if (mean_sq > 1e-10) {
        result.loudness_momentary_lufs = -0.691 + 10.0 * std::log10(mean_sq);
    } else {
        result.loudness_momentary_lufs = -70.0;
    }

    // ===== 6. True Peak dBTP (4x 过采样, 从 mono 缓冲) =====
    double true_peak = 0.0;
    for (int i = 0; i < frame_count - 1; ++i) {
        const double s0 = mono[i];
        const double s1 = mono[i + 1];
        // 4x 线性插值, 找峰值
        // 优化: 只需检查端点和中间极值
        const double a0 = std::abs(s0);
        const double a1 = std::abs(0.25 * s0 + 0.75 * s1);
        const double a2 = std::abs(0.5 * s0 + 0.5 * s1);
        const double a3 = std::abs(0.75 * s0 + 0.25 * s1);
        const double a4 = std::abs(s1);
        const double local_max = std::max({a0, a1, a2, a3, a4});
        if (local_max > true_peak) true_peak = local_max;
    }
    if (true_peak > 1e-10) {
        result.true_peak_dbtp = 20.0 * std::log10(true_peak);
    } else {
        result.true_peak_dbtp = -70.0;
    }

    return result;
}

} // namespace player
} // namespace videoeye
