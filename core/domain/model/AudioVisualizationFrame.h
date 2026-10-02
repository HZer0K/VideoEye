#pragma once

#include <vector>

namespace videoeye {
namespace model {

struct AudioVisualizationFrame {
    int index = 0;
    double timestamp_seconds = 0.0;
    double level = 0.0;
    int sample_rate = 0;
    int channels = 0;
    std::vector<double> waveform_points;
    std::vector<double> spectrum_bins;
    // 响度指标
    double loudness_momentary_lufs = -70.0;  // 400ms 窗口 LUFS
    double peak_dbfs = -70.0;                // 峰值 dBFS
    double true_peak_dbtp = -70.0;           // 真实峰值 dBTP
};

} // namespace model
} // namespace videoeye
