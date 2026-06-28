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
    AudioVisualizationResult Process(const int16_t* samples, int sample_count,
                                     int sample_rate, int channels) const;
};

} // namespace player
} // namespace videoeye
