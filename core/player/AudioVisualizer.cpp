#include "AudioVisualizer.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace videoeye {
namespace player {

namespace {
constexpr int kWaveformPointCount = 256;
constexpr int kSpectrumBinCount = 64;
constexpr double kPi = 3.14159265358979323846;
} // namespace

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

    // 波形数据
    const int waveform_points = std::min(kWaveformPointCount, frame_count);
    result.waveform_points.reserve(waveform_points);
    for (int i = 0; i < waveform_points; ++i) {
        const int frame_index = (i * frame_count) / waveform_points;
        const int sample_index = frame_index * channels;
        double mono = 0.0;
        for (int ch = 0; ch < channels; ++ch) {
            mono += static_cast<double>(samples[sample_index + ch]) / 32768.0;
        }
        mono /= static_cast<double>(channels);
        result.waveform_points.push_back(std::clamp(mono, -1.0, 1.0));
    }

    // 频谱数据 (DFT)
    const int fft_size = std::min(frame_count, 512);
    if (fft_size >= 8) {
        result.spectrum_bins.reserve(kSpectrumBinCount);
        for (int bin = 0; bin < kSpectrumBinCount; ++bin) {
            const double normalized_bin = static_cast<double>(bin) / static_cast<double>(kSpectrumBinCount);
            const int k = std::min(fft_size / 2 - 1, std::max(0, static_cast<int>(normalized_bin * (fft_size / 2 - 1))));
            std::complex<double> acc(0.0, 0.0);
            for (int n = 0; n < fft_size; ++n) {
                const int sample_index = n * channels;
                double mono = 0.0;
                for (int ch = 0; ch < channels; ++ch) {
                    mono += static_cast<double>(samples[sample_index + ch]) / 32768.0;
                }
                mono /= static_cast<double>(channels);
                const double window = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (fft_size - 1)));
                mono *= window;
                const double angle = -2.0 * kPi * static_cast<double>(k * n) / static_cast<double>(fft_size);
                acc += std::complex<double>(mono * std::cos(angle), mono * std::sin(angle));
            }
            const double magnitude = std::abs(acc) / static_cast<double>(fft_size) * 2.0;
            result.spectrum_bins.push_back(magnitude);
        }
    }

    // Peak dBFS
    int16_t max_abs = 0;
    for (int i = 0; i < sample_count; ++i) {
        int16_t abs_val = std::abs(samples[i]);
        if (abs_val > max_abs) max_abs = abs_val;
    }
    if (max_abs > 0) {
        result.peak_dbfs = 20.0 * std::log10(static_cast<double>(max_abs) / 32768.0);
    } else {
        result.peak_dbfs = -70.0;
    }

    // Momentary LUFS (简化 EBU R128)
    double sum_sq = 0.0;
    for (int i = 0; i < frame_count; ++i) {
        double mono = 0.0;
        for (int ch = 0; ch < channels; ++ch) {
            mono += static_cast<double>(samples[i * channels + ch]) / 32768.0;
        }
        mono /= static_cast<double>(channels);
        sum_sq += mono * mono;
    }
    double mean_sq = sum_sq / static_cast<double>(frame_count);
    if (mean_sq > 1e-10) {
        result.loudness_momentary_lufs = -0.691 + 10.0 * std::log10(mean_sq);
    } else {
        result.loudness_momentary_lufs = -70.0;
    }

    // True Peak dBTP (4x 过采样)
    double true_peak = 0.0;
    for (int i = 0; i < frame_count - 1; ++i) {
        double s0 = 0.0, s1 = 0.0;
        for (int ch = 0; ch < channels; ++ch) {
            s0 += static_cast<double>(samples[i * channels + ch]) / 32768.0;
            s1 += static_cast<double>(samples[(i + 1) * channels + ch]) / 32768.0;
        }
        s0 /= channels;
        s1 /= channels;
        for (int k = 0; k < 4; ++k) {
            double t = k / 4.0;
            double interp = s0 * (1.0 - t) + s1 * t;
            double abs_interp = std::abs(interp);
            if (abs_interp > true_peak) true_peak = abs_interp;
        }
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
