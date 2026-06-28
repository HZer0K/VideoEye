#pragma once

#include <cstdint>
#include <vector>

namespace videoeye {
namespace player {

// 音频可视化计算结果
struct AudioVisualizationResult {
    std::vector<double> waveform_points;
    std::vector<double> spectrum_bins;
    double peak_dbfs = -70.0;
    double loudness_momentary_lufs = -70.0;
    double true_peak_dbtp = -70.0;
};

// 音频可视化处理器：从 PCM 样本计算波形、频谱、响度
class AudioVisualizer {
public:
    AudioVisualizer();

    AudioVisualizationResult Process(const int16_t* samples, int sample_count,
                                     int sample_rate, int channels) const;

    // 预计算的 FFT 查找表
    struct FftTables {
        std::vector<double> hann_window;    // Hann 窗系数
        std::vector<double> twiddle_cos;    // 旋转因子实部
        std::vector<double> twiddle_sin;    // 旋转因子虚部
        std::vector<int> bit_reverse;       // 位反转置换表
        int size = 0;                       // FFT 尺寸 (2 的幂)
    };

    // 按需构建并缓存指定尺寸的 FFT 表
    static FftTables& GetTables(int fft_size);

    // In-place radix-2 Cooley-Tukey FFT
    static void FftInPlace(std::vector<double>& re, std::vector<double>& im,
                           const FftTables& tables);

private:
    static constexpr int kMaxFftSize = 512;
};

} // namespace player
} // namespace videoeye
